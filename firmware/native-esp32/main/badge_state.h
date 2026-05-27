#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "badge_led.h"

/*
 * badge_state — mutex-protected door-sign state with NVS persistence.
 *
 * All functions are safe to call from any FreeRTOS task.
 * The display is automatically refreshed whenever the state changes.
 */

typedef enum {
    SIGN_PLEASE_RING    = 0,
    SIGN_DO_NOT_DISTURB = 1,
} sign_state_t;

#define BADGE_LABEL_PLEASE_RING    "Bitte klingeln"
#define BADGE_LABEL_DO_NOT_DISTURB "Nicht Stören"

/**
 * Initialise NVS flash, load persisted custom text, and create the internal
 * mutex. The sign always starts in the please-ring/off-LED state.
 * Must be called once before any other badge_state_* function.
 */
esp_err_t badge_state_init(void);

/**
 * Return the current door-sign state (thread-safe read).
 */
sign_state_t badge_state_get_state(void);

/**
 * Set door-sign state, persist to NVS, and refresh the display.
 */
void badge_state_set_state(sign_state_t state);

/**
 * Copy the current custom text into buf (at most len bytes, NUL-terminated).
 */
void badge_state_get_custom_text(char *buf, size_t len);

/**
 * Set custom text, persist to NVS, and refresh the display.
 * text is truncated to BADGE_CUSTOM_TEXT_MAX-1 characters.
 */
void badge_state_set_custom_text(const char *text);

/**
 * Return true when a custom steady LED color is active.
 */
bool badge_state_get_led_color(badge_led_color_t *color);

/**
 * Set or clear the custom steady LED color.
 */
void badge_state_set_led_color(bool enabled, badge_led_color_t color);

/**
 * Refresh the display using the current state without changing persisted data.
 * Use this when non-persistent display metadata, such as WiFi IP, changes.
 */
void badge_state_refresh_display(void);
