idf_component_register(
    SRCS "rtsp_audio_server.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_netif
        lwip
        log
        freertos
        esp_system
)
