#ifndef SD_SDMMC_H
#define SD_SDMMC_H

#include <stdint.h>

#include "esp_err.h"

/**
 * Mount the 4-bit SDMMC card, initialize PCM5102A I2S output, and start the
 * audio streaming task.
 */
esp_err_t SD_init(void);

/** Queue a sound file for playback using its terminal command number. */
esp_err_t SD_play_sound(uint8_t sound_number);

#endif
