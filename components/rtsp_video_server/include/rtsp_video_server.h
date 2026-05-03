#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the RTSP video server.
 *
 * Must be called before rtsp_video_start_streaming().
 * Creates internal mutex and initializes RTP state.
 */
esp_err_t rtsp_video_server_init(void);

/**
 * Start the video frame transmission task.
 *
 * Spawns a FreeRTOS task that sends MJPEG frames at 1 fps.
 * Must be called after rtsp_audio_server_start().
 */
esp_err_t rtsp_video_start_streaming(void);

/**
 * Send a new video frame to all connected RTSP clients.
 *
 * Thread-safe; can be called from any task.
 * Frame is cached internally until next call.
 *
 * @param frame_data  JPEG image data (valid JPEG bitstream)
 * @param frame_size  Size in bytes (must be ≤ 16 KB)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if frame too large
 */
esp_err_t rtsp_video_server_send_frame(const uint8_t *frame_data, size_t frame_size);

#ifdef __cplusplus
}
#endif
