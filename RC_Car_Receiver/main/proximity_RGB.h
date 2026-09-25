// proximity_RGB.h
#ifndef PROXIMITY_RGB_H
#define PROXIMITY_RGB_H

#include <stdbool.h>
#include "esp_now_receiver.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the proximity + RGB module and start its FreeRTOS task. */
void proximity_RGB_init(void);

/**
 * Send a copy of the latest control packet to the proximity task for processing.
 * Returns true if the packet was queued successfully.
 */
bool proximity_RGB_send_control(const control_packet_t *ctl);

#ifdef __cplusplus
}
#endif

#endif // PROXIMITY_RGB_H
