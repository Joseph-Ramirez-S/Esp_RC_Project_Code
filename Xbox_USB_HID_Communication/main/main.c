#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "esp_now_sender.h"
#include "shared_protocol.h"
#include "driver/gpio.h"

control_packet_t send_data_to_receiver;

// MAC del receptor RC (expuesta para uso por el módulo ESP-NOW)
uint8_t mac_RC_receiver[6] = {0x78, 0x1C, 0x3C, 0x2D, 0x37, 0x84};

static const char *TAG = "XBOX_HOST";
static usb_host_client_handle_t client_hdl;
static usb_device_handle_t dev_hdl_global = NULL;
static usb_transfer_t *in_transfer = NULL;

// ==============================================================================
// VARIABLES GLOBALES (Comunicación entre Callback IN y Bucle Main)
// ==============================================================================
static volatile bool send_color_update = false;

// Variables individuales para cada color RGB (0 a 255)
static volatile uint8_t target_r = 0x00;
static volatile uint8_t target_g = 0x00;
static volatile uint8_t target_b = 0x00;

// Estado anterior de los botones para evitar saturación del bus USB
static bool x_was_pressed = false; 
static bool y_was_pressed = false;
static bool a_was_pressed = false;
static bool b_was_pressed = false;

static bool dpad_up_was_pressed = false;
static bool dpad_down_was_pressed = false;
static bool dpad_left_was_pressed = false;
static bool dpad_right_was_pressed = false;

// Definición de la zona muerta para absorber la imprecisión del centro físico
#define JOYSTICK_DEADZONE 2500

// ==============================================================================
// FUNCIÓN DE PROCESAMIENTO Y ESCALADO DE JOYSTICK
// ==============================================================================
static uint16_t process_joystick(int16_t raw_val)
{
    if (raw_val > JOYSTICK_DEADZONE) {
        return 128 + ((int32_t)(raw_val - JOYSTICK_DEADZONE) * 128) / (32767 - JOYSTICK_DEADZONE);
    }
    else if (raw_val < -JOYSTICK_DEADZONE) {
        return 128 - ((int32_t)(raw_val + JOYSTICK_DEADZONE) * 128) / (-32768 + JOYSTICK_DEADZONE);
    }
    else {
        return 128;
    }
}

// ==============================================================================
// CALLBACK DE SALIDA (OUT)
// ==============================================================================
static void out_transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if(transfer->num_bytes == 5) {
            ESP_LOGI(TAG, "🌟 PAQUETE MÁGICO ENVIADO (Wake Up).");
        } else if (transfer->num_bytes == 9) { // Ajustado a los 9 bytes descubiertos
            ESP_LOGI(TAG, "🎨 Comando de color enviado con éxito.");
        }
    } else {
        ESP_LOGE(TAG, "Fallo al enviar OUT transfer. Estado: %d", transfer->status);
    }
    usb_host_transfer_free(transfer);
}
// ==============================================================================
// FUNCIÓN PARA ENVIAR VIBRACION
// ==============================================================================
static void send_rumble()
{
    if (!dev_hdl_global) return;

    usb_transfer_t *rumble_transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(64, 0, &rumble_transfer);
    if (err != ESP_OK) return;

    rumble_transfer->device_handle = dev_hdl_global;
    rumble_transfer->bEndpointAddress = 0x02; 
    rumble_transfer->callback = out_transfer_cb;
    rumble_transfer->context = NULL;
    rumble_transfer->num_bytes = 13; 

    uint8_t payload[13] = {0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00, 0x40, 0x40, 0x48, 0x00, 0x00};
    memcpy(rumble_transfer->data_buffer, payload, 13);

    usb_host_transfer_submit(rumble_transfer);
}

// ==============================================================================
// FUNCIÓN PARA ENVIAR COLOR (Bytes crudos de 9 bytes)
// ==============================================================================
static void set_controller_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!dev_hdl_global) return;

    usb_transfer_t *color_transfer = NULL;
    // Asignamos memoria para la transferencia
    esp_err_t err = usb_host_transfer_alloc(64, 0, &color_transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error asignando memoria para transferencia RGB");
        return;
    }

    color_transfer->device_handle = dev_hdl_global;
    color_transfer->bEndpointAddress = 0x02; // Endpoint OUT
    color_transfer->callback = out_transfer_cb;
    color_transfer->context = NULL;
    color_transfer->num_bytes = 9; // El tamaño exacto de tu captura

    // Matriz de 9 bytes con inyección de RGB
    uint8_t payload[9] = {
        0x0e, 0x00, 0x00, 0x05, 0x00, 0x00, // Cabecera
        r,                                  // Byte 6: Rojo
        g,                                  // Byte 7: Verde
        b                                   // Byte 8: Azul
    };
    
    // Copiamos la matriz al buffer de transferencia de USB
    memcpy(color_transfer->data_buffer, payload, 9);
    
    // Enviamos el comando al mando
    esp_err_t submit_err = usb_host_transfer_submit(color_transfer);
    if (submit_err != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando comando RGB: %s", esp_err_to_name(submit_err));
        usb_host_transfer_free(color_transfer);
    }
}

// ==============================================================================
// CALLBACK DE ENTRADA (IN)
// ==============================================================================
static void in_transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        uint8_t *data = transfer->data_buffer;
        int length = transfer->actual_num_bytes;

        if (length >= 18 && data[0] == 0x20) {
            
            // --- Mapeo de Botones ---
            bool btn_start  = data[4] & 0x04; 
            bool btn_select = data[4] & 0x08; 
            bool btn_a      = data[4] & 0x10;
            bool btn_b      = data[4] & 0x20;
            bool btn_x      = data[4] & 0x40;
            bool btn_y      = data[4] & 0x80;

            bool dpad_up    = data[5] & 0x01;
            bool dpad_down  = data[5] & 0x02;
            bool dpad_left  = data[5] & 0x04;
            bool dpad_right = data[5] & 0x08;
            bool btn_l1     = data[5] & 0x10; 
            bool btn_r1     = data[5] & 0x20; 
            bool btn_l3     = data[5] & 0x40; 
            bool btn_r3     = data[5] & 0x80; 

            uint16_t trigger_l2 = (data[7] << 8) | data[6];
            uint16_t trigger_r2 = (data[9] << 8) | data[8];

            int16_t raw_l_x = (data[11] << 8) | data[10];
            int16_t raw_l_y = (data[13] << 8) | data[12];
            int16_t raw_r_x = (data[15] << 8) | data[14];
            int16_t raw_r_y = (data[17] << 8) | data[16];

            uint8_t joy_l_x = (uint8_t) process_joystick(raw_l_x);
            uint8_t joy_l_y = (uint8_t) process_joystick(raw_l_y);
            uint8_t joy_r_x = (uint8_t) process_joystick(raw_r_x);
            uint8_t joy_r_y = (uint8_t) process_joystick(raw_r_y);

            // Actualizamos el paquete que se va a enviar por ESP-NOW
            uint16_t button_mask = 0;
            button_mask |= btn_a      ? (1 << 0) : 0;
            button_mask |= btn_b      ? (1 << 1) : 0;
            button_mask |= btn_x      ? (1 << 2) : 0;
            button_mask |= btn_y      ? (1 << 3) : 0;
            button_mask |= btn_start  ? (1 << 4) : 0;
            button_mask |= btn_select ? (1 << 5) : 0;
            button_mask |= dpad_up    ? (1 << 6) : 0;
            button_mask |= dpad_down  ? (1 << 7) : 0;
            button_mask |= dpad_left  ? (1 << 8) : 0;
            button_mask |= dpad_right ? (1 << 9) : 0;
            button_mask |= btn_l1     ? (1 << 10) : 0;
            button_mask |= btn_r1     ? (1 << 11) : 0;
            button_mask |= btn_l3     ? (1 << 12) : 0;
            button_mask |= btn_r3     ? (1 << 13) : 0;

            send_data_to_receiver.joy_lx   = joy_l_x;
            send_data_to_receiver.joy_ly   = joy_l_y;
            send_data_to_receiver.joy_rx   = joy_r_x;
            send_data_to_receiver.joy_ry   = joy_r_y;
            send_data_to_receiver.trigger_l = trigger_l2;
            send_data_to_receiver.trigger_r = trigger_r2;
            send_data_to_receiver.buttons  = button_mask;
            esp_now_sender_update_data(&send_data_to_receiver);

            // =========================================================
            // Lógica Discreta de Botones (Colores RGB puros)
            // =========================================================
            if (btn_x && !x_was_pressed) {
                ESP_LOGW(TAG, "X - ROJO");
                target_r = 0xFF; target_g = 0x00; target_b = 0x00; 
                send_color_update = true;
            } else if(btn_y && !y_was_pressed){
                ESP_LOGW(TAG, "Y - AZUL");  
                target_r = 0x00; target_g = 0x00; target_b = 0xFF; 
                send_color_update = true;
            } else if(btn_b && !b_was_pressed){
                ESP_LOGW(TAG, "B - VERDE");  
                target_r = 0x00; target_g = 0xFF; target_b = 0x00; 
                send_color_update = true;
            } else if(btn_a && !a_was_pressed){
                ESP_LOGW(TAG, "A - BLANCO");  
                target_r = 0xFF; target_g = 0xFF; target_b = 0xFF; 
                send_color_update = true;
            } else if(dpad_up && !dpad_up_was_pressed){
                ESP_LOGW(TAG, "UP - VERDE AZULEJO (CYAN)");  
                target_r = 0x00; target_g = 0xFF; target_b = 0xFF; 
                send_color_update = true;
            } else if(dpad_down && !dpad_down_was_pressed){
                ESP_LOGW(TAG, "DOWN - MORADO");  
                target_r = 0x80; target_g = 0x00; target_b = 0x80; 
                send_color_update = true;
            } else if(dpad_left && !dpad_left_was_pressed){
                ESP_LOGW(TAG, "LEFT - AMARILLO");  
                target_r = 0xFF; target_g = 0xFF; target_b = 0x00; 
                send_color_update = true;
            } else if(dpad_right && !dpad_right_was_pressed){
                ESP_LOGW(TAG, "RIGHT - VERDE LIMA");  
                target_r = 0x80; target_g = 0xFF; target_b = 0x00; 
                send_color_update = true;
            } else if(btn_start || btn_select){
                send_rumble();
            }
            //enviar los datos en esta funcion
            // Actualizamos estados pasados para TODAS las teclas
            x_was_pressed = btn_x;
            y_was_pressed = btn_y;
            a_was_pressed = btn_a;
            b_was_pressed = btn_b;
            dpad_up_was_pressed = dpad_up;
            dpad_down_was_pressed = dpad_down;
            dpad_left_was_pressed = dpad_left;
            dpad_right_was_pressed = dpad_right;

            // --- Impresión Limpia y Compacta en Consola ---
            printf("\rL-Joy X:%3u Y:%3u [%d] | R-Joy X:%3u Y:%3u [%d] | L2:%5u R2:%5u | L1:%d R1:%d|DPAD:[%d%d%d%d]|A:%d B:%d X:%d Y:%d|ST:%d SL:%d", 
                   joy_l_x, joy_l_y, btn_l3,
                   joy_r_x, joy_r_y, btn_r3,
                   trigger_l2, trigger_r2,
                   btn_l1, btn_r1,
                   dpad_up, dpad_down, dpad_left, dpad_right,
                   btn_a, btn_b, btn_x, btn_y,
                   btn_start, btn_select);
            fflush(stdout); 
        }

        usb_host_transfer_submit(transfer);
        
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "Dispositivo desconectado. Deteniendo lectura IN.");
    } else {
        usb_host_transfer_submit(transfer); 
    }
}

// ==============================================================================
// CALLBACK CENTRAL DEL CLIENTE USB
// ==============================================================================
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "🟢 Control Detectado. Abriendo dispositivo...");
        
        esp_err_t err = usb_host_device_open(client_hdl, msg->new_dev.address, &dev_hdl_global);
        if (err != ESP_OK) return;

        err = usb_host_interface_claim(client_hdl, dev_hdl_global, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error reclamando Interfaz 0: %s", esp_err_to_name(err));
            return;
        }

        // Transferencia OUT Inicial (Paquete Mágico)
        usb_transfer_t *out_transfer = NULL;
        err = usb_host_transfer_alloc(64, 0, &out_transfer);
        if (err == ESP_OK) {
            out_transfer->device_handle = dev_hdl_global;
            out_transfer->bEndpointAddress = 0x02; 
            out_transfer->callback = out_transfer_cb;
            out_transfer->context = NULL;
            out_transfer->num_bytes = 5;
            uint8_t magic[] = {0x05, 0x20, 0x00, 0x01, 0x00};
            memcpy(out_transfer->data_buffer, magic, 5);
            usb_host_transfer_submit(out_transfer);
        }

        // Transferencia IN Continua (Lectura)
        err = usb_host_transfer_alloc(64, 0, &in_transfer);
        if (err == ESP_OK) {
            in_transfer->device_handle = dev_hdl_global;
            in_transfer->bEndpointAddress = 0x82; 
            in_transfer->callback = in_transfer_cb;
            in_transfer->context = NULL;
            in_transfer->num_bytes = 64; 
            
            ESP_LOGI(TAG, "🎧 Iniciando escucha continua del control...");
            usb_host_transfer_submit(in_transfer);
        } else {
            ESP_LOGE(TAG, "Fallo al asignar memoria para lectura IN");
        }

    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "🔴 Control desconectado.");
        if (dev_hdl_global) {
            if (in_transfer) {
                usb_host_transfer_free(in_transfer);
                in_transfer = NULL;
            }
            usb_host_device_close(client_hdl, dev_hdl_global);
            dev_hdl_global = NULL;
        }
    }
}
// ==============================================================================
// FUNCIÓN PRINCIPAL
// ==============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "=== Sistema Xbox Host Iniciado ===");
    esp_now_sender_init(mac_RC_receiver);

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async.client_event_callback = client_event_cb,
        .async.callback_arg = NULL,
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_hdl));

    // =====================================================
    // LOOP PRINCIPAL
    // =====================================================

    // Variable para almacenar telemetría recibida del receptor
    telemetry_packet_t received_telemetry = {0};
    uint8_t prev_rumble_trigger = 0x00;

    while (1) {
        uint32_t event_flags;
        usb_host_client_handle_events(client_hdl, pdMS_TO_TICKS(10));
        usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        
        // =====================================================
        // VERIFICAR TELEMETRÍA DEL RECEPTOR Y ACTIVAR RUMBLE
        // =====================================================
        if (esp_now_sender_read_telemetry(&received_telemetry)) {
            // Si la bandera de rumble cambió de 0 a 1, activar vibraci ón
            if (received_telemetry.rumble_trigger && !prev_rumble_trigger) {
                ESP_LOGW(TAG, "🎮 KNOCK SENSOR DETECTADO EN CARRO - Activando vibraci ón del mando");
                send_rumble();
            }
            prev_rumble_trigger = received_telemetry.rumble_trigger;
        }
        
        // =====================================================
        // PROCESAMIENTO DE ENVÍO DE DATOS DESDE EL MAIN
        // =====================================================
        if (send_color_update) {
            send_color_update = false; // Limpiamos la bandera inmediatamente
            
            // Inyectamos el color directamente con los bytes RGB descubiertos
            set_controller_rgb(target_r, target_g, target_b);
        }
    }
}