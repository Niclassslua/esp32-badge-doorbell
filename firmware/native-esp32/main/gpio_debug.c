/*
 * gpio_debug.c - live GPIO and ADC probe for TR22 badge bring-up.
 *
 * This mode is intentionally read-only: it never drives a GPIO high/low.  It
 * only changes input pull resistors on pins that support them, then logs:
 *   - static pull/floating classification for every safe GPIO
 *   - ADC readings for every ADC-capable GPIO
 *   - live edge events while buttons are pressed
 *   - periodic level and ADC changes, in case an interrupt is missed
 */

#include "gpio_debug.h"

#include "badge_i2c.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "gpio_dbg";

typedef struct {
    gpio_num_t num;
    const char *note;
    bool input_only;
    bool strapping;
} gpio_probe_t;

/*
 * Excluded:
 *   GPIO1/GPIO3  - UART0 console/logging
 *   GPIO6-GPIO11 - external flash bus
 *
 * Included strapping pins are read-only here.  Do not externally force them at
 * reset unless you mean to change boot mode.
 */
#ifndef GPIO_DEBUG_INPUT_ONLY
#define GPIO_DEBUG_INPUT_ONLY 0
#endif

#if GPIO_DEBUG_INPUT_ONLY
/*
 * Passive probe subset for hunting interrupt lines (IQS550 RDY, PCA9555
 * INT). GPIO34-39 are input-only pads; the rest are the spare GPIOs not
 * claimed by flash, PSRAM (16/17 excluded!), UART, I2C, LED, or e-paper.
 * In this mode ALL pull resistors stay disabled (see
 * watch_pullup_for_probe), so every pin is watched as a bare input buffer
 * — electrically passive even on strap pins.
 */
static const gpio_probe_t SAFE_GPIOS[] = {
    { GPIO_NUM_2,  "spare strap",          false, true  },
    { GPIO_NUM_12, "spare strap adc2",     false, true  },
    { GPIO_NUM_14, "spare jtag adc2",      false, false },
    { GPIO_NUM_15, "spare strap jtag",     false, true  },
    { GPIO_NUM_19, "spare",                false, false },
    { GPIO_NUM_21, "spare (tr19 sda?)",    false, false },
    { GPIO_NUM_26, "spare adc2 dac2",      false, false },
    { GPIO_NUM_32, "spare adc1",           false, false },
    { GPIO_NUM_33, "spare adc1",           false, false },
    { GPIO_NUM_34, "adc1 input-only",      true,  false },
    { GPIO_NUM_35, "adc1 input-only",      true,  false },
    { GPIO_NUM_36, "adc1 input-only",      true,  false },
    { GPIO_NUM_37, "adc1 input-only",      true,  false },
    { GPIO_NUM_38, "adc1 input-only",      true,  false },
    { GPIO_NUM_39, "adc1 input-only",      true,  false },
};
#else
static const gpio_probe_t SAFE_GPIOS[] = {
    { GPIO_NUM_0,  "strap boot / epd rst", false, true  },
    { GPIO_NUM_2,  "strap",                false, true  },
    { GPIO_NUM_4,  "adc2 touch0",          false, false },
    { GPIO_NUM_5,  "strap",                false, true  },
    { GPIO_NUM_12, "strap adc2",           false, true  },
    { GPIO_NUM_13, "epd busy adc2",        false, false },
    { GPIO_NUM_14, "jtag adc2",            false, false },
    { GPIO_NUM_15, "strap jtag adc2",      false, true  },
    { GPIO_NUM_16, "",                     false, false },
    { GPIO_NUM_17, "",                     false, false },
    { GPIO_NUM_18, "epd sck",              false, false },
    { GPIO_NUM_19, "",                     false, false },
    { GPIO_NUM_21, "i2c sda?",             false, false },
    { GPIO_NUM_22, "i2c scl?",             false, false },
    { GPIO_NUM_23, "epd mosi",             false, false },
    { GPIO_NUM_25, "epd cs / adc2 dac1",   false, false },
    { GPIO_NUM_26, "adc2 dac2",            false, false },
    { GPIO_NUM_27, "epd dc / adc2",        false, false },
    { GPIO_NUM_32, "adc1",                 false, false },
    { GPIO_NUM_33, "adc1",                 false, false },
    { GPIO_NUM_34, "adc1 input-only",      true,  false },
    { GPIO_NUM_35, "adc1 input-only",      true,  false },
    { GPIO_NUM_36, "adc1 input-only",      true,  false },
    { GPIO_NUM_37, "adc1 input-only",      true,  false },
    { GPIO_NUM_38, "adc1 input-only",      true,  false },
    { GPIO_NUM_39, "adc1 input-only",      true,  false },
};
#endif /* GPIO_DEBUG_INPUT_ONLY */

#define N_SAFE_GPIOS (sizeof(SAFE_GPIOS) / sizeof(SAFE_GPIOS[0]))

#define SETTLE_MS              8
#define WATCH_QUEUE_LEN        128
#define WATCH_TASK_STACK       4096
#define SAMPLE_TASK_STACK      6144
#define GPIO_SAMPLE_MS         1000
#define ADC_SAMPLE_MS          5000
#define ADC_RAW_DELTA_LOG      80
#define EDGE_NOISE_WINDOW_MS   3000
#define EDGE_NOISE_LIMIT       24
#define BATTERY_SCALE_NUM      2
#define BATTERY_SCALE_DEN      1
#define BATTERY_MIN_MV         2500
#define BATTERY_MAX_MV         4600

typedef struct {
    gpio_num_t pin;
    int level;
    uint32_t tick_ms;
} gpio_event_t;

typedef struct {
    bool readable;
    bool plausible_battery;
    adc_unit_t unit;
    adc_channel_t channel;
    int raw;
    int adc_mv;
    int scaled_mv;
} adc_sample_t;

static QueueHandle_t s_event_queue;
static int s_last_levels[N_SAFE_GPIOS];
static int s_last_adc_raw[GPIO_NUM_MAX];
static bool s_last_adc_valid[GPIO_NUM_MAX];
static volatile bool s_adc_sampling;

static bool gpio_supports_internal_pull(gpio_num_t pin)
{
    return !(pin >= GPIO_NUM_34 && pin <= GPIO_NUM_39);
}

static const gpio_probe_t *find_probe(gpio_num_t pin)
{
    for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
        if (SAFE_GPIOS[i].num == pin) {
            return &SAFE_GPIOS[i];
        }
    }
    return NULL;
}

static const char *pin_note(gpio_num_t pin)
{
    const gpio_probe_t *probe = find_probe(pin);
    return probe ? probe->note : "";
}

static void configure_input(gpio_num_t pin, gpio_pullup_t pull_up,
                            gpio_pulldown_t pull_down, gpio_int_type_t intr)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up,
        .pull_down_en = pull_down,
        .intr_type = intr,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config GPIO%d failed: %s", pin, esp_err_to_name(err));
    }
}

static gpio_pullup_t watch_pullup_for_probe(const gpio_probe_t *probe)
{
#if GPIO_DEBUG_INPUT_ONLY
    /* Passive-probe mode: never engage pull resistors on any pin. */
    (void)probe;
    return GPIO_PULLUP_DISABLE;
#else
    if (!gpio_supports_internal_pull(probe->num) || probe->strapping) {
        return GPIO_PULLUP_DISABLE;
    }
    return GPIO_PULLUP_ENABLE;
#endif
}

static void configure_watch_pin(const gpio_probe_t *probe)
{
    configure_input(probe->num, watch_pullup_for_probe(probe),
                    GPIO_PULLDOWN_DISABLE, GPIO_INTR_ANYEDGE);
}

static void restore_watch_pins(void)
{
    for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
        configure_watch_pin(&SAFE_GPIOS[i]);
        s_last_levels[i] = gpio_get_level(SAFE_GPIOS[i].num);
    }
}

static int read_with_pull(gpio_num_t pin, gpio_pullup_t pull_up,
                          gpio_pulldown_t pull_down)
{
    configure_input(pin, pull_up, pull_down, GPIO_INTR_DISABLE);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    return gpio_get_level(pin);
}

static const char *classify_gpio(bool input_only, int flt, int pu, int pd)
{
    if (input_only) {
        return flt ? "input-only-high" : "input-only-low";
    }
    if (pu == 1 && pd == 1) {
        return "externally-high";
    }
    if (pu == 0 && pd == 0) {
        return "externally-low";
    }
    if (pu == 1 && pd == 0) {
        return "floating";
    }
    return "unstable";
}

static int adc_mv_from_raw(int raw)
{
    return (raw * 4034) / 4095;
}

static int scaled_mv_from_adc_mv(int adc_mv)
{
    return (adc_mv * BATTERY_SCALE_NUM) / BATTERY_SCALE_DEN;
}

static bool plausible_battery_mv(int mv)
{
    return mv >= BATTERY_MIN_MV && mv <= BATTERY_MAX_MV;
}

static esp_err_t read_adc_pin(gpio_num_t gpio, adc_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));

    esp_err_t err = adc_oneshot_io_to_channel(gpio, &sample->unit,
                                              &sample->channel);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_unit_handle_t handle = NULL;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = sample->unit,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &handle);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(handle, sample->channel, &chan_cfg);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(handle);
        return err;
    }

    const int samples = 8;
    int raw_total = 0;
    int raw_count = 0;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        err = adc_oneshot_read(handle, sample->channel, &raw);
        if (err == ESP_OK) {
            raw_total += raw;
            raw_count++;
        }
    }

    adc_oneshot_del_unit(handle);

    if (raw_count == 0) {
        return err == ESP_OK ? ESP_FAIL : err;
    }

    sample->readable = true;
    sample->raw = raw_total / raw_count;
    sample->adc_mv = adc_mv_from_raw(sample->raw);
    sample->scaled_mv = scaled_mv_from_adc_mv(sample->adc_mv);
    sample->plausible_battery = plausible_battery_mv(sample->scaled_mv);
    return ESP_OK;
}

static void log_adc_pin(gpio_num_t gpio, const char *phase)
{
    adc_sample_t sample;
    esp_err_t err = read_adc_pin(gpio, &sample);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "%s adc gpio=%d status=%s note=\"%s\"",
                 phase, gpio, esp_err_to_name(err), pin_note(gpio));
        return;
    }

    ESP_LOGI(TAG,
             "%s adc gpio=%d unit=%d channel=%d raw=%d adc_mv=%d scaled_mv=%d battery_candidate=%s note=\"%s\"",
             phase, gpio, sample.unit, sample.channel, sample.raw,
             sample.adc_mv, sample.scaled_mv,
             sample.plausible_battery ? "yes" : "no", pin_note(gpio));
}

void gpio_debug_scan(void)
{
    ESP_LOGI(TAG, "gpio_debug_start mode=static_scan pins=%u",
             (unsigned)N_SAFE_GPIOS);
    ESP_LOGI(TAG, "gpio_scan_fields gpio floating pullup pulldown class strap note");

    for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
        const gpio_probe_t *probe = &SAFE_GPIOS[i];
        gpio_num_t pin = probe->num;

        int flt = read_with_pull(pin, GPIO_PULLUP_DISABLE,
                                 GPIO_PULLDOWN_DISABLE);
        int pu = flt;
        int pd = flt;

#if !GPIO_DEBUG_INPUT_ONLY
        /* Passive-probe mode skips the pull tests — floating read only. */
        if (gpio_supports_internal_pull(pin)) {
            pu = read_with_pull(pin, GPIO_PULLUP_ENABLE,
                                GPIO_PULLDOWN_DISABLE);
            pd = read_with_pull(pin, GPIO_PULLUP_DISABLE,
                                GPIO_PULLDOWN_ENABLE);
        }
#endif

        s_last_levels[i] = flt;

        ESP_LOGI(TAG,
                 "gpio_scan gpio=%d floating=%d pullup=%d pulldown=%d class=%s strap=%s note=\"%s\"",
                 pin, flt, pu, pd,
                 classify_gpio(probe->input_only, flt, pu, pd),
                 probe->strapping ? "yes" : "no", probe->note);

        configure_input(pin, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE,
                        GPIO_INTR_DISABLE);
    }

    ESP_LOGI(TAG, "adc_scan_start");
    for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
        log_adc_pin(SAFE_GPIOS[i].num, "adc_scan");
    }
    ESP_LOGI(TAG, "adc_scan_done");
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    if (s_adc_sampling) {
        return;
    }

    gpio_num_t pin = (gpio_num_t)(uintptr_t)arg;
    gpio_event_t evt = {
        .pin = pin,
        .level = gpio_get_level(pin),
        .tick_ms = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS),
    };
    xQueueSendFromISR(s_event_queue, &evt, NULL);
}

static void watch_task(void *arg)
{
    (void)arg;
    gpio_event_t evt;
    bool event_seen[GPIO_NUM_MAX] = {0};
    bool noisy_disabled[GPIO_NUM_MAX] = {0};
    int last_event_level[GPIO_NUM_MAX] = {0};
    uint32_t noise_window_start[GPIO_NUM_MAX] = {0};
    uint16_t noise_window_count[GPIO_NUM_MAX] = {0};

    ESP_LOGI(TAG, "gpio_watch_ready press_buttons_now=1");
    while (true) {
        if (xQueueReceive(s_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.pin < 0 || evt.pin >= GPIO_NUM_MAX) {
                continue;
            }
            if (noisy_disabled[evt.pin]) {
                continue;
            }

            if (!event_seen[evt.pin]) {
                event_seen[evt.pin] = true;
                last_event_level[evt.pin] = evt.level;
                noise_window_start[evt.pin] = evt.tick_ms;
                noise_window_count[evt.pin] = 1;
            } else {
                if (evt.tick_ms - noise_window_start[evt.pin] > EDGE_NOISE_WINDOW_MS) {
                    noise_window_start[evt.pin] = evt.tick_ms;
                    noise_window_count[evt.pin] = 0;
                }
                noise_window_count[evt.pin]++;

                if (noise_window_count[evt.pin] > EDGE_NOISE_LIMIT) {
                    gpio_intr_disable(evt.pin);
                    noisy_disabled[evt.pin] = true;
                    ESP_LOGW(TAG,
                             "gpio_noise_suppressed gpio=%d window_ms=%d events=%u note=\"%s\"",
                             evt.pin, EDGE_NOISE_WINDOW_MS,
                             (unsigned)noise_window_count[evt.pin],
                             pin_note(evt.pin));
                    continue;
                }

                if (evt.level == last_event_level[evt.pin]) {
                    continue;
                }
            }

            last_event_level[evt.pin] = evt.level;

            ESP_LOGI(TAG,
                     "gpio_edge ms=%" PRIu32 " gpio=%d level=%d edge=%s note=\"%s\"",
                     evt.tick_ms, evt.pin, evt.level,
                     evt.level ? "rising" : "falling", pin_note(evt.pin));
        }
    }
}

static void sampler_task(void *arg)
{
    (void)arg;
    uint32_t last_adc_ms = 0;

    while (true) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
            gpio_num_t pin = SAFE_GPIOS[i].num;
            int level = gpio_get_level(pin);
            if (level != s_last_levels[i]) {
                ESP_LOGI(TAG, "gpio_level_change ms=%" PRIu32 " gpio=%d level=%d prev=%d note=\"%s\"",
                         now_ms, pin, level, s_last_levels[i], SAFE_GPIOS[i].note);
                s_last_levels[i] = level;
            }
        }

        if (now_ms - last_adc_ms >= ADC_SAMPLE_MS) {
            last_adc_ms = now_ms;
            s_adc_sampling = true;
            for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
                gpio_num_t gpio = SAFE_GPIOS[i].num;
                adc_sample_t sample;
                esp_err_t err = read_adc_pin(gpio, &sample);
                if (err != ESP_OK) {
                    continue;
                }

                int previous = s_last_adc_raw[gpio];
                bool previous_valid = s_last_adc_valid[gpio];
                int delta = previous_valid ? sample.raw - previous : 0;
                if (!previous_valid || delta > ADC_RAW_DELTA_LOG ||
                    delta < -ADC_RAW_DELTA_LOG || sample.plausible_battery) {
                    ESP_LOGI(TAG,
                             "adc_live ms=%" PRIu32 " gpio=%d raw=%d delta=%d adc_mv=%d scaled_mv=%d battery_candidate=%s note=\"%s\"",
                             now_ms, gpio, sample.raw, delta, sample.adc_mv,
                             sample.scaled_mv,
                             sample.plausible_battery ? "yes" : "no",
                             SAFE_GPIOS[i].note);
                }
                s_last_adc_raw[gpio] = sample.raw;
                s_last_adc_valid[gpio] = true;
            }
            restore_watch_pins();
            s_adc_sampling = false;
        }

        vTaskDelay(pdMS_TO_TICKS(GPIO_SAMPLE_MS));
    }
}

#if GPIO_DEBUG_INPUT_ONLY
/*
 * PCA9555 expander watcher: the nav buttons use only port 0, so the IQS550
 * RDY line may be routed to one of the 8 unused port-1 pins. Poll both
 * input ports fast and log every change. Requires badge_i2c_init() first.
 */
#define EXPANDER_ADDR        0x20
#define EXPANDER_POLL_MS     20
#define EXPANDER_I2C_TIMEOUT 20

static void expander_watch_task(void *arg)
{
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)arg;
    uint8_t last[2] = { 0xff, 0xff };
    bool have_last = false;

    while (true) {
        uint8_t reg = 0x00; /* input port 0; reads auto-increment to port 1 */
        uint8_t ports[2] = { 0 };
        esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1,
                                                    ports, sizeof(ports),
                                                    EXPANDER_I2C_TIMEOUT);
        if (err == ESP_OK) {
            if (!have_last) {
                have_last = true;
                ESP_LOGI(TAG, "expander_watch addr=0x%02x port0=0x%02x port1=0x%02x (initial)",
                         EXPANDER_ADDR, ports[0], ports[1]);
            } else if (ports[0] != last[0] || ports[1] != last[1]) {
                uint32_t now_ms =
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "expander_change ms=%" PRIu32
                              " port0=0x%02x->0x%02x port1=0x%02x->0x%02x",
                         now_ms, last[0], ports[0], last[1], ports[1]);
            }
            last[0] = ports[0];
            last[1] = ports[1];
        }
        vTaskDelay(pdMS_TO_TICKS(EXPANDER_POLL_MS));
    }
}

static void expander_watch_start(void)
{
    i2c_master_bus_handle_t bus = badge_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "expander_watch skipped: I2C bus not initialised");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = EXPANDER_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "expander_watch attach 0x%02x failed: %s",
                 EXPANDER_ADDR, esp_err_to_name(err));
        return;
    }
    xTaskCreate(expander_watch_task, "exp_watch", 3072, dev, 5, NULL);
}
#endif /* GPIO_DEBUG_INPUT_ONLY */

void gpio_debug_watch(void)
{
#if GPIO_DEBUG_INPUT_ONLY
    expander_watch_start();
#endif
    s_event_queue = xQueueCreate(WATCH_QUEUE_LEN, sizeof(gpio_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "event_queue_create failed");
        return;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return;
    }

    for (size_t i = 0; i < N_SAFE_GPIOS; i++) {
        const gpio_probe_t *probe = &SAFE_GPIOS[i];
        gpio_num_t pin = probe->num;

        gpio_pullup_t pull_up = watch_pullup_for_probe(probe);
        configure_watch_pin(probe);

        err = gpio_isr_handler_add(pin, gpio_isr_handler,
                                   (void *)(uintptr_t)pin);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gpio_isr_handler_add GPIO%d failed: %s",
                     pin, esp_err_to_name(err));
        }
        s_last_levels[i] = gpio_get_level(pin);

        ESP_LOGI(TAG, "gpio_watch gpio=%d initial=%d pullup=%s strap=%s note=\"%s\"",
                 pin, s_last_levels[i],
                 pull_up == GPIO_PULLUP_ENABLE ? "yes" : "no",
                 probe->strapping ? "yes" : "no", probe->note);
    }

    xTaskCreate(watch_task, "gpio_watch", WATCH_TASK_STACK, NULL, 5, NULL);
    xTaskCreate(sampler_task, "gpio_sample", SAMPLE_TASK_STACK, NULL, 4, NULL);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
