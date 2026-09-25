#include "esp_now_sender.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

static const char *TAG = "ESP_NOW_MOD";

// Variables internas del módulo ("wires" internos)
static uint8_t target_mac[6];
static control_packet_t current_tx_data;
static telemetry_packet_t current_rx_telemetry;

// Callback que se ejecuta automáticamente cuando el carro nos responde
/*
 * Callback de recepción de ESP-NOW (bidireccional)
 * - Recibe paquetes de telemetría desde el receptor (carro).
 * - Al recibir el tamaño esperado, guarda la última telemetría en
 *   `current_rx_telemetry` para que otras partes del firmware la consulten.
 */
static void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == sizeof(telemetry_packet_t)) {
        memcpy(&current_rx_telemetry, data, sizeof(telemetry_packet_t));
        ESP_LOGI(TAG, "Telemetría recibida desde el receptor");
    }
}

// Tarea cíclica de transmisión (Correrá en el Núcleo 0)
/*
 * Tarea de transmisión periódica
 * - Envía continuamente `current_tx_data` al receptor a ~50Hz (cada 20ms).
 * - `esp_now_sender_update_data()` actualiza `current_tx_data` desde el hilo USB/host.
 */
static void esp_now_tx_task(void *pvParameters) {
    ESP_LOGI(TAG, "Tarea de transmisión iniciada en el Núcleo %d", xPortGetCoreID());
    while(1) {
        esp_err_t result = esp_now_send(target_mac, (uint8_t *) &current_tx_data, sizeof(control_packet_t));
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Error al enviar paquete: %s", esp_err_to_name(result));
        }
        // Transmitir cada 20 ms (50 Hz), adecuado para control en tiempo real
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Función pública para que el código del USB actualice los joysticks
/*
 * Actualiza los datos de control que se envían por aire.
 * Esta función debe llamarse desde el contexto del USB/host cuando
 * haya nueva información de los joysticks/buttons.
 */
void esp_now_sender_update_data(const control_packet_t *new_data) {
    memcpy(&current_tx_data, new_data, sizeof(control_packet_t));
}

// Función pública para leer telemetría del receptor
/*
 * Lee la última telemetría recibida desde el receptor.
 * Retorna true si hay datos disponibles, false si aún no se ha recibido nada.
 */
bool esp_now_sender_read_telemetry(telemetry_packet_t *out_telemetry) {
    if (out_telemetry == NULL) return false;
    memcpy(out_telemetry, &current_rx_telemetry, sizeof(telemetry_packet_t));
    return true;
}

// Inicialización completa del módulo
void esp_now_sender_init(const uint8_t *receiver_mac) {
    // Guardamos la MAC del receptor internamente
    memcpy(target_mac, receiver_mac, 6);
    // 1. Inicializar NVS (Requerido por el Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Inicializar la pila de Wi-Fi en modo de bajo nivel
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 3. Inicializar ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    
    // Registrar el callback de recepción (para la bidireccionalidad)
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // 4. Registrar al Carro como un "Peer" (compañero de comunicación)
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, target_mac, 6);
    peer_info.channel = 1; // Mismo canal Wi-Fi
    peer_info.encrypt = false;
    
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
    ESP_LOGI(TAG, "ESP-NOW Inicializado correctamente.");

    // 5. ¡Instanciamos la tarea en el NÚCLEO 0!
    xTaskCreatePinnedToCore(
        esp_now_tx_task,    // Función de la tarea
        "esp_now_tx_task",  // Nombre
        4096,               // Tamaño del Stack
        NULL,               // Parámetros
        5,                  // Prioridad (Media-Alta)
        NULL,               // Handle
        0                   // <--- Forzado al CORE 0
    );
}