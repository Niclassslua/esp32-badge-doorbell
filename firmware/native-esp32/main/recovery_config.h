#pragma once

#include "driver/gpio.h"
#include "esp_partition.h"

/*
 * Current-layout recovery policy:
 *   ota_0 = minimal recovery firmware
 *   ota_1 = normal badge application
 *
 * This intentionally avoids a partition-table change. The tradeoff is that
 * normal app OTA updates must be applied by recovery so ota_0 is not erased.
 */
#define RECOVERY_SLOT_SUBTYPE       ESP_PARTITION_SUBTYPE_APP_OTA_0
#define RECOVERY_APP_SLOT_SUBTYPE   ESP_PARTITION_SUBTYPE_APP_OTA_1

#define RECOVERY_SD_MOUNT_POINT     "/sdcard"
#define RECOVERY_SD_APP_PATH        RECOVERY_SD_MOUNT_POINT "/TR22/app.bin"
#define RECOVERY_SD_LEGACY_APP_PATH RECOVERY_SD_MOUNT_POINT "/tr22_custom.bin"

/* GPIO0 is the ESP32 BOOT strap and is also reused as e-paper reset later. */
#define RECOVERY_BOOT_GPIO          GPIO_NUM_0
#define RECOVERY_BOOT_HOLD_MS       1500

/* SDMMC fixed pins on ESP32, confirmed from the original badge firmware. */
#define RECOVERY_SD_CMD_GPIO        GPIO_NUM_15
#define RECOVERY_SD_CLK_GPIO        GPIO_NUM_14
#define RECOVERY_SD_D0_GPIO         GPIO_NUM_2

#define RECOVERY_RETRY_DELAY_MS     5000
