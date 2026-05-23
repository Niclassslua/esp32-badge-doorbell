#include "tr19_epaper.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tr22.epaper";

enum {
    EPD_WIDTH = 128,
    EPD_HEIGHT = 296,
    EPD_BUFFER_SIZE = EPD_WIDTH * EPD_HEIGHT / 8,

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
#define DISPLAY_UPDATE_CONTROL_2              0x22
#define WRITE_RAM                             0x24
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

static const uint8_t font5x7[][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x00, 0x00, 0x5f, 0x00, 0x00},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
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

static esp_err_t epd_send_lut(void)
{
    ESP_RETURN_ON_ERROR(epd_command(WRITE_LUT_REGISTER), TAG, "send lut command");
    for (size_t i = 0; i < sizeof(lut_full); i++) {
        ESP_RETURN_ON_ERROR(epd_data(lut_full[i]), TAG, "send lut byte");
    }
    return ESP_OK;
}

static esp_err_t epd_init_panel(void)
{
    ESP_RETURN_ON_ERROR(epd_reset(), TAG, "reset display");
    ESP_RETURN_ON_ERROR(epd_command(DRIVER_OUTPUT_CONTROL), TAG, "driver output");
    ESP_RETURN_ON_ERROR(epd_data((EPD_HEIGHT - 1) & 0xff), TAG, "height lo");
    ESP_RETURN_ON_ERROR(epd_data(((EPD_HEIGHT - 1) >> 8) & 0xff), TAG, "height hi");
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
    return epd_send_lut();
}

static esp_err_t epd_set_memory_area(void)
{
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_X_ADDRESS_START_END_POSITION), TAG, "x area");
    ESP_RETURN_ON_ERROR(epd_data(0), TAG, "x start");
    ESP_RETURN_ON_ERROR(epd_data((EPD_WIDTH - 1) >> 3), TAG, "x end");
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_Y_ADDRESS_START_END_POSITION), TAG, "y area");
    ESP_RETURN_ON_ERROR(epd_data(0), TAG, "y start lo");
    ESP_RETURN_ON_ERROR(epd_data(0), TAG, "y start hi");
    ESP_RETURN_ON_ERROR(epd_data((EPD_HEIGHT - 1) & 0xff), TAG, "y end lo");
    ESP_RETURN_ON_ERROR(epd_data(((EPD_HEIGHT - 1) >> 8) & 0xff), TAG, "y end hi");
    return ESP_OK;
}

static esp_err_t epd_set_memory_pointer(void)
{
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_X_ADDRESS_COUNTER), TAG, "x pointer");
    ESP_RETURN_ON_ERROR(epd_data(0), TAG, "x pointer value");
    ESP_RETURN_ON_ERROR(epd_command(SET_RAM_Y_ADDRESS_COUNTER), TAG, "y pointer");
    ESP_RETURN_ON_ERROR(epd_data(0), TAG, "y pointer lo");
    ESP_RETURN_ON_ERROR(epd_data(0), TAG, "y pointer hi");
    return epd_wait_idle(pdMS_TO_TICKS(5000));
}

static esp_err_t epd_update(void)
{
    ESP_RETURN_ON_ERROR(epd_set_memory_area(), TAG, "set memory area");
    ESP_RETURN_ON_ERROR(epd_set_memory_pointer(), TAG, "set memory pointer");
    ESP_RETURN_ON_ERROR(epd_command(WRITE_RAM), TAG, "write ram");
    ESP_RETURN_ON_ERROR(epd_data_buffer(framebuffer, sizeof(framebuffer)), TAG, "write framebuffer");
    ESP_RETURN_ON_ERROR(epd_command(DISPLAY_UPDATE_CONTROL_2), TAG, "update control");
    ESP_RETURN_ON_ERROR(epd_data(0xc4), TAG, "update control value");
    ESP_RETURN_ON_ERROR(epd_command(MASTER_ACTIVATION), TAG, "master activation");
    ESP_RETURN_ON_ERROR(epd_command(TERMINATE_FRAME_READ_WRITE), TAG, "terminate frame");
    return epd_wait_idle(pdMS_TO_TICKS(30000));
}

static void fb_clear(bool white)
{
    memset(framebuffer, white ? 0xff : 0x00, sizeof(framebuffer));
}

static void fb_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
        return;
    }

    size_t index = (size_t)x / 8 + (size_t)y * (EPD_WIDTH / 8);
    uint8_t mask = 0x80 >> (x % 8);
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
        unsigned char c = (unsigned char)*text++;
        if (c >= sizeof(font5x7) / sizeof(font5x7[0]) || font5x7[c][0] == 0) {
            c = ' ';
        }

        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if ((font5x7[c][col] >> row) & 0x01) {
                    for (int sx = 0; sx < scale; sx++) {
                        for (int sy = 0; sy < scale; sy++) {
                            fb_pixel(cursor + col * scale + sx, y + row * scale + sy, true);
                        }
                    }
                }
            }
        }
        cursor += 6 * scale;
    }
}

esp_err_t tr19_epaper_show_hello(void)
{
    ESP_LOGI(TAG, "initializing TR19-compatible ePaper on SCK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
             EPD_PIN_SCK, EPD_PIN_MOSI, EPD_PIN_CS, EPD_PIN_DC, EPD_PIN_RST, EPD_PIN_BUSY);

    ESP_RETURN_ON_ERROR(epd_init_bus(), TAG, "init display bus");
    ESP_RETURN_ON_ERROR(epd_init_panel(), TAG, "init display panel");

    fb_clear(true);
    fb_text("HELLO", 16, 92, 4);
    fb_text("TR22", 22, 130, 4);
    fb_text("CUSTOM FW", 14, 174, 2);

    ESP_RETURN_ON_ERROR(epd_update(), TAG, "update display");
    ESP_LOGI(TAG, "display update complete");
    return ESP_OK;
}
