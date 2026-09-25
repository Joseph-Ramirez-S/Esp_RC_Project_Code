idf_component_register(SRCS "esp_now_sender.c" "main.c"
                    INCLUDE_DIRS "." "../../common"
                    REQUIRES usb esp_wifi esp_netif esp_event nvs_flash driver)
