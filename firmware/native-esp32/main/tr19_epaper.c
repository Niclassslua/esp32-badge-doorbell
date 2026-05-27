#include "tr19_epaper.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "badge_config.h"
#include "badge_power.h"
#include "wifi_connect.h"

static const char *TAG = "tr22.epaper";

static bool s_epaper_initialized = false;
static bool s_epaper_partial_lut = false;
static bool s_recovery_layout_valid = false;
static char s_recovery_line1[32];
static char s_recovery_line2[32];

enum {
    EPD_PHYS_WIDTH = 128,
    EPD_PHYS_HEIGHT = 296,
    EPD_BUFFER_SIZE = EPD_PHYS_WIDTH * EPD_PHYS_HEIGHT / 8,
    EPD_BYTES_PER_ROW = EPD_PHYS_WIDTH / 8,

    SCREEN_WIDTH = EPD_PHYS_HEIGHT,
    SCREEN_HEIGHT = EPD_PHYS_WIDTH,

    EPD_PIN_SCK = GPIO_NUM_18,
    EPD_PIN_MOSI = GPIO_NUM_23,
    EPD_PIN_CS = GPIO_NUM_25,
    EPD_PIN_DC = GPIO_NUM_27,
    EPD_PIN_RST = GPIO_NUM_0,
    EPD_PIN_BUSY = GPIO_NUM_13,
};

#define DRIVER_OUTPUT_CONTROL                 0x01
#define BOOSTER_SOFT_START_CONTROL            0x0C
#define DATA_ENTRY_MODE_SETTING               0x11
#define MASTER_ACTIVATION                     0x20
#define DISPLAY_UPDATE_CONTROL_1              0x21
#define DISPLAY_UPDATE_CONTROL_2              0x22
#define WRITE_RAM                             0x24
#define WRITE_RAM_RED                         0x26
#define WRITE_VCOM_REGISTER                   0x2C
#define WRITE_LUT_REGISTER                    0x32
#define SET_DUMMY_LINE_PERIOD                 0x3A
#define SET_GATE_TIME                         0x3B
#define SET_RAM_X_ADDRESS_START_END_POSITION  0x44
#define SET_RAM_Y_ADDRESS_START_END_POSITION  0x45
#define SET_RAM_X_ADDRESS_COUNTER             0x4E
#define SET_RAM_Y_ADDRESS_COUNTER             0x4F
#define TERMINATE_FRAME_READ_WRITE            0xFF

static spi_device_handle_t epd_spi;
static uint8_t framebuffer[EPD_BUFFER_SIZE];

static const uint8_t lut_full[] = {
    0x02, 0x02, 0x01, 0x11, 0x12, 0x12, 0x22, 0x22, 0x66, 0x69,
    0x69, 0x59, 0x58, 0x99, 0x99, 0x88, 0x00, 0x00, 0x00, 0x00,
    0xF8, 0xB4, 0x13, 0x51, 0x35, 0x51, 0x51, 0x19, 0x01, 0x00,
};

static const uint8_t lut_partial[] = {
    0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x13, 0x14, 0x44, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define FONT_GLYPH_O_UMLAUT 0x80
#define FONT_GLYPH_COUNT    (FONT_GLYPH_O_UMLAUT + 1)

static const uint8_t font5x7[FONT_GLYPH_COUNT][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x00, 0x00, 0x5f, 0x00, 0x00},
    ['%'] = {0x62, 0x64, 0x08, 0x13, 0x23},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['0'] = {0x3e, 0x51, 0x49, 0x45, 0x3e},
    ['1'] = {0x00, 0x42, 0x7f, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4b, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7f, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3c, 0x4a, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1e},
    ['A'] = {0x7e, 0x11, 0x11, 0x11, 0x7e},
    ['B'] = {0x7f, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3e, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7f, 0x41, 0x41, 0x22, 0x1c},
    ['E'] = {0x7f, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7f, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3e, 0x41, 0x49, 0x49, 0x7a},
    ['H'] = {0x7f, 0x08, 0x08, 0x08, 0x7f},
    ['I'] = {0x00, 0x41, 0x7f, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3f, 0x01},
    ['K'] = {0x7f, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7f, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7f, 0x02, 0x0c, 0x02, 0x7f},
    ['N'] = {0x7f, 0x04, 0x08, 0x10, 0x7f},
    ['O'] = {0x3e, 0x41, 0x41, 0x41, 0x3e},
    ['P'] = {0x7f, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3e, 0x41, 0x51, 0x21, 0x5e},
    ['R'] = {0x7f, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7f, 0x01, 0x01},
    ['U'] = {0x3f, 0x40, 0x40, 0x40, 0x3f},
    ['V'] = {0x1f, 0x20, 0x40, 0x20, 0x1f},
    ['W'] = {0x3f, 0x40, 0x38, 0x40, 0x3f},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    [FONT_GLYPH_O_UMLAUT] = {0x38, 0x45, 0x44, 0x45, 0x38},
};

static esp_err_t epd_write(const uint8_t *data, size_t len)
{
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_transmit(epd_spi, &transaction);
}

static esp_err_t epd_command(uint8_t command)
{
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_CS, 0);
    esp_err_t err = epd_write(&command, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    return err;
}

static esp_err_t epd_data(uint8_t data)
{
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    esp_err_t err = epd_write(&data, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    return err;
}

static esp_err_t epd_data_buffer(const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    esp_err_t err = epd_write(data, len);
    gpio_set_level(EPD_PIN_CS, 1);
    return err;
}

static esp_err_t epd_wait_idle(TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD_PIN_BUSY) == 1) {
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            ESP_LOGE(TAG, "display busy timeout on GPIO%d", EPD_PIN_BUSY);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static esp_err_t epd_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    return epd_wait_idle(pdMS_TO_TICKS(5000));
}

static esp_err_t epd_init_bus(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << EPD_PIN_CS) | (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "configure output pins");
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_RST, 1);

    gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy), TAG, "configure busy pin");

    spi_bus_config_t bus_config = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = EPD_PIN_SCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = EPD_BUFFER_SIZE,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = GPIO_NUM_NC,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &device_config, &epd_spi), TAG, "add spi device");
    return ESP_OK;
}

static esp_err_t epd_send_lut(const uint8_t *lut)
{
    ESP_RETURN_ON_ERROR(epd_command(WRITE_LUT_REGISTER), TAG, "send lut command");
    for (size_t i = 0; i < 30; i++) {
        ESP_RETURN_ON_ERROR(epd_data(lut[i]), TAG, "send lut byte");
    }
    return ESP_OK;
}

static esp_err_t epd_use_partial_lut(bool partial)
{
    if (s_epaper_partial_lut == partial) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(epd_send_lut(partial ? lut_partial : lut_full),
                        TAG, "switch lut");
    s_epaper_partial_lut = partial;
    return ESP_OK;
}

static esp_err_t epd_init_panel(void)
{
    ESP_RETURN_ON_ERROR(epd_reset(), TAG, "reset display");
    ESP_RETURN_ON_ERROR(epd_command(DRIVER_OUTPUT_CONTROL), TAG, "driver output");
    ESP_RETURN_ON_ERROR(epd_data((EPD_PHYS_HEIGHT - 1) & 0xff), TAG, "height lo");
    ESP_RETURN_ON_ERROR(epd_data(((EPD_PHYS_HEIGHT - 1) >> 8) & 0xff), TAG, "height hi");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "scan mode");
    ESP_RETURN_ON_ERROR(epd_command(BOOSTER_SOFT_START_CONTROL), TAG, "booster");
    ESP_RETURN_ON_ERROR(epd_data(0xd7), TAG, "booster 1");
    ESP_RETURN_ON_ERROR(epd_data(0xd6), TAG, "booster 2");
    ESP_RETURN_ON_ERROR(epd_data(0x9d), TAG, "booster 3");
    ESP_RETURN_ON_ERROR(epd_command(WRITE_VCOM_REGISTER), TAG, "vcom");
    ESP_RETURN_ON_ERROR(epd_data(0xa8), TAG, "vcom value");
    ESP_RETURN_ON_ERROR(epd_command(SET_DUMMY_LINE_PERIOD), TAG, "dummy line");
    ESP_RETURN_ON_ERROR(epd_data(0x1a), TAG, "dummy line value");
    ESP_RETURN_ON_ERROR(epd_command(SET_GATE_TIME), TAG, "gate time");
    ESP_RETURN_ON_ERROR(epd_data(0x08), TAG, "gate time value");
    ESP_RETURN_ON_ERROR(epd_command(DATA_ENTRY_MODE_SETTING), TAG, "entry mode");
    ESP_RETURN_ON_ERROR(epd_data(0x03), TAG, "entry mode value");
    ESP_RETURN_ON_ERROR(epd_send_lut(lut_full), TAG, "send full lut");
    s_epaper_partial_lut = false;
    return ESP_OK;
}

static esp_err_t epd_set_memory_area_phys(int x_start, int y_start,
                                          int x_end, int y_end)
{
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_X_ADDRESS_START_END_POSITION), TAG, "x area");
    ESP_RETURN_ON_ERROR(epd_data((x_start >> 3) & 0xff), TAG, "x start");
    ESP_RETURN_ON_ERROR(epd_data((x_end >> 3) & 0xff), TAG, "x end");
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_Y_ADDRESS_START_END_POSITION), TAG, "y area");
    ESP_RETURN_ON_ERROR(epd_data(y_start & 0xff), TAG, "y start lo");
    ESP_RETURN_ON_ERROR(epd_data((y_start >> 8) & 0xff), TAG, "y start hi");
    ESP_RETURN_ON_ERROR(epd_data(y_end & 0xff), TAG, "y end lo");
    ESP_RETURN_ON_ERROR(epd_data((y_end >> 8) & 0xff), TAG, "y end hi");
    return ESP_OK;
}

static esp_err_t epd_set_memory_area(void)
{
    return epd_set_memory_area_phys(0, 0, EPD_PHYS_WIDTH - 1,
                                    EPD_PHYS_HEIGHT - 1);
}

static esp_err_t epd_set_memory_pointer_phys(int x, int y)
{
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_X_ADDRESS_COUNTER), TAG, "x pointer");
    ESP_RETURN_ON_ERROR(epd_data((x >> 3) & 0xff), TAG, "x pointer value");
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_Y_ADDRESS_COUNTER), TAG, "y pointer");
    ESP_RETURN_ON_ERROR(epd_data(y & 0xff), TAG, "y pointer lo");
    ESP_RETURN_ON_ERROR(epd_data((y >> 8) & 0xff), TAG, "y pointer hi");
    return epd_wait_idle(pdMS_TO_TICKS(5000));
}

static esp_err_t epd_set_memory_pointer(void)
{
    return epd_set_memory_pointer_phys(0, 0);
}

static esp_err_t epd_update(void)
{
    ESP_RETURN_ON_ERROR(epd_use_partial_lut(false), TAG, "full lut");
    /* Ensure the controller is in full-panel mode (not RED-RAM bypass). */
    ESP_RETURN_ON_ERROR(epd_command(DISPLAY_UPDATE_CONTROL_1), TAG, "duc1 full");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "duc1 full b1");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "duc1 full b2");
    ESP_RETURN_ON_ERROR(epd_set_memory_area(), TAG, "set memory area");
    ESP_RETURN_ON_ERROR(epd_set_memory_pointer(), TAG, "set memory pointer");
    ESP_RETURN_ON_ERROR(epd_command(WRITE_RAM), TAG, "write ram");
    ESP_RETURN_ON_ERROR(epd_data_buffer(framebuffer, sizeof(framebuffer)), TAG, "write framebuffer");
    /* Keep RED RAM in sync so the first partial update after this full
     * refresh has a valid baseline. */
    ESP_RETURN_ON_ERROR(epd_set_memory_pointer(), TAG, "reset pointer for old ram");
    ESP_RETURN_ON_ERROR(epd_command(WRITE_RAM_RED), TAG, "write old ram");
    ESP_RETURN_ON_ERROR(epd_data_buffer(framebuffer, sizeof(framebuffer)), TAG, "write old framebuffer");
    ESP_RETURN_ON_ERROR(epd_command(DISPLAY_UPDATE_CONTROL_2), TAG, "update control");
    ESP_RETURN_ON_ERROR(epd_data(0xc4), TAG, "update control value");
    ESP_RETURN_ON_ERROR(epd_command(MASTER_ACTIVATION), TAG, "master activation");
    ESP_RETURN_ON_ERROR(epd_command(TERMINATE_FRAME_READ_WRITE), TAG, "terminate frame");
    return epd_wait_idle(pdMS_TO_TICKS(30000));
}

static esp_err_t epd_update_logical_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > SCREEN_WIDTH) {
        w = SCREEN_WIDTH - x;
    }
    if (y + h > SCREEN_HEIGHT) {
        h = SCREEN_HEIGHT - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    int phys_x_min = EPD_PHYS_WIDTH - (y + h);
    int phys_x_max = EPD_PHYS_WIDTH - 1 - y;
    int phys_y_min = x;
    int phys_y_max = x + w - 1;
    int byte_start = phys_x_min / 8;
    int byte_end = phys_x_max / 8;
    int bytes_per_row = byte_end - byte_start + 1;

    /*
     * DISPLAY_UPDATE_CONTROL_1 byte 2 = 0x80 enables RED-RAM-bypass mode.
     * In this mode the controller applies the partial LUT directly to the
     * BLACK RAM (0x24) without performing a full-panel waveform cycle.
     * Without this register the controller falls back to full-panel drive
     * regardless of which LUT is loaded — which is what caused the whole-
     * screen flash that persisted even after loading lut_partial.
     * GxEPD2 and Waveshare's own SDK both set this register before every
     * partial refresh and clear it (0x00, 0x00) before full refreshes.
     */
    ESP_RETURN_ON_ERROR(epd_command(DISPLAY_UPDATE_CONTROL_1), TAG, "duc1 partial");
    ESP_RETURN_ON_ERROR(epd_data(0x00), TAG, "duc1 partial b1");
    ESP_RETURN_ON_ERROR(epd_data(0x80), TAG, "duc1 partial b2");
    ESP_RETURN_ON_ERROR(epd_use_partial_lut(true), TAG, "partial lut");
    ESP_RETURN_ON_ERROR(epd_set_memory_area_phys(byte_start * 8,
                                                 phys_y_min,
                                                 byte_end * 8 + 7,
                                                 phys_y_max),
                        TAG, "set partial memory area");
    /* Write new pixel data to the new-frame register (0x24). */
    ESP_RETURN_ON_ERROR(epd_set_memory_pointer_phys(byte_start * 8,
                                                    phys_y_min),
                        TAG, "set partial memory pointer");
    ESP_RETURN_ON_ERROR(epd_command(WRITE_RAM), TAG, "write partial ram");
    for (int row = phys_y_min; row <= phys_y_max; row++) {
        const uint8_t *src =
            &framebuffer[(size_t)row * EPD_BYTES_PER_ROW + byte_start];
        ESP_RETURN_ON_ERROR(epd_data_buffer(src, (size_t)bytes_per_row),
                            TAG, "write partial framebuffer");
    }
    /*
     * Mirror the same data into the old-frame register (0x26).  This
     * becomes the "old" baseline for the NEXT partial refresh, so the
     * controller can correctly identify only the pixels that change.
     * Without this, 0x26 lags behind by one full update and the
     * controller drives every pixel in the window on every refresh.
     */
    ESP_RETURN_ON_ERROR(epd_set_memory_pointer_phys(byte_start * 8,
                                                    phys_y_min),
                        TAG, "set partial old-ram pointer");
    ESP_RETURN_ON_ERROR(epd_command(WRITE_RAM_RED), TAG, "write partial old ram");
    for (int row = phys_y_min; row <= phys_y_max; row++) {
        const uint8_t *src =
            &framebuffer[(size_t)row * EPD_BYTES_PER_ROW + byte_start];
        ESP_RETURN_ON_ERROR(epd_data_buffer(src, (size_t)bytes_per_row),
                            TAG, "write partial old framebuffer");
    }
    ESP_RETURN_ON_ERROR(epd_command(DISPLAY_UPDATE_CONTROL_2),
                        TAG, "partial update control");
    /*
     * 0x0c = Display with Mode 1 (use LUT register) without re-enabling
     * clock/charge-pump.  0xc4 (the full-refresh byte) re-enables them
     * and triggers the full waveform sequence even with a partial LUT
     * loaded — which is why the whole screen was flashing.
     */
    ESP_RETURN_ON_ERROR(epd_data(0x0c), TAG, "partial update control value");
    ESP_RETURN_ON_ERROR(epd_command(MASTER_ACTIVATION),
                        TAG, "partial master activation");
    ESP_RETURN_ON_ERROR(epd_command(TERMINATE_FRAME_READ_WRITE),
                        TAG, "partial terminate frame");
    return epd_wait_idle(pdMS_TO_TICKS(10000));
}

static void fb_clear(bool white)
{
    memset(framebuffer, white ? 0xff : 0x00, sizeof(framebuffer));
}

static void fb_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) {
        return;
    }

    /*
     * Logical drawing coordinates are 296x128 landscape.  They are rotated
     * left into the physical 128x296 panel memory so the badge is upright
     * when the display is turned 90 degrees to the right.
     */
    int phys_x = EPD_PHYS_WIDTH - 1 - y;
    int phys_y = x;

    size_t index = (size_t)phys_x / 8 + (size_t)phys_y * EPD_BYTES_PER_ROW;
    uint8_t mask = 0x80 >> (phys_x % 8);
    if (black) {
        framebuffer[index] &= (uint8_t)~mask;
    } else {
        framebuffer[index] |= mask;
    }
}

static void fb_text(const char *text, int x, int y, int scale)
{
    int cursor = x;
    while (*text != '\0') {
        unsigned char c;
        const unsigned char *p = (const unsigned char *)text;
        if (p[0] == 0xc3 && (p[1] == 0x96 || p[1] == 0xb6)) {
            c = FONT_GLYPH_O_UMLAUT;
            text += 2;
        } else {
            c = *p;
            text++;
        }

        /* Check whether the character is within the font table and has at
         * least one non-zero column.  Checking only column 0 was wrong for
         * 'I', whose first column is 0 but which has pixels in columns 1-3. */
        bool renderable = (c < sizeof(font5x7) / sizeof(font5x7[0]));
        if (renderable) {
            renderable = false;
            for (int k = 0; k < 5; k++) {
                if (font5x7[c][k] != 0) { renderable = true; break; }
            }
        }
        if (!renderable) {
            c = ' ';
        }

        unsigned char glyph = c == FONT_GLYPH_O_UMLAUT ? 'O' : c;

        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if ((font5x7[glyph][col] >> row) & 0x01) {
                    for (int sx = 0; sx < scale; sx++) {
                        for (int sy = 0; sy < scale; sy++) {
                            fb_pixel(cursor + col * scale + sx, y + row * scale + sy, true);
                        }
                    }
                }
            }
        }

        if (c == FONT_GLYPH_O_UMLAUT) {
            int dot_y = y - (2 * scale);
            int dot_size = scale > 1 ? scale : 1;
            for (int dot = 0; dot < 2; dot++) {
                int dot_x = cursor + (dot == 0 ? scale : 3 * scale);
                for (int sx = 0; sx < dot_size; sx++) {
                    for (int sy = 0; sy < dot_size; sy++) {
                        fb_pixel(dot_x + sx, dot_y + sy, true);
                    }
                }
            }
        }
        cursor += 6 * scale;
    }
}

static int fb_text_width(const char *text, int scale)
{
    int glyphs = 0;
    while (*text != '\0') {
        const unsigned char *p = (const unsigned char *)text;
        if (p[0] == 0xc3 && (p[1] == 0x96 || p[1] == 0xb6)) {
            text += 2;
        } else {
            text++;
        }
        glyphs++;
    }
    return glyphs * 6 * scale;
}

static void uppercase_ascii(char *text)
{
    for (char *cp = text; *cp; cp++) {
        if (*cp >= 'a' && *cp <= 'z') {
            *cp = (char)(*cp - 32);
        }
    }
}

static void copy_text_glyphs(char *dst, size_t dst_len,
                             const char *src, int max_glyphs)
{
    size_t out = 0;
    int glyphs = 0;

    if (dst_len == 0) {
        return;
    }

    while (*src != '\0' && glyphs < max_glyphs && out + 1 < dst_len) {
        const unsigned char *p = (const unsigned char *)src;
        if (p[0] == 0xc3 && (p[1] == 0x96 || p[1] == 0xb6)) {
            if (out + 2 >= dst_len) {
                break;
            }
            dst[out++] = (char)p[0];
            dst[out++] = (char)p[1];
            src += 2;
        } else {
            dst[out++] = *src++;
        }
        glyphs++;
    }
    dst[out] = '\0';
}

static void fb_text_centered(const char *text, int y, int scale)
{
    int x = (SCREEN_WIDTH - fb_text_width(text, scale)) / 2;
    if (x < 0) {
        x = 0;
    }
    fb_text(text, x, y, scale);
}

static void fb_rect(int x, int y, int w, int h, bool black)
{
    for (int xx = x; xx < x + w; xx++) {
        fb_pixel(xx, y, black);
        fb_pixel(xx, y + h - 1, black);
    }
    for (int yy = y; yy < y + h; yy++) {
        fb_pixel(x, yy, black);
        fb_pixel(x + w - 1, yy, black);
    }
}

static void fb_fill_rect(int x, int y, int w, int h, bool black)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_pixel(xx, yy, black);
        }
    }
}

static void fb_battery_icon(int x, int y, int percent)
{
    const int body_w = 22;
    const int body_h = 10;
    const int nub_w = 3;
    const int fill_w = body_w - 4;

    fb_rect(x, y, body_w, body_h, true);
    fb_fill_rect(x + body_w, y + 3, nub_w, body_h - 6, true);

    if (percent >= 0) {
        int filled = (fill_w * percent + 50) / 100;
        if (filled > 0) {
            fb_fill_rect(x + 2, y + 2, filled, body_h - 4, true);
        }
    } else {
        fb_text("-", x + 8, y + 1, 1);
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

esp_err_t tr19_epaper_init(void)
{
    if (s_epaper_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initializing TR19-compatible ePaper on "
             "SCK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
             EPD_PIN_SCK, EPD_PIN_MOSI, EPD_PIN_CS,
             EPD_PIN_DC, EPD_PIN_RST, EPD_PIN_BUSY);

    ESP_RETURN_ON_ERROR(epd_init_bus(),   TAG, "init display bus");
    ESP_RETURN_ON_ERROR(epd_init_panel(), TAG, "init display panel");

    s_epaper_initialized = true;
    return ESP_OK;
}

/* Draw a solid horizontal line at row y (1 pixel tall). */
static void fb_hline(int y)
{
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        fb_pixel(x, y, true);
    }
}

/*
 * Door sign layout (296 x 128 landscape, rotated for right-hand use)
 * ----------------------------------------------------------------
 *  please ring    : "BITTE" / "KLINGELN" on two lines, large scale
 *  do not disturb : "NICHT" / "STÖREN" on two lines, large scale
 *  optional custom line between headline and footer
 *  footer   : IP and battery
 *
 * Two lines at scale 5 (each line is 7*5 = 35 px tall, characters are
 * 6*5 = 30 px wide). Longest label is "KLINGELN" (8 chars = 240 px),
 * fits the 296 px logical width with headroom.
 */
esp_err_t tr19_epaper_show_sign(bool please_ring, const char *custom_text)
{
    ESP_RETURN_ON_ERROR(tr19_epaper_init(), TAG, "init");

    s_recovery_layout_valid = false;

    fb_clear(true); /* white background */

    char ip[16];
    wifi_get_ip(ip, sizeof(ip));

    char ip_label[24];
    snprintf(ip_label, sizeof(ip_label), "IP %s", ip);

    int battery_percent = badge_power_get_battery_percent();
    char battery_label[16];
    if (battery_percent >= 0) {
        snprintf(battery_label, sizeof(battery_label), "%d%%", battery_percent);
    } else {
        snprintf(battery_label, sizeof(battery_label), "--%%");
    }

    char custom_line[40] = {0};
    bool has_custom_text = custom_text && custom_text[0] != '\0';
    if (has_custom_text) {
        copy_text_glyphs(custom_line, sizeof(custom_line), custom_text, 24);
        uppercase_ascii(custom_line);
    }

    int headline_scale = has_custom_text ? 4 : 5;
    int top_y = has_custom_text ? 8 : 16;
    int bottom_y = has_custom_text ? 44 : 61;
    if (please_ring) {
        fb_text_centered("BITTE", top_y, headline_scale);
        fb_text_centered("KLINGELN", bottom_y, headline_scale);
    } else {
        int dnd_top_y = has_custom_text ? 6 : 12;
        int dnd_bottom_y = has_custom_text ? 48 : 70;
        fb_text_centered("NICHT", dnd_top_y, headline_scale);
        fb_text_centered("STÖREN", dnd_bottom_y, headline_scale);
    }

    if (has_custom_text) {
        fb_text_centered(custom_line, 88, 2);
    }

    fb_hline(114);
    fb_hline(115);
    fb_text(ip_label, 6, 119, 1);

    /* Bottom-right corner: percentage label, then battery icon flush to the
     * right edge with a 2 px margin. Icon body+nub is 25 px wide. */
    const int icon_w = 25;
    const int margin = 2;
    const int gap = 4;
    int icon_x = SCREEN_WIDTH - icon_w - margin;
    int label_w = fb_text_width(battery_label, 1);
    int label_x = icon_x - gap - label_w;
    if (label_x < 0) {
        label_x = 0;
    }
    fb_text(battery_label, label_x, 119, 1);
    fb_battery_icon(icon_x, 117, battery_percent);

    ESP_RETURN_ON_ERROR(epd_update(), TAG, "update display");
    ESP_LOGI(TAG, "sign display updated (state=%s ip=%s battery=%s)",
             please_ring ? "please_ring" : "do_not_disturb", ip, battery_label);
    return ESP_OK;
}

esp_err_t tr19_epaper_show_hello(void)
{
    ESP_RETURN_ON_ERROR(tr19_epaper_init(), TAG, "init");

    s_recovery_layout_valid = false;
    fb_clear(true);
    fb_text_centered("HELLO", 24, 4);
    fb_text_centered("TR22", 64, 4);
    fb_text_centered("UPDATED", 100, 2);

    ESP_RETURN_ON_ERROR(epd_update(), TAG, "update display");
    ESP_LOGI(TAG, "hello display update complete");
    return ESP_OK;
}

esp_err_t tr19_epaper_show_recovery(const char *line1, const char *line2)
{
    ESP_RETURN_ON_ERROR(tr19_epaper_init(), TAG, "init");

    char upper1[32] = {0};
    char upper2[32] = {0};
    snprintf(upper1, sizeof(upper1), "%s", line1 ? line1 : "");
    snprintf(upper2, sizeof(upper2), "%s", line2 ? line2 : "");
    uppercase_ascii(upper1);
    uppercase_ascii(upper2);

    if (s_recovery_layout_valid &&
        strcmp(s_recovery_line1, upper1) == 0 &&
        strcmp(s_recovery_line2, upper2) != 0) {
        fb_fill_rect(0, 78, SCREEN_WIDTH, 34, false);
        fb_text_centered(upper2, 84, 2);
        ESP_RETURN_ON_ERROR(epd_update_logical_rect(0, 78, SCREEN_WIDTH, 34),
                            TAG, "partial recovery text update");
        snprintf(s_recovery_line2, sizeof(s_recovery_line2), "%s", upper2);
        ESP_LOGI(TAG, "recovery display partial update: %s / %s",
                 upper1, upper2);
        return ESP_OK;
    }
    if (s_recovery_layout_valid &&
        strcmp(s_recovery_line1, upper1) == 0 &&
        strcmp(s_recovery_line2, upper2) == 0) {
        return ESP_OK;
    }

    fb_clear(true);
    fb_hline(0);
    fb_hline(1);
    fb_text_centered("TR22 SAFE MODE", 8, 2);
    fb_hline(30);
    fb_hline(31);
    fb_text_centered(upper1, 48, 3);
    fb_text_centered(upper2, 84, 2);
    fb_hline(118);
    fb_hline(119);
    fb_text_centered("SD APP: /TR22/APP.BIN", 122, 1);

    ESP_RETURN_ON_ERROR(epd_update(), TAG, "update recovery display");
    s_recovery_layout_valid = true;
    snprintf(s_recovery_line1, sizeof(s_recovery_line1), "%s", upper1);
    snprintf(s_recovery_line2, sizeof(s_recovery_line2), "%s", upper2);
    ESP_LOGI(TAG, "recovery display updated: %s / %s", upper1, upper2);
    return ESP_OK;
}
