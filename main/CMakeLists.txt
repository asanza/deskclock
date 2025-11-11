idf_component_register(SRCS "esp32_deskclock.c" 
                            "clock.c"
                            "display.c"
                            "battery.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver nvs_flash bt esp_driver_i2c esp_adc einkdrv pcf8563)
