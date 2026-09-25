#include "esp_now_receiver.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "string.h"

#define LINK_LOSS_TIMEOUT_MS 1000
#define LINK_RECOVERY_WINDOW_MS 7000
#define LINK_RECOVERY_RETRY_MS 250

static const char *TAG = "ESP_NOW_RX_MOD";

// Variables internas del módulo
static uint8_t master_mac[6];
static control_packet_t received_control;
static bool new_data_available = false;
static volatile TickType_t master_last_rx_tick = 0;
static SemaphoreHandle_t data_mutex = NULL;
static SemaphoreHandle_t send_mutex = NULL;

static esp_err_t register_master_peer(void)
{
    if (esp_now_is_peer_exist(master_mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, master_mac, 6);
    peer_info.channel = 1;
    peer_info.encrypt = false;
    return esp_now_add_peer(&peer_info);
}

static void link_recovery_task(void *arg)
{
    telemetry_packet_t heartbeat = {
        .battery_voltage = 0.0f,
        .distance_cm = 0,
        .status_flags = 0x80,
        .rumble_trigger = 0,
    };

    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool link_lost = master_last_rx_tick != 0 &&
                         (now - master_last_rx_tick) >=
                             pdMS_TO_TICKS(LINK_LOSS_TIMEOUT_MS);
        if (link_lost) {
            TickType_t deadline = now +
                                  pdMS_TO_TICKS(LINK_RECOVERY_WINDOW_MS);
            ESP_LOGW(TAG, "Control link lost; recovering for %d seconds",
                     LINK_RECOVERY_WINDOW_MS / 1000);
            while (xTaskGetTickCount() < deadline &&
                   !esp_now_receiver_is_connected()) {
                esp_err_t peer_err = register_master_peer();
                if (peer_err == ESP_OK || peer_err == ESP_ERR_ESPNOW_EXIST) {
                    esp_err_t send_err = ESP_ERR_TIMEOUT;
                    if (xSemaphoreTake(send_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        send_err = esp_now_send(
                            master_mac, (uint8_t *)&heartbeat,
                            sizeof(heartbeat));
                        xSemaphoreGive(send_mutex);
                    }
                    if (send_err != ESP_OK) {
                        ESP_LOGW(TAG, "Recovery heartbeat failed: %s",
                                 esp_err_to_name(send_err));
                    }
                } else {
                    ESP_LOGW(TAG, "Peer recovery failed: %s",
                             esp_err_to_name(peer_err));
                }
                vTaskDelay(pdMS_TO_TICKS(LINK_RECOVERY_RETRY_MS));
            }
            if (!esp_now_receiver_is_connected()) {
                ESP_LOGE(TAG, "Control link was not restored in %d seconds",
                         LINK_RECOVERY_WINDOW_MS / 1000);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_RECOVERY_RETRY_MS));
    }
}

// Callback que se ejecuta inmediatamente por interrupción cuando el mando envía datos
static void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len == sizeof(control_packet_t) && memcmp(recv_info->src_addr, master_mac, 6) == 0) {
        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            memcpy(&received_control, data, sizeof(control_packet_t));
            new_data_available = true;
            master_last_rx_tick = xTaskGetTickCount();
            xSemaphoreGive(data_mutex);
        }
    }
}

bool esp_now_receiver_is_connected(void) {
    TickType_t last_rx = master_last_rx_tick;
    return last_rx != 0 &&
           (xTaskGetTickCount() - last_rx) < pdMS_TO_TICKS(1000);
}

// Función pública para que el bucle principal del carro lea el control remoto
bool esp_now_receiver_read_control(control_packet_t *dest) {
    bool has_new = false;
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(dest, &received_control, sizeof(control_packet_t));
        has_new = new_data_available;
        new_data_available = false; // Consumimos el dato
        xSemaphoreGive(data_mutex);
    }
    return has_new;
}

// Función pública para enviar la telemetría de regreso al ESP32-S3
void esp_now_receiver_send_telemetry(const telemetry_packet_t *telemetry) {
    if (send_mutex == NULL ||
        xSemaphoreTake(send_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "Telemetry send skipped: ESP-NOW transmitter busy");
        return;
    }
    esp_err_t result = esp_now_send(master_mac, (uint8_t *) telemetry,
                                    sizeof(telemetry_packet_t));
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Error enviando telemetría: %s", esp_err_to_name(result));
    }
    xSemaphoreGive(send_mutex);
}

// Inicialización del módulo receptor
void esp_now_receiver_init(const uint8_t *sender_mac) {
    memcpy(master_mac, sender_mac, 6);
    data_mutex = xSemaphoreCreateMutex();
    send_mutex = xSemaphoreCreateMutex();

    // 1. Inicializar almacenamiento NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Inicializar Wi-Fi en modo Estación (Requerido por ESP-NOW)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 3. Inicializar protocolo ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // 4. Registrar al Mando (Sender) como Peer para poder responderle
    ESP_ERROR_CHECK(register_master_peer());
    BaseType_t task_result = xTaskCreate(link_recovery_task, "espnow_recovery",
                                         3072, NULL, 3, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "ESP-NOW recovery task could not be created");
    }
    ESP_LOGI(TAG, "Receptor ESP-NOW enlazado al mando correctamente.");
}