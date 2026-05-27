#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"

const esp_partition_t *recovery_partition(void);
const esp_partition_t *recovery_app_partition(void);
bool recovery_boot_button_held(void);
esp_err_t recovery_boot_app_partition(const esp_partition_t *app_part);
esp_err_t recovery_reboot_to_recovery(void);
