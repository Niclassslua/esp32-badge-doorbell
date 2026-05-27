#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * Initialize the shared I2C master bus that the badge peripherals (PCA9555
 * button expander, DRV2605 battery monitor, …) all sit on. Call once from
 * app_main before any I2C user. Subsequent calls are no-ops.
 */
esp_err_t badge_i2c_init(void);

/**
 * Return the shared bus handle, or NULL when init has not run or has failed.
 */
i2c_master_bus_handle_t badge_i2c_bus(void);
