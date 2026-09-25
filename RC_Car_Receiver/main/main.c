/*
 * @file main.c
 * @brief ESP-NOW RC car receiver application for ESP32.
 *
 * This module implements the main receiver logic for the RC Car.
 * It initializes ESP-NOW reception, directional lights, proximity/RGB feedback,
 * and a KY-031 knock sensor. The main task reads controller packets,
 * updates runtime state at 100 Hz, shows packet changes via an LED,
 * refreshes console status on each loop, and sends telemetry back to the sender.
 */

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_now_receiver.h"
#include "driver/gpio.h"
#include "directions_inters.h"
#include "proximity_RGB.h"
#include "servo_sweep.h"
#include "SD_sdmmc.h"
#include "shared_protocol.h"

#define LED_GPIO GPIO_NUM_27
#define KY_PIN GPIO_NUM_36 // KY-031 knock sensor input pin

static const char *TAG = "MAIN_CAR";
bool enable_telemetry = false;

// KY-031 knock sensor state (file-scope)
static QueueHandle_t ky_queue = NULL;
static TickType_t last_knock_tick = 0;
static TickType_t last_telemetry_send = 0;  // Track last telemetry send for immediate transmission on knock

// Telemetry packet (shared between tasks)
static telemetry_packet_t mi_telemetria = {
    .battery_voltage = 12.6, // Simulación de batería Full 3S Lipo
    .distance_cm = 150,
    .status_flags = 0x00,
    .rumble_trigger = 0x00   // Inicializado sin vibración
};

// ISR handler for KY-031 (very small, just notify queue)
static void IRAM_ATTR ky_isr_handler(void *arg)
{
    uint32_t one = 1;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (ky_queue) {
        xQueueSendFromISR(ky_queue, &one, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
    }
}

// Task to process knock events (runs in task context)
static void ky_task(void *arg)
{
    uint32_t evt;
    while (1) {
        // Espera de forma indefinida hasta que el ISR envíe un evento
        if (xQueueReceive(ky_queue, &evt, portMAX_DELAY) == pdTRUE) {
            
            // 1. Registra el tiempo y activa la alarma de las direccionales
            last_knock_tick = xTaskGetTickCount();
            directions_inters_mark_knock_activation();
            
            // 2. Activar vibración y ENVIAR TELEMETRÍA INMEDIATAMENTE
            mi_telemetria.rumble_trigger = 0x01;
            esp_now_receiver_send_telemetry(&mi_telemetria); // <--- Disparo instantáneo
            
            ESP_LOGI(TAG, "KY-031 knock: hazard activated + rumble telemetry SENT");

            // 3. Limpiamos la bandera para que el bucle normal de 1Hz no la vuelva a enviar
            mi_telemetria.rumble_trigger = 0x00;

            // 4. Debounce mecánico: Esperamos 200ms a que el resorte del sensor deje de vibrar
            // (Aumenté a 200ms porque 20ms es muy poco para un choque físico fuerte)
            vTaskDelay(pdMS_TO_TICKS(200));

            // 5. Limpiamos la cola para ignorar cualquier pulso de "rebote"
            xQueueReset(ky_queue);
        }
    }
}

static void print_uart_help(void)
{
    printf("Commands: 1=sndaL.wav 2=sndaR.wav 3=sndb.wav 4=sndc.wav "
           "5=music_A.wav 6=music_B.wav 7=music_C.wav\n"
           "          t=toggle telemetry h/?=show help\n");
    fflush(stdout);
}

static void uart_input_task(void *arg)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    while (1) {
        int character = fgetc(stdin);
        if (character != EOF) {
            if (character >= '1' && character <= '7') {
                uint8_t sound_number = (uint8_t)(character - '0');
                esp_err_t err = SD_play_sound(sound_number);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Sound %u selected", sound_number);
                } else {
                    ESP_LOGW(TAG, "Sound %u unavailable: %s", sound_number,
                             esp_err_to_name(err));
                }
            } else if (character == 't' || character == 'T') {
                enable_telemetry = !enable_telemetry;
                ESP_LOGI(TAG, "Joystick telemetry %s",
                         enable_telemetry ? "enabled" : "paused");
            } else if (character == 'h' || character == '?') {
                print_uart_help();
            } else if (character != '\r' && character != '\n' && character != ' ' &&
                       character != '\t') {
                ESP_LOGW(TAG, "Unknown command: %c (press h for help)", character);
            }
        } else {
            clearerr(stdin);
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                errno = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/*
 * @brief Main receiver task for the RC car.
 *
 * The task performs the following responsibilities:
 * - Initialize ESP-NOW reception and validate the sender MAC address.
 * - Initialize status LED, directional interlock outputs, RGB proximity module,
 *   and KY-031 knock sensor support.
 * - Read control packets from the sender at 100 Hz and keep the last known
 *   packet available when no update is received.
 * - Briefly pulse the status LED when a new, changed packet arrives.
 * - Forward button and stick data to the direction lights module immediately.
 * - Handle knock-based hazard activation and clear it when the A button is pressed.
 * - Send telemetry back to the sender approximately once per second.
 */
static void main_task(void *arg)
{
    //MAC ESP32-S3 (SENDER)
    uint8_t XBOX_MAC[6] = {0x44, 0x1B, 0xF6, 0x81, 0xEB, 0x74};

    // Inicializamos el LED en GPIO27 para indicar cambios de paquete
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

    // Inicializamos nuestro módulo receptor
    esp_now_receiver_init(XBOX_MAC);
    ESP_LOGI(TAG, "Receptor ESP-NOW inicializado con MAC del mando");

        ESP_LOGI(TAG, "Receptor ESP-NOW inicializado con MAC %02X:%02X:%02X:%02X:%02X:%02X",
              XBOX_MAC[0], XBOX_MAC[1], XBOX_MAC[2], XBOX_MAC[3], XBOX_MAC[4], XBOX_MAC[5]);
    
    // Inicializamos directional lights (GPIO32 y GPIO33)
    directions_inters_init();

    // Inicializamos el módulo proximity + RGB (crea su propia tarea)
    proximity_RGB_init();

    // Inicializamos los servos en RX2 (GPIO16) y TX2 (GPIO17)
    servo_sweep_init();

    // Mount 4-bit SDMMC and start PCM5102A WAV streaming/serial command tasks.
    if (SD_init() != ESP_OK) {
        ESP_LOGE(TAG, "SD card/audio initialization failed; sound commands disabled");
    }

    // Configure KY_PIN as input with rising-edge interrupt and start handler task
    ky_queue = xQueueCreate(10, sizeof(uint32_t));
    if (ky_queue) {
        gpio_config_t ky_conf = {
            .pin_bit_mask = 1ULL << KY_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE, 
            .intr_type = GPIO_INTR_POSEDGE,
        };
        gpio_config(&ky_conf);
        gpio_install_isr_service(0);
        gpio_isr_handler_add(KY_PIN, ky_isr_handler, (void *)KY_PIN);

        xTaskCreatePinnedToCore(ky_task, "ky_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL, 1);
        ESP_LOGI(TAG, "KY-031 knock sensor initialized on GPIO%d", KY_PIN);
    } else {
        ESP_LOGW(TAG, "Failed to create KY queue; knock sensor disabled");
    }

        control_packet_t Xbox_controller_data;
        control_packet_t prev_control = {0};
        TickType_t led_on_until = 0;

    int contador_telemetria = 0;

    while (1) {
        // Consultamos al módulo si el mando envió algo nuevo
        bool has_new_control = esp_now_receiver_read_control(&Xbox_controller_data);
        if (has_new_control) {
            // Si el paquete cambió respecto al anterior, encender LED brevemente
            if (memcmp(&Xbox_controller_data, &prev_control, sizeof(control_packet_t)) != 0) {
                led_on_until = xTaskGetTickCount() + pdMS_TO_TICKS(150);
                memcpy(&prev_control, &Xbox_controller_data, sizeof(control_packet_t));
            }
        } else {
            // No hay nuevo paquete: mantenemos el último estado conocido para actualizar siempre la pantalla
            memcpy(&Xbox_controller_data, &prev_control, sizeof(control_packet_t));
        }

        // Desempaquetar máscara de botones según el mapeo del sender
        uint16_t btns = Xbox_controller_data.buttons;
        bool rcv_a = btns & (1 << 0);
        bool rcv_b = btns & (1 << 1);
        bool rcv_x = btns & (1 << 2);
        bool rcv_y = btns & (1 << 3);
        bool rcv_start = btns & (1 << 4);
        bool rcv_select = btns & (1 << 5);
        bool rcv_dpad_up = btns & (1 << 6);
        bool rcv_dpad_down = btns & (1 << 7);
        bool rcv_dpad_left = btns & (1 << 8);
        bool rcv_dpad_right = btns & (1 << 9);
        bool rcv_l1 = btns & (1 << 10);
        bool rcv_r1 = btns & (1 << 11);
        bool rcv_l3 = btns & (1 << 12);
        bool rcv_r3 = btns & (1 << 13);
        
        // Procesar DPAD para directional lights (GPIO32 y GPIO33)
        // Enviar los botones al módulo de direccionales inmediatamente para evitar retrasos.
        // NOTE: The directions_inters module will ignore DPAD while knock-activated hazards are active.
        directions_inters_send_buttons(btns);

        // Handle A button: if hazards were activated by knock, A clears them
        static bool prev_a = false;
        if (rcv_a && !prev_a) {
            // rising edge of A
            // Only clear if knock activation recorded and 25 ms have passed
            if (last_knock_tick != 0) {
                if (xTaskGetTickCount() >= last_knock_tick + pdMS_TO_TICKS(25)) {
                    if (directions_inters_try_clear_knock_activation()) {
                        last_knock_tick = 0;
                        ESP_LOGI(TAG, "Hazard cleared by A (knock-activated)");
                    }
                }
            }
        }
        prev_a = rcv_a;

        // Forward the latest control packet to the proximity_RGB module for processing.
        proximity_RGB_send_control(&Xbox_controller_data);

         if (enable_telemetry) {
             printf("\rL-Joy X:%3u Y:%3u [%d] | R-Joy X:%3u Y:%3u [%d] | L2:%5u R2:%5u | L1:%d R1:%d|DPAD:[%d%d%d%d]|A:%d B:%d X:%d Y:%d|ST:%d SL:%d",
                 Xbox_controller_data.joy_lx, Xbox_controller_data.joy_ly, rcv_l3,
                 Xbox_controller_data.joy_rx, Xbox_controller_data.joy_ry, rcv_r3,
                 Xbox_controller_data.trigger_l, Xbox_controller_data.trigger_r,
                 rcv_l1, rcv_r1,
                 rcv_dpad_up, rcv_dpad_down, rcv_dpad_left, rcv_dpad_right,
                 rcv_a, rcv_b, rcv_x, rcv_y,
                 rcv_start, rcv_select);
             fflush(stdout);
         }

        // Actualizar estado del LED sin bloquear; se enciende brevemente cuando detectamos cambio
        if (xTaskGetTickCount() < led_on_until) {
            gpio_set_level(LED_GPIO, 1);
        } else {
            gpio_set_level(LED_GPIO, 0);
        }

        // Cada 1 segundo (100 ciclos de 10ms), le mandamos telemetría de vuelta al mando
        contador_telemetria++;
        if (contador_telemetria >= 100) {
            contador_telemetria = 0;
            
            // Simulamos que la batería se descarga un poco para ver el cambio
            mi_telemetria.battery_voltage -= 0.01; 
            if (mi_telemetria.battery_voltage < 10.0) mi_telemetria.battery_voltage = 12.6;
            
            // Enviamos el latido normal de telemetría (batería, etc.)
            esp_now_receiver_send_telemetry(&mi_telemetria);
        }

        // Bucle de control corriendo a 100Hz (Cada 10ms)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    xTaskCreate(uart_input_task, "uart_input", 3072, NULL, 4, NULL);

    xTaskCreatePinnedToCore(
        main_task,
        "main_task",
        8192,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        0
    );

    vTaskDelete(NULL);
}