#include "driver/i2s_std.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

#include "app_config.h"
#include "rtsp_audio_server.h"

static const char *TAG = "main";

#if (SAMPLE_RATE < 8000) || (SAMPLE_RATE > 48000)
#error "SAMPLE_RATE out of supported range (8000..48000). Did you mean 44100 instead of 441000?"
#endif

#if (FRAME_SAMPLES < 128) || (FRAME_SAMPLES > 2048)
#error "FRAME_SAMPLES out of supported range (128..2048)."
#endif

// ── Wi-Fi ─────────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected — reconnecting…");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void) {
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t wc = {};
    strlcpy((char *) wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *) wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s…", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ── I2S ───────────────────────────────────────────────────────────────────

static i2s_chan_handle_t s_rx_chan;

static void i2s_start_capture(void) {
    // Allocate an RX-only channel on I2S0.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    // Build slot config and override the slot mask.
    // INMP441 uses the Philips I2S format and places data on:
    // - LEFT slot when L/R pin is LOW (GND)
    // - RIGHT slot when L/R pin is HIGH (VDD)
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = I2S_SCK_PIN,
                .ws = I2S_WS_PIN,
                .dout = I2S_GPIO_UNUSED,
                .din = I2S_SD_PIN,
            },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_LOGI(TAG, "I2S capture started — %d Hz, 32-bit stereo (using %s slot)", SAMPLE_RATE,
#if MIC_USE_LEFT_SLOT
        "LEFT"
#else
        "RIGHT"
#endif
    );
}

// ── Audio streaming task ───────────────────────────────────────────────────

static void audio_task(void *pvParam) {
    // Static buffers keep these off the (small) task stack.
    static int32_t raw[FRAME_SAMPLES * 2];
    static int16_t pcm[FRAME_SAMPLES];
    static int32_t dc_estimate = 0;
    static int32_t prev_pcm = 0;

    int64_t next_level_log_us = esp_timer_get_time() + 1000000;

    while (true) {
        size_t    bytes_read = 0;
        esp_err_t err =
            i2s_channel_read(s_rx_chan, raw, FRAME_SAMPLES * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_read: %s", esp_err_to_name(err));
            continue;
        }

        // Downshift 32-bit raw slot data to 16-bit PCM, then remove DC/very-low
        // frequency bias that often appears as a persistent low buzz/hum.
        const size_t frames = bytes_read / (2 * sizeof(int32_t));
        int16_t      peak = 0;
        for (size_t i = 0; i < frames; i++) {
#if MIC_USE_LEFT_SLOT
            const int32_t slot_raw = raw[i * 2];
#else
            const int32_t slot_raw = raw[i * 2 + 1];
#endif

            int32_t s16 = (int32_t) (slot_raw >> (INPUT_BITS - OUTPUT_BITS));
            dc_estimate += (s16 - dc_estimate) >> DC_BLOCK_SHIFT;
            int32_t filtered = s16 - dc_estimate;

#if CLICK_LIMIT_DELTA > 0
            int32_t delta = filtered - prev_pcm;
            if (delta > CLICK_LIMIT_DELTA) {
                filtered = prev_pcm + CLICK_LIMIT_DELTA;
            } else if (delta < -CLICK_LIMIT_DELTA) {
                filtered = prev_pcm - CLICK_LIMIT_DELTA;
            }
#endif
            prev_pcm = filtered;

            if (filtered > 32767)
                filtered = 32767;
            if (filtered < -32768)
                filtered = -32768;
            pcm[i] = (int16_t) filtered;

            int16_t abs_val = pcm[i] >= 0 ? pcm[i] : (int16_t) -pcm[i];
            if (abs_val > peak) {
                peak = abs_val;
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_level_log_us) {
            ESP_LOGI(TAG, "Audio peak=%d (16-bit)", peak);
            next_level_log_us = now_us + 1000000;
        }

        // Send to all connected RTSP clients; no-op if none are playing.
        rtsp_audio_server_send_audio(pcm, frames);
    }
}

// ── Entry point ────────────────────────────────────────────────────────────

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 I2S→RTSP audio streamer (native ESP-IDF)");

    // NVS is required by the Wi-Fi driver.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_start();
    i2s_start_capture();

    rtsp_audio_server_config_t rtsp_cfg = {
        .port = RTSP_PORT,
        .sample_rate = SAMPLE_RATE,
    };
    ESP_ERROR_CHECK(rtsp_audio_server_start(&rtsp_cfg));

    // Audio task pinned to APP_CPU (core 1) so the Wi-Fi/RTSP stack
    // runs freely on PRO_CPU (core 0).
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 5, NULL, 1);
}
