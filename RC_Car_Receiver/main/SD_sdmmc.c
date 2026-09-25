#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "SD_sdmmc.h"

#define SD_MOUNT_POINT "/sdcard"

// Classic ESP32 SDMMC slot 0 pins. External 10 kOhm pull-ups are required.
#define SD_CLK_GPIO GPIO_NUM_14
#define SD_CMD_GPIO GPIO_NUM_15
#define SD_D0_GPIO GPIO_NUM_2
#define SD_D1_GPIO GPIO_NUM_4
#define SD_D2_GPIO GPIO_NUM_12
#define SD_D3_GPIO GPIO_NUM_13

// PCM5102A: BCK, LRCK, DIN. SCK/MCLK is not required by the PCM5102A.
#define AUDIO_BCLK_GPIO GPIO_NUM_23
#define AUDIO_LRCLK_GPIO GPIO_NUM_21
#define AUDIO_DATA_GPIO GPIO_NUM_22

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_DMA_DESC_NUM 8
#define AUDIO_DMA_FRAME_NUM 256
#define AUDIO_COMMAND_QUEUE_LENGTH 1
#define AUDIO_READ_BUFFER_SIZE 2048
#define AUDIO_OUTPUT_SAMPLE_CAPACITY AUDIO_READ_BUFFER_SIZE
#define AUDIO_WRITE_TIMEOUT_MS 500
#define AUDIO_WRITE_RETRIES 3
#define AUDIO_I2S_FRAME_BYTES (2 * sizeof(int16_t))
#define AUDIO_DMA_WRITE_BYTES (AUDIO_DMA_FRAME_NUM * AUDIO_I2S_FRAME_BYTES)
#define AUDIO_RECOVERY_WINDOW_MS 7000
#define SDMMC_CLOCK_KHZ 40000
#define SDMMC_COMMAND_TIMEOUT_MS 2000
#define SD_READ_RETRIES 4
#define SD_READ_RETRY_DELAY_MS 20
#define SOUND_FILE_COUNT 7
#define SD_READ_ERROR_THRESHOLD 3
#define AUDIO_VOLUME 0.8f

static const char *TAG = "SD_AUDIO";
static QueueHandle_t sound_queue;
static i2s_chan_handle_t i2s_tx_handle;
static bool i2s_enabled;
static sdmmc_card_t *sd_card;
static uint8_t consecutive_read_errors;
static StaticQueue_t sound_queue_control;
static DRAM_ATTR uint8_t sound_queue_storage[AUDIO_COMMAND_QUEUE_LENGTH * sizeof(uint8_t)];
static uint8_t *audio_input;
static int16_t *audio_output_16;
static volatile bool audio_abort_requested;
static const char *const sound_file_names[SOUND_FILE_COUNT + 1] = {
    [1] = "sndaL.wav",
    [2] = "sndaR.wav",
    [3] = "sndb.wav",
    [4] = "sndc.wav",
    [5] = "music_A.wav",
    [6] = "music_B.wav",
    [7] = "music_C.wav",
};

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} wav_info_t;

static bool read_u16(FILE *file, uint16_t *value)
{
    uint8_t bytes[2];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
        return false;
    }
    *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return true;
}

static bool read_u32(FILE *file, uint32_t *value)
{
    uint8_t bytes[4];
    if (fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
        return false;
    }
    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) |
             ((uint32_t)bytes[3] << 24);
    return true;
}

static esp_err_t mount_sd_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
        .disk_status_check_enable = true,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_CLOCK_KHZ;
    host.command_timeout_ms = SDMMC_COMMAND_TIMEOUT_MS;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.flags &= ~SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot_config.clk = SD_CLK_GPIO;
    slot_config.cmd = SD_CMD_GPIO;
    slot_config.d0 = SD_D0_GPIO;
    slot_config.d1 = SD_D1_GPIO;
    slot_config.d2 = SD_D2_GPIO;
    slot_config.d3 = SD_D3_GPIO;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    if (err != ESP_OK) {
        sd_card = NULL;
        ESP_LOGE(TAG, "4-bit SDMMC mount failed: %s", esp_err_to_name(err));
        return err;
    }
    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

static esp_err_t recover_sd_card(void)
{
    ESP_LOGW(TAG, "Recovering SD card after %u consecutive read errors",
             consecutive_read_errors);

    if (sd_card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
        sd_card = NULL;
    }
    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(AUDIO_RECOVERY_WINDOW_MS);
    esp_err_t err = ESP_FAIL;
    do {
        vTaskDelay(pdMS_TO_TICKS(250));
        err = mount_sd_card();
        if (err == ESP_OK) {
            consecutive_read_errors = 0;
            ESP_LOGI(TAG, "SD card recovered");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD card retry failed: %s", esp_err_to_name(err));
    } while (xTaskGetTickCount() < deadline);

    ESP_LOGE(TAG, "SD card recovery failed after %d seconds",
             AUDIO_RECOVERY_WINDOW_MS / 1000);
    return err;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_DMA_DESC_NUM;
    channel_config.dma_frame_num = AUDIO_DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&channel_config, &i2s_tx_handle, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_clk_config_t clk_config =
        I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    i2s_std_slot_config_t slot_config =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    slot_config.slot_mask = I2S_STD_SLOT_BOTH;
    slot_config.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;

    i2s_std_config_t standard_config = {
        .clk_cfg = clk_config,
        .slot_cfg = slot_config,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK_GPIO,
            .ws = AUDIO_LRCLK_GPIO,
            .dout = AUDIO_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(i2s_tx_handle, &standard_config);
    if (err != ESP_OK) {
        i2s_del_channel(i2s_tx_handle);
        i2s_tx_handle = NULL;
        return err;
    }
    esp_err_t enable_err = i2s_channel_enable(i2s_tx_handle);
    if (enable_err == ESP_OK) {
        i2s_enabled = true;
    }
    return enable_err;
}

static esp_err_t reconfigure_i2s(uint32_t sample_rate)
{
    i2s_std_clk_config_t clk_config =
        I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

    esp_err_t err = ESP_OK;
    if (i2s_enabled) {
        err = i2s_channel_disable(i2s_tx_handle);
        if (err != ESP_OK) {
            return err;
        }
        i2s_enabled = false;
    }
    err = i2s_channel_reconfig_std_clock(i2s_tx_handle, &clk_config);
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(i2s_tx_handle);
    if (err == ESP_OK) {
        i2s_enabled = true;
    }
    return err;
}

static int16_t scale_sample(int16_t sample)
{
    float scaled = (float)sample * AUDIO_VOLUME;
    if (scaled > (float)INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled < (float)INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

static bool skip_wav_chunk(FILE *file, uint32_t chunk_size)
{
    if (chunk_size > (uint32_t)LONG_MAX ||
        fseek(file, (long)chunk_size, SEEK_CUR) != 0) {
        return false;
    }
    if ((chunk_size & 1U) != 0 && fseek(file, 1, SEEK_CUR) != 0) {
        return false;
    }
    return true;
}

static bool read_wav_header(FILE *file, wav_info_t *info)
{
    char riff[4];
    char wave[4];
    uint32_t riff_size;
    bool fmt_found = false;
    bool data_found = false;

    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        !read_u32(file, &riff_size) ||
        fread(wave, 1, sizeof(wave), file) != sizeof(wave) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
        return false;
    }
    (void)riff_size;

    memset(info, 0, sizeof(*info));

    while (!fmt_found || !data_found) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, sizeof(chunk_id), file) != sizeof(chunk_id) ||
            !read_u32(file, &chunk_size)) {
            return false;
        }

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16 || !read_u16(file, &info->audio_format) ||
                !read_u16(file, &info->channels) ||
                !read_u32(file, &info->sample_rate) ||
                !read_u32(file, &info->byte_rate) ||
                !read_u16(file, &info->block_align) ||
                !read_u16(file, &info->bits_per_sample)) {
                return false;
            }
            if (chunk_size > 16 &&
                !skip_wav_chunk(file, chunk_size - 16)) {
                return false;
            }
            fmt_found = true;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            long data_offset = ftell(file);
            if (data_offset < 0 || (unsigned long)data_offset > UINT32_MAX) {
                return false;
            }
            info->data_offset = (uint32_t)data_offset;
            info->data_size = chunk_size;
            data_found = true;
            if (!skip_wav_chunk(file, chunk_size)) {
                return false;
            }
        } else if (!skip_wav_chunk(file, chunk_size)) {
            return false;
        }
    }

    return info->audio_format == 1 && info->bits_per_sample == 16 &&
           (info->channels == 1 || info->channels == 2) &&
            info->sample_rate > 0 &&
            info->block_align == info->channels * sizeof(int16_t) &&
            info->byte_rate == info->sample_rate * info->block_align &&
            data_found;
}

static esp_err_t write_audio_buffer(const int16_t *buffer, size_t byte_count)
{
    const uint8_t *current = (const uint8_t *)buffer;
    size_t remaining = byte_count;

    while (remaining > 0 && !audio_abort_requested) {
        size_t chunk_bytes = remaining < AUDIO_DMA_WRITE_BYTES ?
                             remaining : AUDIO_DMA_WRITE_BYTES;
        chunk_bytes -= chunk_bytes % AUDIO_I2S_FRAME_BYTES;
        if (chunk_bytes == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t err = ESP_FAIL;
        size_t bytes_written = 0;
        for (int attempt = 0; attempt < AUDIO_WRITE_RETRIES; attempt++) {
            bytes_written = 0;
            err = i2s_channel_write(
                i2s_tx_handle, current, chunk_bytes, &bytes_written,
                pdMS_TO_TICKS(AUDIO_WRITE_TIMEOUT_MS));
            if (err == ESP_OK && bytes_written > 0 &&
                bytes_written <= chunk_bytes &&
                (bytes_written % AUDIO_I2S_FRAME_BYTES) == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (err != ESP_OK || bytes_written == 0 ||
            bytes_written > chunk_bytes ||
            (bytes_written % AUDIO_I2S_FRAME_BYTES) != 0) {
            return err == ESP_OK ? ESP_FAIL : err;
        }
        current += bytes_written;
        remaining -= bytes_written;
    }
    return remaining == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void play_sound(uint8_t sound_number)
{
    if (sound_number == 0 || sound_number > SOUND_FILE_COUNT) {
        ESP_LOGW(TAG, "Unknown sound number: %u", sound_number);
        return;
    }

    char path[sizeof(SD_MOUNT_POINT) + 1 + 16];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s",
             sound_file_names[sound_number]);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return;
    }

    wav_info_t info;
    if (!read_wav_header(file, &info)) {
        ESP_LOGE(TAG, "%s is not supported PCM 16-bit WAV", path);
        fclose(file);
        return;
    }
    if (i2s_tx_handle == NULL || info.sample_rate == 0) {
        ESP_LOGW(TAG, "%s has an invalid sample rate", path);
        fclose(file);
        return;
    }
    esp_err_t clock_err = reconfigure_i2s(info.sample_rate);
    if (clock_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S clock for %s to %lu Hz: %s", path,
                 (unsigned long)info.sample_rate, esp_err_to_name(clock_err));
        fclose(file);
        return;
    }
    if (fseek(file, info.data_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Cannot seek to PCM data in %s", path);
        fclose(file);
        return;
    }

    uint32_t remaining = info.data_size;
    uint32_t data_position = info.data_offset;
    bool sd_read_error = false;
    bool dma_error = false;
    size_t frame_bytes = sizeof(int16_t) * info.channels;
    if (frame_bytes == 0 || remaining % frame_bytes != 0) {
        ESP_LOGE(TAG, "%s has incomplete PCM frames", path);
        fclose(file);
        return;
    }
    while (remaining > 0 && !audio_abort_requested) {
        size_t bytes_to_read = remaining < AUDIO_READ_BUFFER_SIZE ?
                       remaining : AUDIO_READ_BUFFER_SIZE;
        bytes_to_read -= bytes_to_read % frame_bytes;
        if (bytes_to_read == 0) {
            sd_read_error = true;
            break;
        }
        size_t bytes_read = 0;
        bool block_read_ok = false;
        for (int attempt = 0; attempt < SD_READ_RETRIES; attempt++) {
            clearerr(file);
            if (attempt > 0 && fseek(file, (long)data_position, SEEK_SET) != 0) {
                break;
            }
            bytes_read = fread(audio_input, 1, bytes_to_read, file);
            if (bytes_read == bytes_to_read &&
                (bytes_read % frame_bytes) == 0 && !ferror(file)) {
                block_read_ok = true;
                break;
            }
            ESP_LOGW(TAG, "SD read retry %d/%d in %s: requested=%u received=%u",
                     attempt + 1, SD_READ_RETRIES, path, (unsigned)bytes_to_read,
                     (unsigned)bytes_read);
            vTaskDelay(pdMS_TO_TICKS(SD_READ_RETRY_DELAY_MS));
        }
        if (!block_read_ok) {
            sd_read_error = true;
            ESP_LOGW(TAG, "SD read failed in %s after retries", path);
            break;
        }
        consecutive_read_errors = 0;
        remaining -= bytes_read;
        data_position += bytes_read;
        size_t output_samples = 0;
        for (size_t index = 0; index + 1 < bytes_read; index += frame_bytes) {
            int16_t left = (int16_t)((uint16_t)audio_input[index] |
                                     ((uint16_t)audio_input[index + 1] << 8));
            left = scale_sample(left);
            audio_output_16[output_samples++] = left;
            if (info.channels == 2) {
                int16_t right = (int16_t)(
                    (uint16_t)audio_input[index + 2] |
                    ((uint16_t)audio_input[index + 3] << 8));
                audio_output_16[output_samples++] = scale_sample(right);
            } else {
                audio_output_16[output_samples++] = left;
            }
        }
        if (write_audio_buffer(audio_output_16,
                               output_samples * sizeof(int16_t)) != ESP_OK) {
            dma_error = true;
            ESP_LOGW(TAG, "I2S DMA write failed for %s", path);
            break;
        }
        vTaskDelay(1);
    }

    memset(audio_input, 0, AUDIO_READ_BUFFER_SIZE);
    memset(audio_output_16, 0,
           AUDIO_OUTPUT_SAMPLE_CAPACITY * sizeof(int16_t));
    for (int silence_write = 0; silence_write < 2 && !audio_abort_requested;
         silence_write++) {
        if (write_audio_buffer(audio_output_16,
                               AUDIO_READ_BUFFER_SIZE) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to flush I2S DMA with silence");
            break;
        }
    }
    if (i2s_enabled) {
        esp_err_t disable_err = i2s_channel_disable(i2s_tx_handle);
        if (disable_err == ESP_OK) {
            i2s_enabled = false;
        } else {
            ESP_LOGW(TAG, "Failed to stop I2S after %s: %s", path,
                     esp_err_to_name(disable_err));
        }
    }
    fclose(file);

    if ((sd_read_error || dma_error) && !audio_abort_requested) {
        if (consecutive_read_errors < UINT8_MAX) {
            consecutive_read_errors++;
        }
        ESP_LOGW(TAG, "%s failed (%u consecutive errors)",
                 dma_error ? "I2S DMA" : "WAV read",
                 consecutive_read_errors);
        if (consecutive_read_errors >= SD_READ_ERROR_THRESHOLD) {
            recover_sd_card();
        }
    }
    ESP_LOGI(TAG, "Played sound %u (%s)", sound_number,
             sound_file_names[sound_number]);
}

static void audio_task(void *arg)
{
    uint8_t sound_number;
    while (xQueueReceive(sound_queue, &sound_number, portMAX_DELAY) == pdTRUE) {
        audio_abort_requested = false;
        play_sound(sound_number);
    }
    vTaskDelete(NULL);
}

esp_err_t SD_play_sound(uint8_t sound_number)
{
    if (sound_number == 0 || sound_number > SOUND_FILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sound_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_abort_requested = true;
    xQueueReset(sound_queue);
    if (xQueueSend(sound_queue, &sound_number, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Sound queue unavailable; command %u ignored", sound_number);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t SD_init(void)
{
    esp_err_t err = mount_sd_card();
    if (err != ESP_OK) {
        err = recover_sd_card();
        if (err != ESP_OK) {
            return err;
        }
    }
    err = init_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed: %s", esp_err_to_name(err));
        if (sd_card != NULL) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
            sd_card = NULL;
        }
        return err;
    }
    audio_input = heap_caps_malloc(AUDIO_READ_BUFFER_SIZE,
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    audio_output_16 = heap_caps_malloc(
        AUDIO_OUTPUT_SAMPLE_CAPACITY * sizeof(int16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (audio_input == NULL || audio_output_16 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate DMA-capable internal audio buffers");
        heap_caps_free(audio_input);
        heap_caps_free(audio_output_16);
        audio_input = NULL;
        audio_output_16 = NULL;
        return ESP_ERR_NO_MEM;
    }
    sound_queue = xQueueCreateStatic(AUDIO_COMMAND_QUEUE_LENGTH,
                                     sizeof(uint8_t), sound_queue_storage,
                                     &sound_queue_control);
    if (sound_queue == NULL ||
        xTaskCreate(audio_task, "audio_stream", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start audio tasks");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Audio ready: use the UART command task to select sounds");
    return ESP_OK;
}
