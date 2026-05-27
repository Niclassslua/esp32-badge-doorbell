#include "badge_power.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "badge_config.h"
#include "badge_i2c.h"

static const char *TAG = "badge.power";

#define BATTERY_I2C_TIMEOUT_MS 30

/*
 * Haptic uses the DRV2605's built-in ROM waveform library rather than RTP.
 * The library effects are tuned for the actuator-class and produce much
 * stronger output than driving RTP at fixed amplitude, even at 0x7f
 * (which is already max signed-mode scale).
 *
 * Effect 47 = "Buzz 1 - 100%" — long, intense buzz, ideal for a doorbell.
 * Effect 14 = "Strong Buzz - 100%" — shorter alternative if 47 feels too long.
 *
 * Library 1 = ERM library A (works for the cheap eccentric-rotating-mass
 * motors typical of conference badges). After power-on reset, the library
 * selection register is 0 (empty) so we must set it explicitly or the GO
 * trigger plays silence.
 */
#define HAPTIC_DRV_STATUS_REG     0x00
#define HAPTIC_DRV_LIB_REG        0x03
#define HAPTIC_DRV_WAVESEQ1_REG   0x04   /* slot 1; slots 2-8 are 0x05..0x0b */
#define HAPTIC_DRV_RATED_V_REG    0x16
#define HAPTIC_DRV_OD_CLAMP_REG   0x17
#define HAPTIC_DRV_FB_CONTROL_REG 0x1a
#define HAPTIC_DRV_CONTROL3_REG   0x1d
/*
 * The badge actuator is an LRA, not an ERM (deduced from the diagnostic
 * read-back: GO accepted, DIAG_RESULT=0, but virtually no felt output in
 * ERM mode). The DRV2605L's LRA support lives in a different library and
 * needs different control-register bits.
 *
 * Library 6 = ROM LRA library. Effects 1-123 exist in this library tuned
 * for LRA actuators at resonance, so the same effect numbers used in ERM
 * mode work here — just with proper LRA drive timing.
 */
#define HAPTIC_LIBRARY_LRA        0x06
/*
 * Effect 14 = "Strong Buzz - 100%" in the LRA library, ~0.84 s.
 * Chosen over the longer effect 118 because the longer one was too
 * intense as a doorbell. If we ever want it stronger/longer again,
 * swap back to 118.
 */
#define HAPTIC_EFFECT_BUZZ        14
#define HAPTIC_EFFECT_END         0x00
/*
 * FB_CONTROL register (0x1A):
 *   bit 7  N_ERM_LRA: 1 = LRA actuator
 *   bits 6:4 FB_BRAKE_FACTOR: 0b011 = 4x (default)
 *   bits 3:2 LOOP_GAIN: 0b01 = medium (default)
 *   bits 1:0 BEMF_GAIN: 0b10 = default
 *   = 0b1_011_01_10 = 0xB6
 */
#define HAPTIC_FB_CONTROL_LRA     0xB6
/*
 * CONTROL3 register (0x1D):
 *   bit 0 LRA_OPEN_LOOP = 0 → closed loop (chip auto-finds LRA resonance
 *         via back-EMF; works without autocal because the LRA's resonance
 *         is reasonably narrow-band). Other bits at default 0x80.
 *   = 0x80
 */
#define HAPTIC_CONTROL3_LRA       0x80
#define HAPTIC_MODE_INTERNAL      0x00   /* Internal trigger via GO */
/* Half-intensity drive: roughly half the default voltage envelope. */
#define HAPTIC_RATED_VOLTAGE      0x80
#define HAPTIC_OD_CLAMP           0x80
#define HAPTIC_CHAIN_SLOTS        1
#define HAPTIC_PLAYBACK_MS        750    /* matches effect 14 length */

static i2c_master_dev_handle_t s_drv_dev;
static SemaphoreHandle_t       s_drv_mutex;

static int clamp_int(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static bool battery_mv_plausible(int battery_mv)
{
    return battery_mv >= BADGE_BATTERY_VALID_MIN_MV &&
           battery_mv <= BADGE_BATTERY_VALID_MAX_MV;
}

static int battery_mv_from_drv2605_raw(uint8_t raw)
{
    return ((int)raw * 5600 + 127) / 255;
}

/*
 * Realistic single-cell LiPo discharge curve. Linear interpolation between
 * adjacent entries; clamped to [0, 100] outside the table. The curve sits
 * mostly near ~3.7 V which is what a real cell does — a straight 3.3-4.2 V
 * map would otherwise report 50% for a long stretch where the cell is
 * actually anywhere from 30% to 80%.
 */
static const struct {
    int mv;
    int percent;
} k_lipo_curve[] = {
    { 4200, 100 },
    { 4060,  90 },
    { 3980,  80 },
    { 3920,  70 },
    { 3870,  60 },
    { 3820,  50 },
    { 3790,  40 },
    { 3770,  30 },
    { 3740,  20 },
    { 3680,  10 },
    { 3450,   5 },
    { 3300,   0 },
};

static int battery_percent_from_mv(int battery_mv)
{
    const size_t n = sizeof(k_lipo_curve) / sizeof(k_lipo_curve[0]);
    if (battery_mv >= k_lipo_curve[0].mv) {
        return 100;
    }
    if (battery_mv <= k_lipo_curve[n - 1].mv) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if (battery_mv >= k_lipo_curve[i].mv) {
            int hi_mv = k_lipo_curve[i - 1].mv;
            int lo_mv = k_lipo_curve[i].mv;
            int hi_pct = k_lipo_curve[i - 1].percent;
            int lo_pct = k_lipo_curve[i].percent;
            int span_mv = hi_mv - lo_mv;
            int span_pct = hi_pct - lo_pct;
            int pct = lo_pct + ((battery_mv - lo_mv) * span_pct) / span_mv;
            return clamp_int(pct, 0, 100);
        }
    }
    return 0;
}

static esp_err_t drv_attach(void)
{
    if (!s_drv_mutex) {
        s_drv_mutex = xSemaphoreCreateMutex();
        if (!s_drv_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_drv_dev) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = badge_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "shared I2C bus not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BADGE_BATTERY_DRV2605_ADDR,
        .scl_speed_hz = BADGE_BATTERY_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_drv_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 attach failed: %s", esp_err_to_name(err));
        s_drv_dev = NULL;
    }
    return err;
}

static esp_err_t drv_read_reg(uint8_t reg, uint8_t *value, size_t value_len)
{
    return i2c_master_transmit_receive(s_drv_dev, &reg, 1, value, value_len,
                                       BATTERY_I2C_TIMEOUT_MS);
}

static esp_err_t drv_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_drv_dev, buf, sizeof(buf),
                               BATTERY_I2C_TIMEOUT_MS);
}

/*
 * Trigger a fresh VBAT sample on the DRV2605, then read it.
 *
 * Per TI datasheet SLOS825B §7.6.1, VBAT is only updated "during playback".
 * An idle DRV2605 returns the last value (typically saturated 0xFF on cold
 * boot). We put the chip into RTP (Real-Time Playback) mode with the input
 * amplitude register set to 0. RTP at amplitude 0 engages the chip's supply
 * monitor without driving any current into the motor, so the measurement is
 * silent. After a short delay we read VBAT and return the chip to standby.
 */
static esp_err_t drv2605_trigger_and_read_vbat(uint8_t *vbat_raw)
{
    esp_err_t err;

    err = drv_write_reg(BADGE_BATTERY_DRV2605_MODE_REG,
                        BADGE_BATTERY_DRV2605_MODE_STANDBY);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "drv2605 standby write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = drv_write_reg(BADGE_BATTERY_DRV2605_RTP_INPUT_REG, 0x00);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "drv2605 rtp_input=0 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = drv_write_reg(BADGE_BATTERY_DRV2605_MODE_REG,
                        BADGE_BATTERY_DRV2605_MODE_RTP);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "drv2605 rtp mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = drv_write_reg(BADGE_BATTERY_DRV2605_GO_REG,
                        BADGE_BATTERY_DRV2605_GO_BIT);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "drv2605 GO write failed: %s", esp_err_to_name(err));
        goto restore_standby;
    }

    vTaskDelay(pdMS_TO_TICKS(BADGE_BATTERY_DRV2605_TRIGGER_MS));

    err = drv_read_reg(BADGE_BATTERY_DRV2605_VBAT_REG, vbat_raw, 1);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "drv2605 vbat read failed: %s", esp_err_to_name(err));
        goto restore_standby;
    }

    int64_t deadline_us =
        esp_timer_get_time() + (int64_t)BADGE_BATTERY_DRV2605_GO_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline_us) {
        uint8_t go = 0;
        if (drv_read_reg(BADGE_BATTERY_DRV2605_GO_REG, &go, 1) == ESP_OK &&
            (go & BADGE_BATTERY_DRV2605_GO_BIT) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

restore_standby:
    (void)drv_write_reg(BADGE_BATTERY_DRV2605_MODE_REG,
                        BADGE_BATTERY_DRV2605_MODE_STANDBY);
    return err;
}

static int read_drv2605_battery_percent(void)
{
    if (drv_attach() != ESP_OK) {
        return -1;
    }

    xSemaphoreTake(s_drv_mutex, portMAX_DELAY);
    uint8_t raw = 0;
    esp_err_t err = drv2605_trigger_and_read_vbat(&raw);
    xSemaphoreGive(s_drv_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 read failed: %s", esp_err_to_name(err));
        return -1;
    }

    int battery_mv = battery_mv_from_drv2605_raw(raw);
    bool plausible = battery_mv_plausible(battery_mv) && raw != 0xff;
    int percent = battery_percent_from_mv(battery_mv);

    ESP_LOGI(TAG, "battery DRV2605 raw=%u mv=%d percent=%d%s",
             raw, battery_mv, percent,
             plausible ? "" : " ignored; saturated or out of range after trigger");

    return plausible ? percent : -1;
}

void badge_power_haptic_pulse(void)
{
    if (drv_attach() != ESP_OK) {
        return;
    }

    /* Non-blocking acquire: if a VBAT read is in flight, just skip the buzz —
     * the user still gets LED + webhook, and contention is rare given the
     * 5 s VBAT cache. */
    if (xSemaphoreTake(s_drv_mutex, 0) != pdTRUE) {
        ESP_LOGD(TAG, "haptic skipped: DRV2605 busy");
        return;
    }

    esp_err_t err;

    /* Wake from standby into internal-trigger mode. */
    err = drv_write_reg(BADGE_BATTERY_DRV2605_MODE_REG, HAPTIC_MODE_INTERNAL);
    if (err != ESP_OK) goto done;

    /* Configure for LRA actuator: closed-loop, default brake/gain. */
    err = drv_write_reg(HAPTIC_DRV_FB_CONTROL_REG, HAPTIC_FB_CONTROL_LRA);
    if (err != ESP_OK) goto done;
    err = drv_write_reg(HAPTIC_DRV_CONTROL3_REG, HAPTIC_CONTROL3_LRA);
    if (err != ESP_OK) goto done;

    /* Select the LRA ROM library. */
    err = drv_write_reg(HAPTIC_DRV_LIB_REG, HAPTIC_LIBRARY_LRA);
    if (err != ESP_OK) goto done;

    /* Push drive voltage to max so the ERM gets a real kick. */
    err = drv_write_reg(HAPTIC_DRV_RATED_V_REG, HAPTIC_RATED_VOLTAGE);
    if (err != ESP_OK) goto done;
    err = drv_write_reg(HAPTIC_DRV_OD_CLAMP_REG, HAPTIC_OD_CLAMP);
    if (err != ESP_OK) goto done;

    /* Waveform sequence: chain N buzz effects, then end-marker. The slot
     * registers are contiguous from 0x04, so we just walk forward. */
    for (int slot = 0; slot < HAPTIC_CHAIN_SLOTS; slot++) {
        err = drv_write_reg(HAPTIC_DRV_WAVESEQ1_REG + slot, HAPTIC_EFFECT_BUZZ);
        if (err != ESP_OK) goto done;
    }
    err = drv_write_reg(HAPTIC_DRV_WAVESEQ1_REG + HAPTIC_CHAIN_SLOTS,
                        HAPTIC_EFFECT_END);
    if (err != ESP_OK) goto done;

    /* Fire. */
    err = drv_write_reg(BADGE_BATTERY_DRV2605_GO_REG,
                        BADGE_BATTERY_DRV2605_GO_BIT);
    if (err != ESP_OK) goto done;

    /* Confirm the chip actually accepted the trigger. GO should still be
     * set right after the write (playback in progress); STATUS bit 3
     * (DIAG_RESULT) being set indicates a diagnostic failure. */
    uint8_t go = 0, status = 0;
    (void)drv_read_reg(BADGE_BATTERY_DRV2605_GO_REG, &go, 1);
    (void)drv_read_reg(HAPTIC_DRV_STATUS_REG, &status, 1);
    ESP_LOGI(TAG, "haptic fired: GO=0x%02x STATUS=0x%02x (DIAG_RESULT=%d)",
             go, status, (status >> 3) & 1);

    vTaskDelay(pdMS_TO_TICKS(HAPTIC_PLAYBACK_MS));

done:
    (void)drv_write_reg(BADGE_BATTERY_DRV2605_MODE_REG,
                        BADGE_BATTERY_DRV2605_MODE_STANDBY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "haptic pulse failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_drv_mutex);
}

int badge_power_get_battery_percent(void)
{
    static int cached_percent = -1;
    static int64_t cached_until_us = 0;

    int64_t now_us = esp_timer_get_time();
    if (cached_percent >= 0 && now_us < cached_until_us) {
        return cached_percent;
    }

    int percent = read_drv2605_battery_percent();
    if (percent >= 0) {
        cached_percent = percent;
        cached_until_us = now_us + (int64_t)BADGE_BATTERY_CACHE_TTL_MS * 1000;
    } else {
        /* Short cool-down on failure so we don't I2C-flog the bus from
         * a tight redraw loop, but expire fast so a recovery is noticed. */
        cached_until_us = now_us + (int64_t)1000 * 1000;
    }
    return percent;
}
