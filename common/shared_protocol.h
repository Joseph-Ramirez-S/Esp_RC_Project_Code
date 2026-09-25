#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

#include <stdint.h>

// Paquete que el Mando (Sender) le envía al Carro (Receiver)
typedef struct __attribute__((packed)) {
    uint8_t joy_lx;     // Joystick Izquierdo X (0-255)
    uint8_t joy_ly;     // Joystick Izquierdo Y (0-255)
    uint8_t joy_rx;     // Joystick Derecho X (0-255)
    uint8_t joy_ry;     // Joystick Derecho Y (0-255)
    uint16_t trigger_l; // Gatillo Izquierdo Raw
    uint16_t trigger_r; // Gatillo Derecho Raw
    uint16_t buttons;   // Máscara de bits para los botones
} control_packet_t;

// Paquete que el Carro (Receiver) le devuelve al Mando (Telemetría)
typedef struct __attribute__((packed)) {
    float battery_voltage;  // Voltaje de la batería del carro
    uint16_t distance_cm;   // Distancia del sensor de proximidad
    uint8_t status_flags;   // Estados (luces encendidas, alarmas, etc.)
    uint8_t rumble_trigger; // Bandera para activar vibración (KY-031 knock sensor)
} telemetry_packet_t;

#endif