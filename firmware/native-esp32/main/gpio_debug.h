#pragma once

/**
 * gpio_debug.h — GPIO probe / change-detector for hardware bring-up.
 *
 * Two phases:
 *
 *  1. gpio_debug_scan()
 *     Iterates every safe ESP32 GPIO, reads it in three resistor states
 *     (floating / pull-up / pull-down) and prints a summary table.
 *     Pins that read differently under pull-up vs pull-down are likely
 *     *floating* inputs; pins that stay the same regardless are *driven*.
 *
 *  2. gpio_debug_watch()
 *     Configures every safe GPIO as input, attaches interrupts, logs every
 *     edge, and periodically samples GPIO/ADC values so you can press buttons
 *     or change power state and identify unknowns in real time.
 *     This function never returns.
 */

void gpio_debug_scan(void);
void gpio_debug_watch(void);
