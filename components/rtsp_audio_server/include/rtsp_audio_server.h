#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t port;         /**< RTSP TCP listen port (default 554)   */
    uint32_t sample_rate;  /**< PCM sample rate in Hz (default 16000) */
} rtsp_audio_server_config_t;

/**
 * Start the RTSP audio server.
 *
 * Must be called after the network interface is up.
 * Spawns an internal FreeRTOS task and returns immediately.
 * The stream is available at  rtsp://<device-ip>:<port>/stream
 */
esp_err_t rtsp_audio_server_start(const rtsp_audio_server_config_t *cfg);

/**
 * Push 16-bit mono PCM samples to all currently-playing RTSP clients.
 *
 * Thread-safe; call from any task (typically the I2S capture task).
 *
 * @param samples   PCM data in host byte order (little-endian on ESP32).
 * @param n_samples Number of int16_t elements in samples[].
 */
void rtsp_audio_server_send_audio(const int16_t *samples, size_t n_samples);

/**
 * Forward a complete MJPEG RTP packet to all currently-playing RTSP clients
 * that have the video track set up.
 *
 * Thread-safe; call from any task.
 *
 * @param pkt      Complete RTP packet including header.
 * @param pkt_len  Packet length in bytes.
 */
void rtsp_audio_server_send_video_packet(const uint8_t *pkt, size_t pkt_len);

#ifdef __cplusplus
}
#endif
