#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} badge_led_color_t;

/**
 * Initialise the TR19-style WS2812 LED chain.
 *
 * Non-fatal if the hardware is not present; callers should log and continue.
 */
esp_err_t badge_led_init(void);

/**
 * Set all badge LEDs to the door-sign state color.
 *
 * please_ring=true  -> dark to save battery
 * please_ring=false -> red  ("Do not disturb")
 */
void badge_led_set_sign_state(bool please_ring);

/**
 * Override the steady sign-state color. Pass enabled=false to restore the
 * default power-saving colors.
 */
void badge_led_set_custom_color(bool enabled, badge_led_color_t color);

/**
 * Briefly raise LED brightness, then restore the door-sign state color.
 */
void badge_led_pulse_button_feedback(bool please_ring);

/**
 * Force all LEDs off regardless of custom color or sign state.
 * Used during night mode to eliminate LED current drain entirely.
 */
void badge_led_off(void);
