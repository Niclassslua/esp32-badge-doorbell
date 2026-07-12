#include "badge_nightmode.h"

#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "badge_state.h"
#include "wifi_connect.h"

static const char *TAG = "badge.nightmode";

/*
 * Berlin timezone: CET (UTC+1) with automatic summer-time switch to
 * CEST (UTC+2) on the last Sunday of March, reverting on the last
 * Sunday of October.
 */
#define TZ_BERLIN "CET-1CEST,M3.5.0,M10.5.0/3"

/* Night window: [NIGHT_START_HOUR, 24) ∪ [0, WAKE_HOUR) Berlin local time.
 * Overridable for bench tests via -DBADGE_NIGHTMODE_TEST_NIGHT_START_HOUR /
 * -DBADGE_NIGHTMODE_TEST_WAKE_HOUR — unlike TEST_TRIGGER_NOW (which fires on
 * the init path), an hour override exercises the real nightmode_task path. */
#ifndef NIGHT_START_HOUR
#define NIGHT_START_HOUR   23
#endif
#ifndef WAKE_HOUR
#define WAKE_HOUR           8
#endif

/*
 * BADGE_NIGHTMODE_TEST_SLEEP_S — override sleep duration (seconds) so you
 * don't have to wait for 08:00 on wakeup:
 *
 *   idf.py build -DBADGE_NIGHTMODE_TEST_SLEEP_S=60
 *
 * BADGE_NIGHTMODE_TEST_TRIGGER_NOW — also skip waiting for 23:00 and enter
 * sleep immediately at boot. Combine both flags for a full cycle test:
 *
 *   idf.py build -DBADGE_NIGHTMODE_TEST_TRIGGER_NOW=1 -DBADGE_NIGHTMODE_TEST_SLEEP_S=60
 *
 * Leave both at 0 (default) for production.
 */
#ifndef BADGE_NIGHTMODE_TEST_SLEEP_S
#define BADGE_NIGHTMODE_TEST_SLEEP_S 0
#endif

#ifndef BADGE_NIGHTMODE_TEST_TRIGGER_NOW
#define BADGE_NIGHTMODE_TEST_TRIGGER_NOW 0
#endif

/* Maximum time to wait for SNTP sync (ms). */
#define SNTP_TIMEOUT_MS    10000

/* How long to wait between SNTP retry attempts when time is not yet known.
 * Doubles after every failure up to the cap: each failed attempt is a full
 * WiFi bring-up with a 15 s timeout (~100+ mA radio), so retrying every
 * minute forever would murder the battery whenever the home AP is out of
 * reach. */
#define SNTP_RETRY_INTERVAL_MS      60000
#define SNTP_RETRY_INTERVAL_MAX_MS  (30u * 60u * 1000u)

static bool s_time_synced = false;

/* -------------------------------------------------------------------------- */

static bool nightmode_is_active_now(const struct tm *t)
{
    return t->tm_hour >= NIGHT_START_HOUR || t->tm_hour < WAKE_HOUR;
}

/*
 * Seconds from the given local time until the next 08:00:00 Berlin.
 * Always returns a positive value in [1, 86400].
 */
static int64_t seconds_until_wake(const struct tm *t)
{
    int now_sec    = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
    int target_sec = WAKE_HOUR * 3600;

    if (now_sec < target_sec) {
        /* Before 08:00 today (e.g. RTC woke us a few seconds early). */
        return (int64_t)(target_sec - now_sec);
    }
    /* After 08:00 — next wakeup is 08:00 tomorrow. */
    return (int64_t)(24 * 3600 - now_sec + target_sec);
}

/*
 * Seconds from the given local time until the next 23:00:00 Berlin.
 * Always returns a positive value in [1, 86400].
 */
static int64_t seconds_until_night(const struct tm *t)
{
    int now_sec    = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
    int target_sec = NIGHT_START_HOUR * 3600;

    if (now_sec < target_sec) {
        return (int64_t)(target_sec - now_sec);
    }
    /* Already past 23:00 — shouldn't be called then, but handle it. */
    return (int64_t)(24 * 3600 - now_sec + target_sec);
}

/* -------------------------------------------------------------------------- */

static bool sntp_sync(void)
{
    if (wifi_ensure_up(15000) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync skipped: WiFi unavailable");
        return false;
    }

    setenv("TZ", TZ_BERLIN, 1);
    tzset();

    /* Keep the CPU fully awake during sync. Without this, the PM module's
     * automatic light sleep puts the CPU to sleep between FreeRTOS ticks,
     * which prevents the WiFi driver from responding to the AP's Block Ack
     * management frames in time — causing the Fritz.Box to drop the link
     * before the NTP response arrives. The lock is released before we return. */
    esp_pm_lock_handle_t pm_lock = NULL;
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sntp_sync", &pm_lock);
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }

    /* Belt-and-suspenders: also tell the WiFi driver to stay awake. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Stop any previous session before re-initialising (safe on first call). */
    esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    /* Primary: Fritz.Box local NTP (always reachable on the LAN, no UDP-123
     * firewall issues). Fallback: public pool in case the router changes. */
    esp_sntp_setservername(0, "192.168.178.1");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();

    ESP_LOGI(TAG, "waiting for SNTP sync (up to %d ms)...", SNTP_TIMEOUT_MS);

    int elapsed_ms = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED &&
           elapsed_ms < SNTP_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed_ms += 500;
    }

    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
        esp_pm_lock_delete(pm_lock);
    }

    wifi_release();

    /* Check wall-clock validity rather than the SNTP status flag. The flag
     * can reset to SNTP_SYNC_STATUS_RESET the moment WiFi drops — which
     * happens to occur right as the sync completes — causing a false timeout
     * even though gettimeofday() already holds the correct time. */
    time_t now;
    struct tm berlin;
    time(&now);
    localtime_r(&now, &berlin);

    bool synced = (berlin.tm_year + 1900 >= 2024);
    if (synced) {
        ESP_LOGI(TAG, "SNTP sync OK — Berlin time: %02d:%02d:%02d",
                 berlin.tm_hour, berlin.tm_min, berlin.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP sync failed after %d ms (year=%d)",
                 elapsed_ms, berlin.tm_year + 1900);
    }
    return synced;
}

/* -------------------------------------------------------------------------- */

/*
 * Render the nightmode screen, then enter deep sleep until 08:00 Berlin.
 * Never returns.
 */
static void nightmode_sleep(void)
{
    time_t now;
    struct tm berlin;
    time(&now);
    localtime_r(&now, &berlin);

    int64_t sleep_secs = seconds_until_wake(&berlin);
#if BADGE_NIGHTMODE_TEST_SLEEP_S > 0
    sleep_secs = BADGE_NIGHTMODE_TEST_SLEEP_S;
    ESP_LOGW(TAG, "TEST MODE: sleeping %lld s (ignoring real wake time)",
             (long long)sleep_secs);
#else
    ESP_LOGI(TAG, "entering deep sleep at %02d:%02d Berlin — "
                  "waking in %lld s (~08:00)",
             berlin.tm_hour, berlin.tm_min, (long long)sleep_secs);
#endif

    /* Update state, kill LEDs, render display. Blocks until e-paper done. */
    (void)badge_state_enter_nightmode();

    /* The render + I2C battery read above is the deepest call path this task
     * ever executes — log the remaining headroom so stack-size regressions
     * show up on the serial monitor instead of as a canary panic at 23:00. */
    ESP_LOGI(TAG, "stack high-water mark before deep sleep: %u bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    esp_sleep_enable_timer_wakeup((uint64_t)sleep_secs * 1000000ULL);
    esp_deep_sleep_start();
    /* unreachable */
}

/* -------------------------------------------------------------------------- */

/*
 * Log the wakeup cause and, once time is known, the post-sleep accuracy.
 * Called once at init so every boot shows this in the serial monitor.
 */
static void log_wakeup_info(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "wakeup cause: RTC timer (night-mode sleep ended)");
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            ESP_LOGI(TAG, "wakeup cause: power-on / reset (not a deep-sleep wake)");
            break;
        default:
            ESP_LOGI(TAG, "wakeup cause: %d", (int)cause);
            break;
    }
}

/*
 * Once time is synced, if this was a timer wakeup, log whether we landed
 * close to 08:00 so RTC drift is visible in the serial monitor.
 */
static void log_wakeup_accuracy(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        return;
    }
    time_t now;
    struct tm berlin;
    time(&now);
    localtime_r(&now, &berlin);

    int actual_sec  = berlin.tm_hour * 3600 + berlin.tm_min * 60 + berlin.tm_sec;
    int target_sec  = WAKE_HOUR * 3600;
    int delta_sec   = actual_sec - target_sec;
    /* Fold into [-12h, +12h] to handle midnight boundary. */
    if (delta_sec > 12 * 3600) {
        delta_sec -= 24 * 3600;
    } else if (delta_sec < -12 * 3600) {
        delta_sec += 24 * 3600;
    }
    ESP_LOGI(TAG, "wakeup accuracy: actual=%02d:%02d, target=08:00, delta=%+d s",
             berlin.tm_hour, berlin.tm_min, delta_sec);
}

/* -------------------------------------------------------------------------- */

/*
 * Background task. Runs only when we are outside the night window at boot
 * time (i.e. init() didn't deep-sleep us immediately).
 *
 * If SNTP sync failed at init, we retry here every SNTP_RETRY_INTERVAL_MS.
 * Once we have a valid clock we compute the EXACT delay until 23:00 and do
 * a single vTaskDelay — no polling loop needed.
 */
static void nightmode_task(void *arg)
{
    (void)arg;

    /* Retry SNTP until the clock is known, backing off exponentially. */
    uint32_t retry_ms = SNTP_RETRY_INTERVAL_MS;
    while (!s_time_synced) {
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        s_time_synced = sntp_sync();
        if (!s_time_synced) {
            retry_ms = retry_ms * 2 > SNTP_RETRY_INTERVAL_MAX_MS
                           ? SNTP_RETRY_INTERVAL_MAX_MS
                           : retry_ms * 2;
            ESP_LOGW(TAG, "SNTP still failing — next retry in %u s",
                     (unsigned)(retry_ms / 1000));
        }
    }

    /* Log time-to-nightmode for visibility. */
    {
        time_t now;
        struct tm berlin;
        time(&now);
        localtime_r(&now, &berlin);
        int64_t delay_sec = seconds_until_night(&berlin);
        ESP_LOGI(TAG, "night mode in %lld min (%lld h %lld min)",
                 delay_sec / 60, delay_sec / 3600, (delay_sec % 3600) / 60);
    }

    /* Wait for 23:00 in 1-hour chunks.
     * pdMS_TO_TICKS intermediate math is uint32_t: at 100 Hz the multiplication
     * overflows for any delay > ~11.9 h, wrapping to a much shorter delay and
     * causing nightmode_sleep() to fire hours too early. Capping each chunk at
     * 3600 s keeps the intermediate value (3 600 000 * 100 = 360 000 000) safely
     * below UINT32_MAX at any tick rate up to 1000 Hz.
     *
     * Break on nightmode_is_active_now(), NOT on rem <= 0: seconds_until_night()
     * always returns a positive value (time until the *next* 23:00), so rem can
     * never reach 0 and the old condition could never fire. */
    for (;;) {
        time_t now;
        struct tm berlin;
        time(&now);
        localtime_r(&now, &berlin);
        if (nightmode_is_active_now(&berlin)) break;
        int64_t rem = seconds_until_night(&berlin);
        int64_t chunk = rem > 3600 ? 3600 : rem;
        vTaskDelay(pdMS_TO_TICKS(chunk * 1000LL));
    }

    nightmode_sleep();
    /* unreachable */
}

/* -------------------------------------------------------------------------- */

esp_err_t badge_nightmode_init(void)
{
    setenv("TZ", TZ_BERLIN, 1);
    tzset();

    log_wakeup_info();

    s_time_synced = sntp_sync();

    if (s_time_synced) {
        log_wakeup_accuracy();

        time_t now;
        struct tm berlin;
        time(&now);
        localtime_r(&now, &berlin);

        if (nightmode_is_active_now(&berlin)) {
            ESP_LOGI(TAG, "booted inside night window (%02d:%02d Berlin) — "
                          "sleeping immediately",
                     berlin.tm_hour, berlin.tm_min);
            nightmode_sleep();
            /* unreachable */
        }

        ESP_LOGI(TAG, "outside night window (%02d:%02d Berlin) — "
                      "sleeping until 23:00",
                 berlin.tm_hour, berlin.tm_min);
    } else {
        ESP_LOGW(TAG, "time unknown — will activate night mode once clock syncs");
    }

#if BADGE_NIGHTMODE_TEST_TRIGGER_NOW
    /* Trigger immediately regardless of time-sync status so the full
     * deep-sleep/wakeup cycle can be tested on the bench at any time. */
    ESP_LOGW(TAG, "TEST_TRIGGER_NOW: entering night mode immediately");
    nightmode_sleep();
    /* unreachable */
#endif

    /* 8 KB, not 3 KB: this task runs the full e-paper render + DRV2605 I2C
     * battery read at 23:00 (the display task sizes the same path at 4 KB)
     * and, on the SNTP retry path, a full WiFi bring-up — the same reason
     * ota_worker_task and task_ring get 8 KB stacks. */
    BaseType_t ok = xTaskCreate(nightmode_task, "nightmode",
                                8192, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create nightmode task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
