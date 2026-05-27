#include "sd_recovery.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "ota_config.h"
#include "recovery_config.h"

static const char *TAG = "tr22.sdrec";

static uint8_t s_ota_buf[OTA_HTTP_BUF_SIZE];

static void configure_sd_gpio(gpio_num_t gpio)
{
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    gpio_set_level(gpio, 1);
}

static esp_err_t mount_sdcard(sdmmc_card_t **card_out)
{
    if (card_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    configure_sd_gpio(RECOVERY_SD_CMD_GPIO);
    configure_sd_gpio(RECOVERY_SD_CLK_GPIO);
    configure_sd_gpio(RECOVERY_SD_D0_GPIO);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 0,
    };

    ESP_LOGI(TAG, "mounting SD card at %s using 1-bit SDMMC CMD=%d CLK=%d D0=%d",
             RECOVERY_SD_MOUNT_POINT,
             RECOVERY_SD_CMD_GPIO,
             RECOVERY_SD_CLK_GPIO,
             RECOVERY_SD_D0_GPIO);

    esp_err_t err = esp_vfs_fat_sdmmc_mount(
        RECOVERY_SD_MOUNT_POINT, &host, &slot_config, &mount_config, card_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void unmount_sdcard(sdmmc_card_t *card)
{
    if (card != NULL) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(RECOVERY_SD_MOUNT_POINT, card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
        }
    }
    esp_err_t err = sdmmc_host_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SDMMC host deinit failed: %s", esp_err_to_name(err));
    }
}

static const char *find_app_image(void)
{
    struct stat st;
    if (stat(RECOVERY_SD_APP_PATH, &st) == 0) {
        return RECOVERY_SD_APP_PATH;
    }
    if (stat(RECOVERY_SD_LEGACY_APP_PATH, &st) == 0) {
        return RECOVERY_SD_LEGACY_APP_PATH;
    }
    return NULL;
}

static esp_err_t read_file_desc(FILE *fp, esp_app_desc_t *desc)
{
    if (fp == NULL || desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    esp_image_header_t image_header = {0};
    if (fread(&image_header, 1, sizeof(image_header), fp) != sizeof(image_header)) {
        return ESP_FAIL;
    }
    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "invalid app image magic 0x%02x", image_header.magic);
        return ESP_ERR_INVALID_RESPONSE;
    }

    long desc_offset = (long)(sizeof(esp_image_header_t) +
                              sizeof(esp_image_segment_header_t));
    if (fseek(fp, desc_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(desc, 1, sizeof(*desc), fp) != sizeof(*desc)) {
        return ESP_FAIL;
    }
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "invalid app descriptor magic 0x%08" PRIx32, desc->magic_word);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static bool app_desc_matches_partition(const esp_partition_t *target,
                                       const esp_app_desc_t *file_desc)
{
    esp_app_desc_t partition_desc = {0};
    if (target == NULL || file_desc == NULL) {
        return false;
    }
    if (esp_ota_get_partition_description(target, &partition_desc) != ESP_OK) {
        return false;
    }

    return strcmp(partition_desc.version, file_desc->version) == 0 &&
           strcmp(partition_desc.date, file_desc->date) == 0 &&
           strcmp(partition_desc.time, file_desc->time) == 0 &&
           strcmp(partition_desc.project_name, file_desc->project_name) == 0;
}

static esp_err_t validate_file_size(const char *path,
                                    const esp_partition_t *target,
                                    size_t *size_out)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat %s failed: errno=%d", path, errno);
        return ESP_FAIL;
    }
    if (st.st_size <= 0) {
        ESP_LOGE(TAG, "app image is empty: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((uint64_t)st.st_size > target->size) {
        ESP_LOGE(TAG, "app image too large: %lld > partition %" PRIu32,
                 (long long)st.st_size, target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    *size_out = (size_t)st.st_size;
    return ESP_OK;
}

static esp_err_t write_file_to_partition(FILE *fp,
                                         size_t file_size,
                                         const esp_partition_t *target)
{
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, file_size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s) failed: %s",
                 target->label, esp_err_to_name(err));
        return err;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        esp_ota_abort(handle);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (true) {
        size_t n = fread(s_ota_buf, 1, sizeof(s_ota_buf), fp);
        if (n > 0) {
            err = esp_ota_write(handle, s_ota_buf, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed after %u bytes: %s",
                         (unsigned)total, esp_err_to_name(err));
                esp_ota_abort(handle);
                return err;
            }
            total += n;
        }

        if (n < sizeof(s_ota_buf)) {
            if (ferror(fp)) {
                ESP_LOGE(TAG, "read failed after %u bytes", (unsigned)total);
                esp_ota_abort(handle);
                return ESP_FAIL;
            }
            break;
        }
    }

    if (total != file_size) {
        ESP_LOGE(TAG, "short app image read: %u != %u",
                 (unsigned)total, (unsigned)file_size);
        esp_ota_abort(handle);
        return ESP_FAIL;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "wrote %u bytes to %s", (unsigned)total, target->label);
    return ESP_OK;
}

esp_err_t sd_recovery_install_app(const esp_partition_t *target)
{
    if (target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_card_t *card = NULL;
    esp_err_t err = mount_sdcard(&card);
    if (err != ESP_OK) {
        return err;
    }

    const char *path = find_app_image();
    if (path == NULL) {
        ESP_LOGI(TAG, "no SD app image at %s or %s",
                 RECOVERY_SD_APP_PATH, RECOVERY_SD_LEGACY_APP_PATH);
        unmount_sdcard(card);
        return ESP_ERR_NOT_FOUND;
    }

    size_t file_size = 0;
    err = validate_file_size(path, target, &file_size);
    if (err != ESP_OK) {
        unmount_sdcard(card);
        return err;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "open %s failed: errno=%d", path, errno);
        unmount_sdcard(card);
        return ESP_FAIL;
    }

    esp_app_desc_t file_desc = {0};
    err = read_file_desc(fp, &file_desc);
    if (err != ESP_OK) {
        fclose(fp);
        unmount_sdcard(card);
        return err;
    }

    ESP_LOGI(TAG, "SD app image: %s %s %s (%u bytes)",
             file_desc.project_name, file_desc.date, file_desc.time,
             (unsigned)file_size);

    if (app_desc_matches_partition(target, &file_desc)) {
        esp_err_t boot_err = esp_ota_set_boot_partition(target);
        if (boot_err == ESP_OK) {
            ESP_LOGI(TAG, "target partition already has this image; booting it");
            fclose(fp);
            unmount_sdcard(card);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        /*
         * app_desc only covers the first 256 bytes of the partition; a
         * matching desc with esp_image_verify failure means the body is
         * truncated/corrupted (e.g. from an interrupted prior write). Fall
         * through and rewrite from the SD file.
         */
        ESP_LOGW(TAG, "%s app desc matches SD but image invalid (%s); rewriting",
                 target->label, esp_err_to_name(boot_err));
    }

    err = write_file_to_partition(fp, file_size, target);
    fclose(fp);
    unmount_sdcard(card);
    ESP_RETURN_ON_ERROR(err, TAG, "write SD app");

    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(target), TAG, "set app boot partition");
    ESP_LOGI(TAG, "SD recovery install complete; rebooting into app");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}
