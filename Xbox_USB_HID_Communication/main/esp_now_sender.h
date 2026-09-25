#ifndef ESP_NOW_SENDER_H
#define ESP_NOW_SENDER_H

#include <stdbool.h>
#include "../../common/shared_protocol.h"

// Global MAC provided by the application
extern uint8_t mac_RC_receiver[6];

// Función para inicializar la radio y ESP-NOW en el Core 0
void esp_now_sender_init(const uint8_t *receiver_mac);

// Función pública para actualizar los datos que queremos enviar
void esp_now_sender_update_data(const control_packet_t *new_data);

// Función pública para leer telemetría del receptor
bool esp_now_sender_read_telemetry(telemetry_packet_t *out_telemetry);

#endif