#ifndef DIRECTIONS_INTERS_H
#define DIRECTIONS_INTERS_H

#include <stdint.h>
#include <stdbool.h>

// GPIO pins for directional and intermittent lights
#define DIR_LEFT_GPIO GPIO_NUM_32
#define DIR_RIGHT_GPIO GPIO_NUM_33

// DPAD data masks
#define DPAD_LEFT_BIT    (1 << 8)   // bit 8 from buttons
#define DPAD_RIGHT_BIT   (1 << 9)   // bit 9 from buttons
#define DPAD_UP_BIT      (1 << 6)   // bit 6 from buttons (for intermittent)
#define DPAD_DOWN_BIT    (1 << 7)   // bit 7 from buttons (for intermittent)

/**
 * Initialize GPIO pins and start the directions / intermitentes task on core 1.
 */
void directions_inters_init(void);

/**
 * Enqueue DPAD values for processing by the directions task.
 * Returns true if the message was queued successfully.
 */
bool directions_inters_send_buttons(uint16_t buttons);

/**
 * Mark that the hazard/intermittent lights were activated by the knock sensor.
 * If the lights are off, this will turn them on. If they are already on,
 * this only records the activation source.
 */
void directions_inters_mark_knock_activation(void);

/**
 * If the hazard/intermittent lights were activated by the knock sensor,
 * turn them off and clear the knock-activation marker. Returns true if
 * the lights were cleared.
 */
bool directions_inters_try_clear_knock_activation(void);

/**
 * Query whether the hazard lights are currently marked as activated by
 * the knock sensor.
 */
bool directions_inters_is_activated_by_knock(void);
#endif // DIRECTIONS_INTERS_H
