#pragma once

/**
 * Return the current battery percentage, or -1 when no configured battery
 * source can be read on this hardware/configuration.
 *
 * Implementation uses a short cache window so callers may invoke this from
 * UI redraw paths without forcing a fresh measurement every time.
 */
int badge_power_get_battery_percent(void);

/**
 * Fire a short haptic buzz on the DRV2605. Safe to call from any task
 * context; returns quickly and the buzz itself is ~12 ms. No-op if the
 * DRV2605 is unreachable or already in use by a concurrent VBAT read.
 */
void badge_power_haptic_pulse(void);
