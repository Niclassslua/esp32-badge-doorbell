#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"

/**
 * Stages emitted by the OTA progress callback. `percent` is only meaningful
 * for OTA_PROGRESS_DOWNLOADING (0..100); for other stages it is -1.
 * `detail` is an optional short string (server version, error name) or NULL.
 */
typedef enum {
    OTA_PROGRESS_CHECKING,        /* contacting server */
    OTA_PROGRESS_DOWNLOAD_START,  /* server build identified, download begins */
    OTA_PROGRESS_DOWNLOADING,     /* percent valid */
    OTA_PROGRESS_INSTALLING,      /* download finished, finalising flash */
    OTA_PROGRESS_SUCCESS,         /* about to esp_restart() */
    OTA_PROGRESS_FAILED,          /* aborted; detail = short reason */
} ota_progress_stage_t;

typedef void (*ota_progress_cb_t)(ota_progress_stage_t stage,
                                  int percent,
                                  const char *detail);

/**
 * Register (or clear with NULL) a single progress callback invoked from the
 * OTA task context. The callback must be non-blocking and re-entrant safe;
 * typical use is to enqueue a display update.
 */
void ota_update_set_progress_cb(ota_progress_cb_t cb);

/**
 * Check OTA_FIRMWARE_URL version headers, then download and flash only when
 * the served firmware differs from the running image.
 *
 * On success:  marks the new partition bootable and calls esp_restart().
 *              This function does NOT return.
 *
 * On failure:  logs the error and returns ESP_FAIL so the caller can
 *              continue normal operation with the current firmware.
 */
esp_err_t ota_update_check_and_apply(void);

/**
 * Run the same server version check, but write the downloaded app to an
 * explicit partition. Recovery uses this so WiFi OTA updates only replace
 * the normal app slot and never erase the recovery slot.
 */
esp_err_t ota_update_check_and_apply_to_partition(const esp_partition_t *update_part);

/**
 * Check server version metadata without downloading an image.
 */
bool ota_update_server_has_newer_firmware(void);

/**
 * If the server advertises a different firmware, select the recovery slot and
 * reboot. This is used by the normal app in the current two-slot layout because
 * it cannot safely update itself without overwriting recovery.
 */
esp_err_t ota_update_reboot_to_recovery_if_server_newer(const esp_partition_t *recovery_part);
