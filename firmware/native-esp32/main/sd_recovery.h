#pragma once

#include "esp_err.h"
#include "esp_partition.h"

esp_err_t sd_recovery_install_app(const esp_partition_t *target);
