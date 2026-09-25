#include "servo_sweep.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERVO_RX2_GPIO GPIO_NUM_16
#define SERVO_TX2_GPIO GPIO_NUM_17
#define SERVO_FREQUENCY_HZ 50
#define SERVO_DUTY_RESOLUTION LEDC_TIMER_14_BIT
#define SERVO_MIN_PULSE_US 500
#define SERVO_MAX_PULSE_US 2500
#define SERVO_PERIOD_US (1000000 / SERVO_FREQUENCY_HZ)
#define SERVO_STEP_DELAY_MS 10

static const char *TAG = "SERVO_SWEEP";

static uint32_t servo_angle_to_duty(uint32_t angle)
{
    uint32_t pulse_us = SERVO_MIN_PULSE_US +
                        ((SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * angle) / 180;
    uint32_t duty_max = (1U << SERVO_DUTY_RESOLUTION) - 1U;
    return (pulse_us * duty_max) / SERVO_PERIOD_US;
}

static void servo_set_angle(ledc_channel_t channel, uint32_t angle)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, servo_angle_to_duty(angle));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static void servo_sweep_task(void *arg)
{
    while (1) {
        for (uint32_t angle = 0; angle <= 180; angle++) {
            servo_set_angle(LEDC_CHANNEL_0, angle);
            servo_set_angle(LEDC_CHANNEL_1, angle);
            vTaskDelay(pdMS_TO_TICKS(SERVO_STEP_DELAY_MS));
        }

        for (int angle = 179; angle >= 0; angle--) {
            servo_set_angle(LEDC_CHANNEL_0, (uint32_t)angle);
            servo_set_angle(LEDC_CHANNEL_1, (uint32_t)angle);
            vTaskDelay(pdMS_TO_TICKS(SERVO_STEP_DELAY_MS));
        }
    }
}

void servo_sweep_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_DUTY_RESOLUTION,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    const ledc_channel_config_t rx2_channel_config = {
        .gpio_num = SERVO_RX2_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = servo_angle_to_duty(0),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&rx2_channel_config));

    const ledc_channel_config_t tx2_channel_config = {
        .gpio_num = SERVO_TX2_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = servo_angle_to_duty(0),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&tx2_channel_config));

    BaseType_t task_result = xTaskCreatePinnedToCore(
        servo_sweep_task,
        "servo_sweep",
        3072,
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        1
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create servo sweep task");
        return;
    }

    ESP_LOGI(TAG, "Servo sweep initialized on RX2/GPIO%d and TX2/GPIO%d", SERVO_RX2_GPIO, SERVO_TX2_GPIO);
}