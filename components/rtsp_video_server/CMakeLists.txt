idf_component_register(
    SRCS "rtsp_video_server.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        esp_timer
        esp_system
        log
        rtsp_audio_server
)
