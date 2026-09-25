#ifndef ESP_NOW_RECEIVER_H
#define ESP_NOW_RECEIVER_H

#include "../../common/shared_protocol.h"
#include <stdbool.h>

// Inicializa ESP-NOWx en el carro y registra el mando como "Peer"
void esp_now_receiver_init(const uint8_t *sender_mac);

// Lee los últimos datos de control recibidos. Devuelve true si hay datos nuevos.
bool esp_now_receiver_read_control(control_packet_t *dest);

// Envía un paquete de telemetría de vuelta al mando (Voltaje, sensores, etc.)
void esp_now_receiver_send_telemetry(const telemetry_packet_t *telemetry);

// Devuelve true si el ESP32-S3 sender está conectado y ha enviado datos recientemente.
bool esp_now_receiver_is_connected(void);

#endif