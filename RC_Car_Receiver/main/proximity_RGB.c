// proximity_RGB.c
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "led_strip.h"
#include "esp_now_receiver.h"
#include "directions_inters.h"
#include "proximity_RGB.h"
#include <math.h>

#define PROXIMITY_SENSOR_ADC_UNIT ADC_UNIT_1
#define PROXIMITY_SENSOR_ADC_CHANNEL_COUNT 2
#define PROXIMITY_SENSOR_ADC_CHANNELS { ADC1_CHANNEL_6, ADC1_CHANNEL_7 }
#define PROXIMITY_SENSOR_ADC_ATTEN ADC_ATTEN_DB_12
#define PROXIMITY_SENSOR_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define PROXIMITY_ADC_MAX_RAW 4095 
#define PROXIMITY_ADC_MAX_VOLTAGE 3.3f
#define ADC_SAMPLE_COUNT 20
#define PROXIMITY_FILTER_ALPHA 0.20f
#define PROXIMITY_UPDATE_MS 35
#define PROXIMITY_DEADBAND 0.04f

#define PROXIMITY_LED_STRIP_GPIO GPIO_NUM_5
#define NUM_LEDS 10
#define UMBRAL_MIN 0.65f
#define VOLT_MAX 3.1f

#define PROXIMITY_TASK_STACK 8192
#define PROXIMITY_TASK_PRIO (tskIDLE_PRIORITY + 2)

static const char *TAG = "PROXIMITY_RGB";
static QueueHandle_t proximity_queue = NULL;
static led_strip_handle_t strip = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static const adc_channel_t proximity_adc_channels[PROXIMITY_SENSOR_ADC_CHANNEL_COUNT] =
    PROXIMITY_SENSOR_ADC_CHANNELS;

__attribute__((weak)) void proximity_RGB_handle_control(const control_packet_t *ctl)
{
    (void)ctl;
}

static esp_err_t proximity_RGB_sensor_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = PROXIMITY_SENSOR_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = PROXIMITY_SENSOR_ADC_ATTEN,
        .bitwidth = PROXIMITY_SENSOR_ADC_BITWIDTH,
    };

    for (int i = 0; i < PROXIMITY_SENSOR_ADC_CHANNEL_COUNT; ++i) {
        ret = adc_oneshot_config_channel(adc_handle, proximity_adc_channels[i], &chan_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_config_channel failed for channel %d: %s",
                     proximity_adc_channels[i], esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t proximity_RGB_strip_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = PROXIMITY_LED_STRIP_GPIO,
        .max_leds = NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = 0 },
    };

    led_strip_rmt_config_t rmt_config = {0};

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }

    err = led_strip_clear(strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_clear failed: %s", esp_err_to_name(err));
        return err;
    }

    return led_strip_refresh(strip);
}

static float proximity_RGB_read_voltage(void)
{
    if (adc_handle == NULL) {
        return 0.0f;
    }

    float maximum_voltage = 0.0f;

    for (int channel_index = 0; channel_index < PROXIMITY_SENSOR_ADC_CHANNEL_COUNT; ++channel_index) {
        int raw = 0;
        long raw_sum = 0;
        int valid_samples = 0;

        for (int sample_index = 0; sample_index < ADC_SAMPLE_COUNT; ++sample_index) {
            esp_err_t err = adc_oneshot_read(adc_handle, proximity_adc_channels[channel_index], &raw);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "adc_oneshot_read failed for channel %d: %s",
                         proximity_adc_channels[channel_index], esp_err_to_name(err));
                continue;
            }
            raw_sum += raw;
            valid_samples++;
        }

        if (valid_samples > 0) {
            float average_raw = raw_sum / (float)valid_samples;
            float voltage = (average_raw / PROXIMITY_ADC_MAX_RAW) * PROXIMITY_ADC_MAX_VOLTAGE;
            if (voltage > maximum_voltage) {
                maximum_voltage = voltage;
            }
        }
    }

    return maximum_voltage;
}

static int proximity_RGB_clamp(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void proximity_RGB_update_strip(float voltage)
{
    if (strip == NULL) {
        return;
    }

    float range_total = VOLT_MAX - UMBRAL_MIN;
    if (range_total <= 0.0f) {
        return;
    }

    float step = range_total / NUM_LEDS;
    for (int i = 0; i < NUM_LEDS; ++i) {
        float threshold = UMBRAL_MIN + step * i;
        float factor = (voltage - threshold) / step;
        int brightness = proximity_RGB_clamp((int)(factor * 255.0f), 0, 255);

        uint32_t red = 0;
        uint32_t green = 0;
        uint32_t blue = 0;

        if (brightness > 0) {
            if (i % 3 == 0) {
                red = brightness;
            } else if (i % 3 == 1) {
                blue = brightness;
            } else {
                red = brightness / 2;
                blue = brightness;
            }
        }

        led_strip_set_pixel(strip, i, red, green, blue);
    }

    led_strip_refresh(strip);
}

static float proximity_RGB_apply_deadband(float voltage, float previous_voltage)
{
    if (fabsf(voltage - previous_voltage) < PROXIMITY_DEADBAND) {
        return previous_voltage;
    }
    return voltage;
}

static void proximity_RGB_task(void *arg)
{
    control_packet_t pkt;
    ESP_LOGI(TAG, "Task started on core %d", xPortGetCoreID());

    const TickType_t queue_wait = pdMS_TO_TICKS(5);
    TickType_t last_update = xTaskGetTickCount();
    float filtered_voltage = 0.0f;
    float display_voltage = 0.0f;
    bool has_voltage = false;

    while (1) {
        if (xQueueReceive(proximity_queue, &pkt, queue_wait) == pdTRUE) {
            proximity_RGB_handle_control(&pkt);
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_update >= pdMS_TO_TICKS(PROXIMITY_UPDATE_MS)) {
            float voltage = proximity_RGB_read_voltage();
            if (!has_voltage) {
                filtered_voltage = voltage;
                display_voltage = voltage;
                has_voltage = true;
            } else {
                filtered_voltage += (voltage - filtered_voltage) * PROXIMITY_FILTER_ALPHA;
                display_voltage = proximity_RGB_apply_deadband(filtered_voltage, display_voltage);
            }

            proximity_RGB_update_strip(display_voltage);
            last_update = now;
        }
    }
}

void proximity_RGB_init(void)
{
    if (proximity_queue != NULL) {
        return;
    }

    if (proximity_RGB_sensor_init() != ESP_OK) {
        ESP_LOGE(TAG, "Proximity sensor initialization failed");
        return;
    }

    if (proximity_RGB_strip_init() != ESP_OK) {
        ESP_LOGE(TAG, "Proximity strip initialization failed");
        return;
    }

    proximity_queue = xQueueCreate(10, sizeof(control_packet_t));
    if (proximity_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create proximity queue");
        return;
    }

    BaseType_t r = xTaskCreatePinnedToCore(
        proximity_RGB_task,
        "proximity_RGB",
        PROXIMITY_TASK_STACK,
        NULL,
        PROXIMITY_TASK_PRIO,
        NULL,
        1
    );
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create proximity_RGB task");
    }
}

bool proximity_RGB_send_control(const control_packet_t *ctl)
{
    if (proximity_queue == NULL || ctl == NULL) {
        return false;
    }
    return xQueueSend(proximity_queue, ctl, 0) == pdTRUE;
}
