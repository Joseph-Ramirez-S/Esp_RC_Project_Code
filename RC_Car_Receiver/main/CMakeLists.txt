idf_component_register(SRCS "SD_sdmmc.c" "main.c" "esp_now_receiver.c" "directions_inters.c" "proximity_RGB.c" "servo_sweep.c"
                    INCLUDE_DIRS "." "../../common"
                    REQUIRES driver fatfs sdmmc esp_driver_sdmmc esp_driver_i2s
                             esp_driver_gpio esp_driver_ledc esp_wifi nvs_flash esp_adc)
