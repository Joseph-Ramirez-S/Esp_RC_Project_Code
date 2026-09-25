#include "directions_inters.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdbool.h>

static const char *TAG = "DIR_INTERS";
static QueueHandle_t directions_queue = NULL;

// Variables de estado simplificadas
static bool State_Left = false;
static bool State_Right = false;
static bool Inter_State = false;
static bool ActivatedByKnock = false;

// Rastrea el estado anterior del D-PAD para detectar solo el momento en que se "presiona" un botón
static uint16_t Last_DPAD_State = 0;

static void directions_inters_process(uint16_t buttons);

static void directions_inters_apply_state(void)
{
    gpio_set_level(DIR_LEFT_GPIO, State_Left ? 1 : 0);
    gpio_set_level(DIR_RIGHT_GPIO, State_Right ? 1 : 0);
}

static void directions_inters_task(void *arg)
{
    uint16_t buttons;
    ESP_LOGI(TAG, "Task started on core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(directions_queue, &buttons, portMAX_DELAY) == pdTRUE) {
            directions_inters_process(buttons);
        }
    }
}

void directions_inters_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DIR_LEFT_GPIO) | (1ULL << DIR_RIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(DIR_LEFT_GPIO, 0);
    gpio_set_level(DIR_RIGHT_GPIO, 0);

    if (directions_queue == NULL) {
        directions_queue = xQueueCreate(10, sizeof(uint16_t));
        if (directions_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create directions queue");
            return;
        }
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        directions_inters_task,
        "directions_inters",
        4096,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        1
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create directions task");
    } else {
        ESP_LOGI(TAG, "Direccionales e intermitentes inicializadas en GPIO32 y GPIO33");
    }
}

bool directions_inters_send_buttons(uint16_t buttons)
{
    if (directions_queue == NULL) {
        return false;
    }
    return xQueueSend(directions_queue, &buttons, 0) == pdTRUE;
}

static void directions_inters_process(uint16_t buttons)
{
    // 1. Extraer los bits de la trama recibida
    // Usamos el bit 'UP' (ya definido en tu .h) como tu activador de Intermitentes
    uint16_t dpad_left  = buttons & DPAD_LEFT_BIT;
    uint16_t dpad_right = buttons & DPAD_RIGHT_BIT;
    uint16_t dpad_inter = buttons & DPAD_UP_BIT; 
    
    // 2. Traducir al formato de datos numérico (0001, 0010, 1000)
    uint16_t current_dpad_state = 0;
    
    if (dpad_inter) {
        current_dpad_state = 8; // DATA_INTER (1000)
    } else if (dpad_right) {
        current_dpad_state = 2; // DATA_DIR_RIGHT (0010)
    } else if (dpad_left) {
        current_dpad_state = 1; // DATA_DIR_LEFT (0001)
    }

    // 3. Máquina de Estados: Reaccionar SOLO cuando la entrada cambia
    // KNOCK SENSOR PRIORITY: If knock-activated hazards are active, block all DPAD commands.
    if (current_dpad_state != Last_DPAD_State) {
        if (ActivatedByKnock && current_dpad_state != 0) {
            // Knock-activated hazards have absolute priority.
            // All DPAD commands (left, right, hazard toggle) are completely ignored.
            // The only way to exit this state is through the main.c X button handler.
            ESP_LOGI(TAG, "Knock-activated state: DPAD command blocked");
            // Do NOT update Last_DPAD_State; preserve the lockout until X clears the knock state.
            return;
        }

        // --- CASO: INTERMITENTES (1000) ---
        if (current_dpad_state == 8) {
            // Normal toggle behavior for non-knock activation
            if (Inter_State) {
                // Si ya estaban encendidas, las apagamos todas (Toggle Off)
                Inter_State = false; State_Left = false; State_Right = false;
            } else {
                // Si estaban apagadas, forzamos ambas a encender
                Inter_State = true; State_Left = true; State_Right = true;
            }
            ESP_LOGI(TAG, "Intermitentes: %d", Inter_State);
        } 
        
        // --- CASO: DERECHA (0010) ---
        else if (current_dpad_state == 2) {
            if (State_Right && !Inter_State) {
                // Si ya estaba en Derecha, se apaga
                State_Right = false;
            } else {
                // Enciende Derecha pura, cancelando Izquierda o Intermitentes
                State_Right = true; State_Left = false; Inter_State = false; 
            }
            ESP_LOGI(TAG, "Direccional Derecha: %d", State_Right);
        } 
        
        // --- CASO: IZQUIERDA (0001) ---
        else if (current_dpad_state == 1) {
            if (State_Left && !Inter_State) {
                // Si ya estaba en Izquierda, se apaga
                State_Left = false; 
            } else {
                // Enciende Izquierda pura, cancelando Derecha o Intermitentes
                State_Left = true; State_Right = false; Inter_State = false; 
            }
            ESP_LOGI(TAG, "Direccional Izquierda: %d", State_Left);
        }

        // Guardar el estado actual para la siguiente evaluación
        Last_DPAD_State = current_dpad_state;

        // 4. Aplicar los estados lógicos calculados a los pines físicos
        directions_inters_apply_state();
    }
}

    void directions_inters_mark_knock_activation(void)
    {
        // Record that activation came from knock. If lights are off, force them on.
        if (Inter_State) {
            ActivatedByKnock = true;
        } else {
            Inter_State = true;
            State_Left = true;
            State_Right = true;
            ActivatedByKnock = true;
            directions_inters_apply_state();
        }
    }

    bool directions_inters_try_clear_knock_activation(void)
    {
        if (ActivatedByKnock) {
            ActivatedByKnock = false;
            Inter_State = false;
            State_Left = false;
            State_Right = false;
            directions_inters_apply_state();
            return true;
        }
        return false;
    }

    bool directions_inters_is_activated_by_knock(void)
    {
        return ActivatedByKnock;
    }