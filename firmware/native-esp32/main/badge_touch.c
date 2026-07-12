#include "badge_touch.h"

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "badge_config.h"
#include "badge_i2c.h"

static const char *TAG = "badge.touch";

#define IQS550_I2C_TIMEOUT_MS   20

/*
 * IQS550 memory map (Azoteq IQS5xx-B000 family).
 *   0x0000  ProductNumber (u16, big-endian)  -> 0x0028 for IQS550 (decimal 40)
 *   0x0011  Number of fingers currently in contact (u8)
 *   0xEEEE  EndCommunicationWindow (write any byte). The chip stays in its
 *           comm-window holding stale data until this is written, so we
 *           close the window after every poll. Without it the finger-count
 *           register never updates and we never see a rising edge.
 * Register addresses are transmitted big-endian (high byte first).
 */
#define IQS550_REG_PRODUCT_NUM  0x0000
#define IQS550_REG_NUM_FINGERS  0x0011
#define IQS550_REG_SYS_CTRL0    0x0431
#define IQS550_REG_SYS_CFG0     0x058E
#define IQS550_REG_SYS_CFG1     0x058F
#define IQS550_REG_END_WINDOW   0xEEEE
#define IQS550_PRODUCT_ID       0x0028

/* SYS_CTRL0 */
#define IQS550_ACK_RESET        0x80
/* SYS_CFG0 (values as written by the Linux iqs5xx driver) */
#define IQS550_SETUP_COMPLETE   0x40
#define IQS550_WDT              0x20
#define IQS550_ALP_REATI        0x08
#define IQS550_REATI            0x04
/* SYS_CFG1 */
#define IQS550_TP_EVENT         0x04
#define IQS550_EVENT_MODE       0x01

/*
 * In event mode the chip NACKs all polls while idle (it sits in its LP
 * sensing modes, µA range) and only opens a comm window on a touch event.
 * A long unbroken run of *successful* reads therefore means the chip
 * reset (brownout etc.) and reverted to its NVM default stream mode —
 * reconfigure it. 80 polls = ~20 s at the 250 ms cadence.
 */
#define IQS550_STREAM_DETECT_READS  80

#define TOUCH_TASK_STACK        4096
#define TOUCH_TASK_PRIO         5

static i2c_master_dev_handle_t   s_iqs_dev;
static badge_touch_press_cb_t    s_press_cb;
static uint8_t                   s_last_count;
static int64_t                   s_debounce_until_us;

static esp_err_t iqs_read(uint16_t reg, uint8_t *out, size_t out_len)
{
    uint8_t addr_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };
    return i2c_master_transmit_receive(s_iqs_dev, addr_buf, sizeof(addr_buf),
                                       out, out_len, IQS550_I2C_TIMEOUT_MS);
}

static esp_err_t iqs_write_u8(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), value };
    return i2c_master_transmit(s_iqs_dev, buf, sizeof(buf),
                               IQS550_I2C_TIMEOUT_MS);
}

/*
 * Switch the IQS550 into event mode. Must be called while a comm window is
 * open (i.e. right after a successful read, before iqs_end_window). Writes
 * are RAM-only — a power cycle restores the chip's NVM defaults (stream
 * mode), which task_touch detects and re-runs this.
 */
static void iqs_configure_event_mode(void)
{
    esp_err_t err = ESP_OK;
    /* Ack a potential reset flag first so the chip accepts the config. */
    err |= iqs_write_u8(IQS550_REG_SYS_CTRL0, IQS550_ACK_RESET);
    err |= iqs_write_u8(IQS550_REG_SYS_CFG0,
                        IQS550_SETUP_COMPLETE | IQS550_WDT |
                        IQS550_ALP_REATI | IQS550_REATI);
    err |= iqs_write_u8(IQS550_REG_SYS_CFG1,
                        IQS550_TP_EVENT | IQS550_EVENT_MODE);
    ESP_LOGI(TAG, "event-mode config %s",
             err == ESP_OK ? "written" : "failed (chip stays in stream mode)");
}

static esp_err_t iqs_end_window(void)
{
    uint8_t buf[3] = {
        (uint8_t)(IQS550_REG_END_WINDOW >> 8),
        (uint8_t)(IQS550_REG_END_WINDOW & 0xff),
        0x00, /* value is ignored, chip just needs the write */
    };
    return i2c_master_transmit(s_iqs_dev, buf, sizeof(buf),
                               IQS550_I2C_TIMEOUT_MS);
}

static esp_err_t iqs_attach(void)
{
    if (s_iqs_dev) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = badge_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BADGE_TOUCH_IQS550_ADDR,
        .scl_speed_hz    = BADGE_TOUCH_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &s_iqs_dev);
}

static esp_err_t iqs_probe(uint16_t *product_out)
{
    uint8_t buf[2] = { 0 };
    /*
     * The IQS550 may hold its communication window closed between polls.
     * Retry the product-number read a few times so a transient NACK at
     * boot does not disable the keyboard for the rest of the session.
     */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        err = iqs_read(IQS550_REG_PRODUCT_NUM, buf, sizeof(buf));
        if (err == ESP_OK) {
            *product_out = ((uint16_t)buf[0] << 8) | buf[1];
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return err;
}

static void task_touch(void *arg)
{
    (void)arg;
    const TickType_t poll_ticks = pdMS_TO_TICKS(BADGE_TOUCH_POLL_MS);

    /* Prime the comm-window state so the very first read returns live data. */
    (void)iqs_end_window();
    s_last_count = 0;

    unsigned ok_streak = 0;

    while (true) {
        vTaskDelay(poll_ticks);

        uint8_t count = 0;
        esp_err_t err = iqs_read(IQS550_REG_NUM_FINGERS, &count, 1);

        if (err != ESP_OK) {
            /* Expected in event mode: the chip NACKs while idle in its
             * low-power sensing modes and only ACKs when a touch event
             * opened a comm window. Don't log. */
            ok_streak = 0;
            continue;
        }

        /* A long unbroken run of ACKs means the chip reset back to its
         * stream-mode NVM defaults — put it back into event mode (we're
         * inside an open comm window right now, so the writes land). */
        if (++ok_streak >= IQS550_STREAM_DETECT_READS) {
            ESP_LOGW(TAG, "stream mode detected (%u consecutive reads) — "
                          "reconfiguring event mode", ok_streak);
            iqs_configure_event_mode();
            ok_streak = 0;
        }

        /* Release the comm window so the chip goes back to sensing. */
        (void)iqs_end_window();

        if (count != s_last_count) {
            ESP_LOGI(TAG, "finger count %u -> %u", s_last_count, count);
        }

        bool rising = (s_last_count == 0) && (count > 0);
        s_last_count = count;
        if (!rising) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us < s_debounce_until_us) {
            continue;
        }
        s_debounce_until_us =
            now_us + (int64_t)BADGE_TOUCH_DEBOUNCE_MS * 1000;

        badge_touch_press_cb_t cb = s_press_cb;
        if (cb) {
            ESP_LOGI(TAG, "touch press (fingers=%u)", count);
            cb();
        }
    }
}

void badge_touch_set_press_cb(badge_touch_press_cb_t cb)
{
    s_press_cb = cb;
}

esp_err_t badge_touch_init(void)
{
    /* The IQS550 sleeps between sensing cycles and NACKs any reads we
     * attempt while it's not in its comm window. That's normal for blind
     * polling without RDY, but the IDF i2c.master logs every NACK at
     * ERROR level which floods the log ring (~24 KB/sec of noise during
     * idle). Silence the driver — our own ESP_LOGD line still records
     * the rare genuine failure. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    esp_err_t err = iqs_attach();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IQS550 attach failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t product = 0;
    err = iqs_probe(&product);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IQS550 not responding at 0x%02x: %s",
                 BADGE_TOUCH_IQS550_ADDR, esp_err_to_name(err));
        return err;
    }
    if (product != IQS550_PRODUCT_ID) {
        ESP_LOGW(TAG, "IQS550 unexpected product=0x%04x (want 0x%04x); continuing anyway",
                 product, IQS550_PRODUCT_ID);
    } else {
        ESP_LOGI(TAG, "IQS550 product=0x%04x attached at 0x%02x",
                 product, BADGE_TOUCH_IQS550_ADDR);
    }
    /* The probe read left a comm window open — use it to switch the chip
     * into event mode (LP sensing between touches, comm window only on
     * events), then release the window so sensing starts. */
    iqs_configure_event_mode();
    (void)iqs_end_window();

    if (xTaskCreate(task_touch, "touch", TOUCH_TASK_STACK,
                    NULL, TOUCH_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
