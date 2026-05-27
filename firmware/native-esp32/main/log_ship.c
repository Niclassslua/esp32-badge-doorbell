/*
 * log_ship.c — Automatic log shipping for TR22 badge firmware.
 *
 * Architecture:
 *   1. vprintf hook intercepts every ESP_LOGx call, writes to a ring buffer,
 *      and passes through to the original UART vprintf.
 *   2. A background FreeRTOS task drains the ring buffer, builds a JSON
 *      payload, and HTTP-POSTs it to the log server every 5 seconds.
 *   3. Rich device metadata (MAC, chip info, app version, reset reason, heap)
 *      is collected at init and included in every batch.
 *
 * Ring buffer: 8 KB circular, byte-granular.  On overflow the oldest bytes
 * are silently evicted; the count is reported in each JSON batch as
 * "dropped_bytes".
 *
 * Thread safety: a portMUX spinlock guards the ring buffer.  The critical
 * sections are intentionally brief (memcpy only — no allocation, no logging).
 */

#include "log_ship.h"
#include "ota_config.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Tunables ────────────────────────────────────────────────────────────────*/

#ifndef GPIO_DEBUG_MODE
#define GPIO_DEBUG_MODE 0
#endif

#if GPIO_DEBUG_MODE
#define LOG_SHIP_BUF_SIZE       32768u  /* ring buffer size - must be power of 2 */
#define LOG_SHIP_DRAIN_MAX      8192u   /* max bytes per HTTP batch             */
#define LOG_SHIP_TASK_PERIOD_MS 1000u   /* live debugger should feel live       */
#else
#define LOG_SHIP_BUF_SIZE       8192u   /* ring buffer size - must be power of 2 */
#define LOG_SHIP_DRAIN_MAX      4096u   /* max bytes per HTTP batch             */
#define LOG_SHIP_TASK_PERIOD_MS 5000u   /* how often the task wakes             */
#endif

#define LOG_SHIP_BUF_MASK       (LOG_SHIP_BUF_SIZE - 1u)
#define LOG_SHIP_TIMEOUT_MS     3000    /* HTTP POST connect+read timeout       */
#define LOG_LINE_MAX            512u    /* max formatted log line length        */
#define LOG_SHIP_TASK_STACK     6144u   /* FreeRTOS task stack (bytes)          */
#define LOG_MSG_MAX             384u    /* max message field in parsed entry    */
#define LOG_TAG_MAX             48u     /* max tag field in parsed entry        */

static const char *TAG = "tr22.logship";

/* ── Ring buffer ─────────────────────────────────────────────────────────────*/

static uint8_t           s_ringbuf[LOG_SHIP_BUF_SIZE];
static volatile uint32_t s_head          = 0;   /* next write index (unbounded) */
static volatile uint32_t s_tail          = 0;   /* next read  index (unbounded) */
static volatile uint32_t s_dropped_bytes = 0;
static portMUX_TYPE      s_mux           = portMUX_INITIALIZER_UNLOCKED;

static vprintf_like_t s_orig_vprintf = NULL;
static TaskHandle_t   s_ship_task    = NULL;
static uint32_t       s_seq          = 0;       /* monotonic batch counter      */

/* ── Device info cache ───────────────────────────────────────────────────────*/

typedef struct {
    char mac[18];           /* "AA:BB:CC:DD:EE:FF" */
    int  chip_model;
    int  chip_cores;
    int  chip_rev;
    char idf_ver[32];
    char app_version[32];
    char app_date[16];
    char app_time[16];
    int  reset_reason;
} device_info_t;

static device_info_t s_dev = {0};

/* ── Ring buffer ops ─────────────────────────────────────────────────────────*/

static void ringbuf_write(const char *data, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < len; i++) {
        uint32_t used = s_head - s_tail;
        if (used >= LOG_SHIP_BUF_SIZE - 1u) {
            /* Full: evict oldest byte */
            s_tail++;
            s_dropped_bytes++;
        }
        s_ringbuf[s_head & LOG_SHIP_BUF_MASK] = (uint8_t)data[i];
        s_head++;
    }
    portEXIT_CRITICAL(&s_mux);
}

/**
 * Drain up to `max` bytes from the ring buffer into `out`.
 * Returns the number of bytes copied.  Resets dropped_bytes counter.
 */
static size_t ringbuf_drain(char *out, size_t max, uint32_t *dropped_out)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t used = s_head - s_tail;
    size_t   n    = (used < (uint32_t)max) ? (size_t)used : max;

    /* Handle ring-buffer wrap with at most two memcpy calls */
    uint32_t tail_idx  = s_tail & LOG_SHIP_BUF_MASK;
    size_t   part1     = LOG_SHIP_BUF_SIZE - (size_t)tail_idx;
    if (part1 > n) part1 = n;

    memcpy(out, &s_ringbuf[tail_idx], part1);
    if (n > part1) {
        memcpy(out + part1, &s_ringbuf[0], n - part1);
    }

    s_tail         += (uint32_t)n;
    *dropped_out    = s_dropped_bytes;
    s_dropped_bytes = 0;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* ── vprintf hook ────────────────────────────────────────────────────────────*/

static int log_ship_vprintf(const char *fmt, va_list args)
{
    /* Copy args before the first consumer uses them */
    va_list args_ring;
    va_copy(args_ring, args);

    /* Keep UART output working via the original handler */
    int ret = s_orig_vprintf(fmt, args);

    /* Format into a local buffer and write to ring buffer */
    char tmp[LOG_LINE_MAX];
    vsnprintf(tmp, sizeof(tmp), fmt, args_ring);
    va_end(args_ring);
    ringbuf_write(tmp, strlen(tmp));

    return ret;
}

/* ── Log line parser ─────────────────────────────────────────────────────────*/

typedef struct {
    char     level;           /* 'I','W','E','D','V', or '?' on parse failure */
    uint32_t ts_ms;
    char     tag[LOG_TAG_MAX];
    char     msg[LOG_MSG_MAX];
} log_entry_t;

/**
 * Parse an ESP-IDF formatted log line.
 *
 * Expected format (colors disabled):
 *   "L (NNNNN) TAG: MESSAGE\n"
 *
 * On failure the full raw line is placed in msg and level is set to '?'.
 */
static void parse_log_line(const char *line, log_entry_t *out)
{
    size_t len = strlen(line);

    /* Default / fallback: copy full line as message */
    out->level  = '?';
    out->ts_ms  = 0;
    out->tag[0] = '\0';
    size_t copy = len < LOG_MSG_MAX - 1u ? len : LOG_MSG_MAX - 1u;
    memcpy(out->msg, line, copy);
    out->msg[copy] = '\0';
    /* Strip trailing newline */
    while (copy > 0 && (out->msg[copy - 1] == '\n' || out->msg[copy - 1] == '\r')) {
        out->msg[--copy] = '\0';
    }

    /* Validate structure: "L (" */
    if (len < 6 || line[1] != ' ' || line[2] != '(') return;
    char lvl = line[0];
    if (lvl != 'E' && lvl != 'W' && lvl != 'I' && lvl != 'D' && lvl != 'V') return;
    out->level = lvl;

    /* Timestamp: digits between '(' and ') ' */
    const char *ts_start = line + 3;
    const char *ts_end   = strstr(ts_start, ") ");
    if (!ts_end) return;
    out->ts_ms = (uint32_t)strtoul(ts_start, NULL, 10);

    /* Tag: from after ") " to ": " */
    const char *tag_start = ts_end + 2;
    const char *colon     = strstr(tag_start, ": ");
    if (!colon) return;
    size_t tag_len = (size_t)(colon - tag_start);
    if (tag_len >= LOG_TAG_MAX) tag_len = LOG_TAG_MAX - 1u;
    memcpy(out->tag, tag_start, tag_len);
    out->tag[tag_len] = '\0';

    /* Message: everything after ": " */
    const char *msg_start = colon + 2;
    size_t msg_len = strlen(msg_start);
    while (msg_len > 0 && (msg_start[msg_len - 1] == '\n' || msg_start[msg_len - 1] == '\r')) {
        msg_len--;
    }
    if (msg_len >= LOG_MSG_MAX) msg_len = LOG_MSG_MAX - 1u;
    memcpy(out->msg, msg_start, msg_len);
    out->msg[msg_len] = '\0';
}

/* ── JSON batch builder ──────────────────────────────────────────────────────*/

/**
 * Build a JSON document from the drained ring-buffer slice.
 * Returns a heap-allocated string; caller must free() it.
 * Returns NULL on allocation failure.
 */
static char *build_json_batch(const char *drained, size_t len, uint32_t dropped)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "schema", 1);

    /* ── Device block ── */
    cJSON *dev = cJSON_CreateObject();
    if (!dev) { cJSON_Delete(root); return NULL; }
    cJSON_AddStringToObject(dev, "mac",          s_dev.mac);
    cJSON_AddNumberToObject(dev, "chip_model",   s_dev.chip_model);
    cJSON_AddNumberToObject(dev, "chip_cores",   s_dev.chip_cores);
    cJSON_AddNumberToObject(dev, "chip_rev",     s_dev.chip_rev);
    cJSON_AddStringToObject(dev, "idf_ver",      s_dev.idf_ver);
    cJSON_AddStringToObject(dev, "app_version",  s_dev.app_version);
    cJSON_AddStringToObject(dev, "app_date",     s_dev.app_date);
    cJSON_AddStringToObject(dev, "app_time",     s_dev.app_time);
    cJSON_AddNumberToObject(dev, "reset_reason", s_dev.reset_reason);
    cJSON_AddItemToObject(root, "device", dev);

    /* ── Runtime stats ── */
    cJSON_AddNumberToObject(root, "heap_free",
                            (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_free_min",
                            (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_ms",
                            (double)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS));
    cJSON_AddNumberToObject(root, "dropped_bytes", (double)dropped);
    cJSON_AddNumberToObject(root, "seq",           (double)s_seq++);

    /* ── Log entries ── */
    cJSON *logs = cJSON_CreateArray();
    if (!logs) { cJSON_Delete(root); return NULL; }

    const char *p   = drained;
    const char *end = drained + len;

    while (p < end) {
        const char *nl       = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t      line_len = nl ? (size_t)(nl - p + 1u) : (size_t)(end - p);
        if (line_len == 0) break;

        char line_buf[LOG_LINE_MAX];
        size_t cp = line_len < LOG_LINE_MAX - 1u ? line_len : LOG_LINE_MAX - 1u;
        memcpy(line_buf, p, cp);
        line_buf[cp] = '\0';

        log_entry_t entry;
        parse_log_line(line_buf, &entry);

        cJSON *e = cJSON_CreateObject();
        if (e) {
            char lvl_str[2] = {entry.level, '\0'};
            cJSON_AddNumberToObject(e, "ts_ms",  (double)entry.ts_ms);
            cJSON_AddStringToObject(e, "level",  lvl_str);
            cJSON_AddStringToObject(e, "tag",    entry.tag);
            cJSON_AddStringToObject(e, "msg",    entry.msg);
            cJSON_AddItemToArray(logs, e);
        }
        p += line_len;
    }

    cJSON_AddItemToObject(root, "logs", logs);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str; /* caller must free() */
}

/* ── HTTP POST ───────────────────────────────────────────────────────────────*/

static esp_err_t http_post_json(const char *json_str)
{
    esp_http_client_config_t cfg = {
        .url        = LOG_SERVER_URL,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = LOG_SHIP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, (int)strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

/* ── Shipping batch ──────────────────────────────────────────────────────────*/

static void log_ship_send_batch(void)
{
    char *drain_buf = (char *)malloc(LOG_SHIP_DRAIN_MAX + 1u);
    if (!drain_buf) return;

    uint32_t dropped = 0;
    size_t   n       = ringbuf_drain(drain_buf, LOG_SHIP_DRAIN_MAX, &dropped);
    drain_buf[n]     = '\0';

    if (n == 0 && dropped == 0) {
        free(drain_buf);
        return; /* nothing to ship */
    }

    char *json_str = build_json_batch(drain_buf, n, dropped);
    free(drain_buf);
    if (!json_str) return;

    esp_err_t err = http_post_json(json_str);
    if (err != ESP_OK) {
        /* Log directly to avoid re-buffering the failure */
        printf("W (%lu) %s: POST failed: %s\n",
               (unsigned long)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS),
               TAG,
               esp_err_to_name(err));
    }

    free(json_str);
}

/* ── Background task ─────────────────────────────────────────────────────────*/

static void log_ship_task(void *arg)
{
    /* Flush buffered pre-WiFi logs immediately on first run */
    log_ship_send_batch();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LOG_SHIP_TASK_PERIOD_MS));
        log_ship_send_batch();
    }
}

/* ── Device info collection ──────────────────────────────────────────────────*/

static void collect_device_info(void)
{
    /* MAC address (WiFi station interface) */
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_dev.mac, sizeof(s_dev.mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_dev.mac, sizeof(s_dev.mac), "??:??:??:??:??:??");
    }

    /* Chip info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    s_dev.chip_model = (int)chip.model;
    s_dev.chip_cores = chip.cores;
    s_dev.chip_rev   = (int)chip.revision;

    /* IDF version */
    strncpy(s_dev.idf_ver, esp_get_idf_version(), sizeof(s_dev.idf_ver) - 1u);
    s_dev.idf_ver[sizeof(s_dev.idf_ver) - 1u] = '\0';

    /* App description (version, build date/time) */
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strncpy(s_dev.app_version, desc->version, sizeof(s_dev.app_version) - 1u);
        s_dev.app_version[sizeof(s_dev.app_version) - 1u] = '\0';
        strncpy(s_dev.app_date,    desc->date,    sizeof(s_dev.app_date)    - 1u);
        s_dev.app_date[sizeof(s_dev.app_date) - 1u] = '\0';
        strncpy(s_dev.app_time,    desc->time,    sizeof(s_dev.app_time)    - 1u);
        s_dev.app_time[sizeof(s_dev.app_time) - 1u] = '\0';
    }

    /* Reset reason */
    s_dev.reset_reason = (int)esp_reset_reason();
}

/* ── Public API ──────────────────────────────────────────────────────────────*/

void log_ship_init(void)
{
    /* Install vprintf hook.  The OTA gate intentionally runs before this. */
    s_orig_vprintf = esp_log_set_vprintf(log_ship_vprintf);

    /* Collect device metadata (does not log) */
    collect_device_info();

    /*
     * Set global log level to DEBUG for maximum data capture.
     * Silence noisy ESP-IDF internal components to WARN to keep
     * signal-to-noise ratio high for application-level debugging.
     */
    esp_log_level_set("*",                  ESP_LOG_DEBUG);
    esp_log_level_set("wifi",               ESP_LOG_WARN);
    esp_log_level_set("phy",                ESP_LOG_WARN);
    esp_log_level_set("phy_init",           ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip",     ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("HTTP_CLIENT",        ESP_LOG_WARN);
    esp_log_level_set("nvs",                ESP_LOG_WARN);
    esp_log_level_set("efuse",              ESP_LOG_WARN);
    esp_log_level_set("spi_flash",          ESP_LOG_WARN);
    esp_log_level_set("wpa",               ESP_LOG_WARN);
    esp_log_level_set("pp",                 ESP_LOG_WARN);
    esp_log_level_set("net80211",           ESP_LOG_WARN);
    /*
     * esp_http_client fires a per-chunk event into the default event loop,
     * which the `event` component logs at DEBUG ("no handlers have been
     * registered for event ESP_HTTP_CLIENT_EVENT:4 …"). During an OTA the
     * 1 MB body produces hundreds of these and floods the ring buffer.
     */
    esp_log_level_set("event",              ESP_LOG_WARN);
}

void log_ship_wifi_ready(void)
{
    if (s_ship_task != NULL) return; /* idempotent */

    xTaskCreate(
        log_ship_task,
        "log_ship",
        LOG_SHIP_TASK_STACK,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_ship_task
    );
}

void log_ship_deinit(void)
{
    if (s_ship_task) {
        vTaskDelete(s_ship_task);
        s_ship_task = NULL;
    }
    if (s_orig_vprintf) {
        esp_log_set_vprintf(s_orig_vprintf);
        s_orig_vprintf = NULL;
    }
}
