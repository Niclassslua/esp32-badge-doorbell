#pragma once

#include "esp_err.h"

/**
 * Callback invoked from the touch task on each finger-down rising edge
 * (count 0 -> >=1). Runs in the touch task context; may take a few ms.
 */
typedef void (*badge_touch_press_cb_t)(void);

/**
 * Register the press callback. Pass NULL to disable. Call before
 * badge_touch_init() or any time after — the touch task picks up the
 * latest pointer on the next poll.
 */
void badge_touch_set_press_cb(badge_touch_press_cb_t cb);

/**
 * Attach the IQS550 on the shared I2C bus and spawn a polling task.
 * Non-fatal: returns the underlying error if the device cannot be
 * reached, and no task is started. Caller should log + carry on.
 */
esp_err_t badge_touch_init(void);
