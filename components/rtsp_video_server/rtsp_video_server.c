#include "rtsp_video_server.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"

static const char *TAG = "rtsp_video";

// ── Configuration ──────────────────────────────────────────────────────────

#define VIDEO_FRAME_RATE_HZ        1
#define MAX_MJPEG_FRAME_SIZE       16384
#define MJPEG_RTP_PAYLOAD_TYPE     26
#define VIDEO_RTP_CLOCK_RATE       90000
#define VIDEO_TASK_STACK           4096
#define VIDEO_TASK_PRIORITY        5

// ── Hardcoded 8×8 black JPEG frame ────────────────────────────────────────
// This is a minimal 8×8 all-black JPEG (179 bytes).
// Replace with your own JPEG data or generate dynamically.

static const uint8_t s_black_jpeg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43,
    0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08, 0x07, 0x07, 0x07, 0x09,
    0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
    0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20,
    0x24, 0x2E, 0x27, 0x20, 0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29,
    0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27, 0x39, 0x3D, 0x38, 0x32,
    0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08,
    0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03,
    0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
    0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
    0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
    0x00, 0x00, 0x3F, 0x00, 0xFB, 0xD0, 0xFF, 0xD9
};

// ── Module state ───────────────────────────────────────────────────────────

typedef struct {
    uint8_t  data[MAX_MJPEG_FRAME_SIZE];
    size_t   size;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t ts;
} frame_buffer_t;

static frame_buffer_t      s_frame_buf;
static SemaphoreHandle_t   s_frame_mutex;
static uint32_t            s_video_ssrc;

// ── RTP Packet Builder ─────────────────────────────────────────────────────

/**
 * Build MJPEG RTP packet header and payload.
 * Returns total packet size.
 */
static size_t build_mjpeg_rtp_packet(uint8_t *pkt, size_t max_len,
                                      uint16_t seq, uint32_t ts,
                                      const uint8_t *jpeg_data, size_t jpeg_size)
{
    if (jpeg_size > MAX_MJPEG_FRAME_SIZE || !jpeg_data || !pkt)
        return 0;

    // RTP header (12 bytes)
    pkt[0]  = 0x80;                           // V=2, P=0, X=0, CC=0
    pkt[1]  = 0x80 | MJPEG_RTP_PAYLOAD_TYPE; // M=1, PT=26
    pkt[2]  = (seq >> 8) & 0xFF;
    pkt[3]  = seq & 0xFF;
    pkt[4]  = (ts >> 24) & 0xFF;
    pkt[5]  = (ts >> 16) & 0xFF;
    pkt[6]  = (ts >> 8) & 0xFF;
    pkt[7]  = ts & 0xFF;
    pkt[8]  = (s_video_ssrc >> 24) & 0xFF;
    pkt[9]  = (s_video_ssrc >> 16) & 0xFF;
    pkt[10] = (s_video_ssrc >> 8) & 0xFF;
    pkt[11] = s_video_ssrc & 0xFF;

    // MJPEG RTP header extension (8 bytes, per RFC 2435)
    // Type-specific (bit 7), Fragment Offset (24 bits), Type (8 bits), Q (8 bits),
    // Width (8 bits), Height (8 bits)
    pkt[12] = 0x00;     // Type-specific (0 for frame start)
    pkt[13] = 0x00;     // Fragment offset (24-bit, high byte)
    pkt[14] = 0x00;     // Fragment offset (middle byte)
    pkt[15] = 0x00;     // Fragment offset (low byte)
    pkt[16] = 0x01;     // Type (1 = baseline JPEG)
    pkt[17] = 0x00;     // Q (0 = default quantization)
    pkt[18] = 0x08;     // Width in units of 8 pixels (8 → 64 pixels)
    pkt[19] = 0x08;     // Height in units of 8 pixels (8 → 64 pixels)

    // Copy JPEG payload
    size_t payload_offset = 20;
    if (payload_offset + jpeg_size > max_len)
        return 0;  // Packet too large

    memcpy(pkt + payload_offset, jpeg_data, jpeg_size);
    return payload_offset + jpeg_size;
}

// ── Frame buffer management ────────────────────────────────────────────────

static void frame_buffer_init(void)
{
    s_frame_buf.size = sizeof(s_black_jpeg);
    memcpy(s_frame_buf.data, s_black_jpeg, s_frame_buf.size);
    s_frame_buf.ssrc = esp_random();
    s_frame_buf.seq = 0;
    s_frame_buf.ts = 0;
    s_video_ssrc = s_frame_buf.ssrc;

    ESP_LOGI(TAG, "Frame buffer initialized (default black JPEG: %zu bytes, SSRC=0x%08" PRIx32 ")",
             s_frame_buf.size, s_video_ssrc);
}

static esp_err_t frame_buffer_update(const uint8_t *frame_data, size_t frame_size)
{
    if (!frame_data || frame_size == 0 || frame_size > MAX_MJPEG_FRAME_SIZE)
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    memcpy(s_frame_buf.data, frame_data, frame_size);
    s_frame_buf.size = frame_size;
    xSemaphoreGive(s_frame_mutex);

    return ESP_OK;
}

static void frame_buffer_get_snapshot(uint8_t *out_data, size_t *out_size,
                                       uint16_t *out_seq, uint32_t *out_ts)
{
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    memcpy(out_data, s_frame_buf.data, s_frame_buf.size);
    *out_size = s_frame_buf.size;
    *out_seq = s_frame_buf.seq++;
    *out_ts = s_frame_buf.ts;
    s_frame_buf.ts += VIDEO_RTP_CLOCK_RATE / VIDEO_FRAME_RATE_HZ;
    xSemaphoreGive(s_frame_mutex);
}

// ── Video frame task ───────────────────────────────────────────────────────

static void video_frame_task(void *pvParam)
{
    ESP_LOGI(TAG, "Video frame task started (%d fps, SSRC=0x%08" PRIx32 ")",
             VIDEO_FRAME_RATE_HZ, s_video_ssrc);

    uint8_t  pkt_buf[12 + 8 + MAX_MJPEG_FRAME_SIZE];  // RTP header + MJPEG header + payload
    uint8_t  frame_data[MAX_MJPEG_FRAME_SIZE];
    size_t   frame_size;
    uint16_t seq;
    uint32_t ts;

    int64_t last_frame_us = esp_timer_get_time();
    int64_t frame_interval_us = 1000000 / VIDEO_FRAME_RATE_HZ;

    while (true) {
        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - last_frame_us;

        if (elapsed_us < frame_interval_us) {
            vTaskDelay(pdMS_TO_TICKS((frame_interval_us - elapsed_us) / 1000));
            continue;
        }

        last_frame_us = now_us;

        // Get current frame snapshot
        frame_buffer_get_snapshot(frame_data, &frame_size, &seq, &ts);

        // Build RTP packet
        size_t pkt_size = build_mjpeg_rtp_packet(pkt_buf, sizeof(pkt_buf), seq, ts,
                                                  frame_data, frame_size);
        if (pkt_size == 0) {
            ESP_LOGW(TAG, "Failed to build RTP packet (frame size %zu)", frame_size);
            continue;
        }

        // TODO: Send packet to RTSP clients via rtsp_audio_server
        // This requires integration with the audio server's client list and sockets.
        // For now, log the frame info:
        ESP_LOGD(TAG, "Video frame: seq=%u ts=%" PRIu32 " size=%zu bytes",
                 seq, ts, pkt_size);
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

esp_err_t rtsp_video_server_init(void)
{
    s_frame_mutex = xSemaphoreCreateMutex();
    if (!s_frame_mutex)
        return ESP_ERR_NO_MEM;

    frame_buffer_init();
    return ESP_OK;
}

esp_err_t rtsp_video_start_streaming(void)
{
    BaseType_t rc = xTaskCreate(video_frame_task, "video_frames",
                                VIDEO_TASK_STACK, NULL,
                                VIDEO_TASK_PRIORITY, NULL);
    return rc == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t rtsp_video_server_send_frame(const uint8_t *frame_data, size_t frame_size)
{
    return frame_buffer_update(frame_data, frame_size);
}
