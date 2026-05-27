#include "ota_update.h"
#include "ota_config.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tr22.ota";

static ota_progress_cb_t s_progress_cb = NULL;

void ota_update_set_progress_cb(ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

static void notify(ota_progress_stage_t stage, int percent, const char *detail)
{
    ota_progress_cb_t cb = s_progress_cb;
    if (cb != NULL) {
        cb(stage, percent, detail);
    }
}

/*
 * Every ESP32 app binary begins with:
 *   esp_image_header_t          (24 bytes)
 *   esp_image_segment_header_t  ( 8 bytes)
 *   esp_app_desc_t              (256 bytes)  ← what we actually want
 * So pulling the first 288 bytes off the wire is enough to identify the
 * server's build without trusting any custom response headers.
 */
#define OTA_APP_DESC_PREFIX_SIZE 288

/* Static buffer avoids heap fragmentation during the streaming write loop. */
static char s_buf[OTA_HTTP_BUF_SIZE];

static bool app_desc_equal(const esp_app_desc_t *a, const esp_app_desc_t *b)
{
    return strncmp(a->project_name, b->project_name, sizeof(a->project_name)) == 0 &&
           strncmp(a->version,      b->version,      sizeof(a->version))      == 0 &&
           strncmp(a->date,         b->date,         sizeof(a->date))         == 0 &&
           strncmp(a->time,         b->time,         sizeof(a->time))         == 0;
}

static void log_app_desc(const char *label, const esp_app_desc_t *desc)
{
    ESP_LOGI(TAG, "%s: project=%s version=%s built=%s %s",
             label, desc->project_name, desc->version, desc->date, desc->time);
}

static esp_err_t parse_app_desc_from_prefix(const uint8_t *prefix,
                                            esp_app_desc_t *out)
{
    const esp_image_header_t *img = (const esp_image_header_t *)prefix;
    if (img->magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "bad image header magic 0x%02x", img->magic);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t desc_offset =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    memcpy(out, prefix + desc_offset, sizeof(*out));
    if (out->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "bad app desc magic 0x%08" PRIx32, out->magic_word);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/*
 * Open a GET to the firmware URL and read exactly enough bytes off the wire
 * to cover the embedded esp_app_desc_t. On success the client/connection is
 * left open so the caller can either stream the rest into flash or clean up
 * to abort.
 */
static esp_err_t ota_open_and_read_prefix(esp_http_client_handle_t *client_out,
                                          uint8_t *prefix, size_t prefix_size,
                                          int64_t *content_len_out)
{
    *client_out = NULL;

    esp_http_client_config_t cfg = {
        .url               = OTA_FIRMWARE_URL,
        .timeout_ms        = 30000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return ESP_FAIL;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    (void)esp_http_client_set_header(client, "X-TR22-OTA-Check",    "running");
    (void)esp_http_client_set_header(client, "X-TR22-App-Version", running->version);
    (void)esp_http_client_set_header(client, "X-TR22-App-Date",    running->date);
    (void)esp_http_client_set_header(client, "X-TR22-App-Time",    running->time);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "http GET returned status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_len_out) {
        *content_len_out = content_len;
    }

    size_t read_total = 0;
    while (read_total < prefix_size) {
        int n = esp_http_client_read(client,
                                     (char *)(prefix + read_total),
                                     prefix_size - read_total);
        if (n < 0) {
            ESP_LOGE(TAG, "prefix read error (errno=%d)", errno);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            ESP_LOGE(TAG, "connection closed before %u-byte prefix (read %u)",
                     (unsigned)prefix_size, (unsigned)read_total);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        read_total += (size_t)n;
    }

    *client_out = client;
    return ESP_OK;
}

bool ota_update_server_has_newer_firmware(void)
{
    const esp_app_desc_t *running = esp_app_get_description();
    log_app_desc("running build", running);

    uint8_t prefix[OTA_APP_DESC_PREFIX_SIZE];
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ota_open_and_read_prefix(&client, prefix, sizeof(prefix), NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not read server image prefix: %s — skipping OTA",
                 esp_err_to_name(err));
        return false;
    }

    /* Have what we need; abandon the rest of the body. */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    esp_app_desc_t server_desc;
    if (parse_app_desc_from_prefix(prefix, &server_desc) != ESP_OK) {
        return false;
    }
    log_app_desc("server build ", &server_desc);

    bool same = app_desc_equal(running, &server_desc);
    ESP_LOGI(TAG, "server firmware %s running app",
             same ? "matches" : "differs from");
    return !same;
}

esp_err_t ota_update_check_and_apply(void)
{
    return ota_update_check_and_apply_to_partition(
        esp_ota_get_next_update_partition(NULL));
}

esp_err_t ota_update_check_and_apply_to_partition(const esp_partition_t *update_part)
{
    ESP_LOGI(TAG, "checking for firmware at: %s", OTA_FIRMWARE_URL);
    notify(OTA_PROGRESS_CHECKING, -1, NULL);

    if (update_part == NULL) {
        ESP_LOGE(TAG, "no OTA partition supplied — check partition table");
        notify(OTA_PROGRESS_FAILED, -1, "NO PARTITION");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "target partition: %s subtype=0x%02x offset=0x%08" PRIx32,
             update_part->label, update_part->subtype, update_part->address);

    uint8_t prefix[OTA_APP_DESC_PREFIX_SIZE];
    esp_http_client_handle_t client = NULL;
    int64_t content_len = 0;
    esp_err_t err = ota_open_and_read_prefix(&client, prefix, sizeof(prefix), &content_len);
    if (err != ESP_OK) {
        notify(OTA_PROGRESS_FAILED, -1, "NO SERVER");
        return err;
    }
    ESP_LOGI(TAG, "content-length: %" PRId64 " bytes", content_len);

    esp_app_desc_t server_desc;
    if (parse_app_desc_from_prefix(prefix, &server_desc) != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        notify(OTA_PROGRESS_FAILED, -1, "BAD IMAGE");
        return ESP_FAIL;
    }
    log_app_desc("server build", &server_desc);
    notify(OTA_PROGRESS_DOWNLOAD_START, -1, server_desc.version);

    /*
     * If the target partition already holds exactly this image, try to
     * point the bootloader at it without touching flash. esp_ota_set_boot_partition
     * runs esp_image_verify internally, so a corrupted image (e.g. truncated
     * by a prior failed OTA) returns ESP_ERR_OTA_VALIDATE_FAILED here — in
     * that case we fall through and rewrite from the open HTTP stream.
     * esp_ota_get_partition_description only reads the first 256 bytes, so
     * matching app desc alone is not proof the rest of the image is intact.
     */
    esp_app_desc_t target_desc = {0};
    if (esp_ota_get_partition_description(update_part, &target_desc) == ESP_OK &&
        app_desc_equal(&server_desc, &target_desc)) {
        esp_err_t boot_err = esp_ota_set_boot_partition(update_part);
        if (boot_err == ESP_OK) {
            ESP_LOGI(TAG, "%s already holds server build — switching boot without rewrite",
                     update_part->label);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            notify(OTA_PROGRESS_SUCCESS, -1, server_desc.version);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s app desc matches but image invalid (%s); rewriting",
                 update_part->label, esp_err_to_name(boot_err));
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        notify(OTA_PROGRESS_FAILED, -1, "FLASH BUSY");
        return ESP_FAIL;
    }

    /* Write the prefix we already pulled off the wire. */
    err = esp_ota_write(ota_handle, prefix, sizeof(prefix));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write(prefix) failed: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        notify(OTA_PROGRESS_FAILED, -1, "FLASH WRITE");
        return ESP_FAIL;
    }

    int total = (int)sizeof(prefix);
    int next_progress = 128 * 1024; /* log every 128 KB so the loop is visible */
    /*
     * Throttle display updates: each e-paper refresh takes seconds, so emit
     * a progress event only when percent advances by >= 10. content_len can
     * be -1 if the server omits the header, in which case we skip percentages.
     * last_pct_notified starts at 0 because we notify 0% right below; this
     * prevents a duplicate 0% notification on the first loop iteration.
     */
    int last_pct_notified = 0;
    notify(OTA_PROGRESS_DOWNLOADING, 0, NULL);
    while (1) {
        int n = esp_http_client_read(client, s_buf, sizeof(s_buf));
        if (n < 0) {
            ESP_LOGE(TAG, "http read error (errno=%d)", errno);
            notify(OTA_PROGRESS_FAILED, -1, "HTTP READ");
            goto fail;
        }
        if (n > 0) {
            err = esp_ota_write(ota_handle, s_buf, (size_t)n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                notify(OTA_PROGRESS_FAILED, -1, "FLASH WRITE");
                goto fail;
            }
            total += n;
            if (total >= next_progress) {
                ESP_LOGI(TAG, "downloaded %d / %" PRId64 " bytes", total, content_len);
                next_progress += 128 * 1024;
            }
            if (content_len > 0) {
                int pct = (int)((int64_t)total * 100 / content_len);
                if (pct > 100) pct = 100;
                if (pct - last_pct_notified >= 10) {
                    notify(OTA_PROGRESS_DOWNLOADING, pct, NULL);
                    last_pct_notified = pct;
                }
            }
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            ESP_LOGE(TAG, "connection closed early (errno=%d)", errno);
            notify(OTA_PROGRESS_FAILED, -1, "CONN LOST");
            goto fail;
        }
    }

    ESP_LOGI(TAG, "downloaded %d bytes total", total);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    notify(OTA_PROGRESS_INSTALLING, -1, NULL);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        notify(OTA_PROGRESS_FAILED, -1, "VERIFY FAIL");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        notify(OTA_PROGRESS_FAILED, -1, "SET BOOT");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success — rebooting into new firmware");
    notify(OTA_PROGRESS_SUCCESS, -1, server_desc.version);
    vTaskDelay(pdMS_TO_TICKS(100)); /* flush UART TX buffer before reboot */
    esp_restart();
    return ESP_OK;

fail:
    esp_ota_abort(ota_handle);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

esp_err_t ota_update_reboot_to_recovery_if_server_newer(const esp_partition_t *recovery_part)
{
    ESP_LOGI(TAG, "checking server before app start");
    if (!ota_update_server_has_newer_firmware()) {
        ESP_LOGI(TAG, "server firmware is not newer");
        return ESP_OK;
    }

    if (recovery_part == NULL) {
        ESP_LOGE(TAG, "server has newer firmware, but recovery partition is missing");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "new firmware available; rebooting to recovery for safe update");
    esp_err_t err = esp_ota_set_boot_partition(recovery_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(recovery) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}
