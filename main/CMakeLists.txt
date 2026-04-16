idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        rtsp_audio_server
        esp_wifi
        esp_netif
        esp_event
    esp_timer
        nvs_flash
    esp_driver_i2s
    esp_driver_gpio
        log
        freertos
)
