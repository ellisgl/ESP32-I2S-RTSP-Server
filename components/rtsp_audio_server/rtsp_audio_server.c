/*
 * rtsp_audio_server.c
 *
 * Minimal RTSP/RTP audio-only server for ESP-IDF 5.x / 6.x.
 *
 * Protocol:  RTSP 1.0 (RFC 2326) over TCP, RTP over UDP
 * Payload:   RTP payload type 96 — L16 PCM mono, big-endian (dynamic PT)
 * Clients:   up to MAX_CLIENTS simultaneous unicast sessions
 *
 * Thread model
 * ─────────────
 *  rtsp_server_task   — accepts RTSP connections, handles OPTIONS / DESCRIBE /
 *                       SETUP / PLAY / TEARDOWN, uses select() so it never
 *                       blocks indefinitely.
 *  caller task        — calls rtsp_audio_server_send_audio() from the I2S
 *                       capture task.  Protected by s_mutex.
 *
 * Mutex discipline
 * ─────────────────
 *  s_mutex is taken only for brief state reads / writes.
 *  send() on RTSP TCP sockets and sendto() on the RTP UDP socket are both
 *  called *outside* the mutex to avoid priority inversion with the audio task.
 */

#include "rtsp_audio_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"

// ── Constants ──────────────────────────────────────────────────────────────

#define MAX_CLIENTS              4
#define RTSP_BUF_SIZE            2048
#define RESP_BUF_SIZE            1024
#define SERVER_RTP_PORT          5004   /* UDP port we send RTP from   */
#define SERVER_RTCP_PORT         5005   /* advertised only, not used   */
#define RTSP_TASK_STACK          (8 * 1024)
#define RTSP_TASK_PRIORITY       6

#define MAX_RTP_TRACKS           2
#define TRACK_AUDIO              0
#define TRACK_VIDEO              1
#define AUDIO_TCP_CHANNEL        0
#define VIDEO_TCP_CHANNEL        2

/*
 * Max audio samples per RTP packet.
 * 720 samples × 2 bytes = 1440 bytes payload, well under 1500-byte MTU
 * after the 12-byte RTP header and IP/UDP framing.
 */
#define MAX_RTP_PAYLOAD_SAMPLES  720

static const char *TAG = "rtsp_server";

#define RTP_PT_L16_DYNAMIC      96
#define RTP_PT_MJPEG            26

// ── Per-client session ─────────────────────────────────────────────────────

typedef struct {
    int      sock;          /* RTSP TCP socket; -1 = free slot */
    uint32_t session_id;
    char     peer_ip[16];   /* dotted-decimal, from accept()   */
    uint16_t rtp_port[MAX_RTP_TRACKS];
    bool     is_playing;
    bool     use_tcp[MAX_RTP_TRACKS];       /* RTP over RTSP/TCP interleaved   */
    uint8_t  tcp_channel[MAX_RTP_TRACKS];
    uint16_t rtp_seq[MAX_RTP_TRACKS];
    uint32_t rtp_ts[MAX_RTP_TRACKS];
} rtsp_client_t;

// ── Module state ───────────────────────────────────────────────────────────

static rtsp_client_t     s_clients[MAX_CLIENTS];
static int               s_listen_sock = -1;
static int               s_rtp_sock    = -1;
static uint32_t          s_sample_rate;
static uint16_t          s_rtsp_port;
static uint32_t          s_ssrc;
static char              s_local_ip[16]; /* resolved once at task start */
static SemaphoreHandle_t s_mutex;

typedef struct {
    int      idx;
    int      sock;
    bool     use_tcp;
    uint8_t  tcp_channel;
    uint16_t rtp_port;
    char     peer_ip[16];
    uint16_t rtp_seq;
    uint32_t rtp_ts;
} rtp_send_target_t;

// ── Internal helpers ───────────────────────────────────────────────────────

static int free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (s_clients[i].sock < 0) return i;
    return -1;
}

static int cseq_from(const char *buf)
{
    const char *p = strstr(buf, "CSeq:");
    if (!p) p = strstr(buf, "CSEQ:");
    return p ? atoi(p + 5) : 0;
}

static bool transport_requests_tcp(const char *buf)
{
    return strstr(buf, "RTP/AVP/TCP") != NULL;
}

static int track_id_from(const char *buf)
{
    const char *p = strstr(buf, "trackID=");
    return p ? atoi(p + 8) : TRACK_AUDIO;
}

static int interleaved_channel_from(const char *buf)
{
    const char *p = strstr(buf, "interleaved=");
    return p ? atoi(p + 12) : -1;
}

static uint16_t client_rtp_port_from(const char *buf)
{
    const char *p = strstr(buf, "client_port=");
    return p ? (uint16_t)atoi(p + 12) : 0;
}

/* TCP is a byte stream; ensure the full interleaved frame is written. */
static int send_all(int sock, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static void build_sdp(char *out, size_t maxlen)
{
    snprintf(out, maxlen,
        "v=0\r\n"
        "o=- 1 1 IN IP4 %s\r\n"
        "s=ESP32 AV\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d L16/%" PRIu32 "/1\r\n"
        "a=control:trackID=0\r\n"
        "m=video 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d JPEG/90000\r\n"
        "a=cliprect:0,0,8,8\r\n"
        "a=framesize:%d 8-8\r\n"
        "a=framerate:1\r\n"
        "a=control:trackID=1\r\n",
        s_local_ip, s_local_ip,
        RTP_PT_L16_DYNAMIC, RTP_PT_L16_DYNAMIC, s_sample_rate,
        RTP_PT_MJPEG, RTP_PT_MJPEG, RTP_PT_MJPEG);
}

/*
 * build_response() — called under s_mutex.
 *
 * Builds the RTSP response string into resp[0..maxlen] and updates client
 * state.  Returns the response length (0 on teardown after response is built
 * there too).  Sets *do_teardown when the socket should be closed after
 * sending the response.
 */
static size_t build_response(rtsp_client_t *c, const char *req,
                              char *resp, size_t maxlen,
                              bool *do_teardown)
{
    *do_teardown = false;
    int cseq = cseq_from(req);

    if (strncmp(req, "OPTIONS", 7) == 0) {
        return (size_t)snprintf(resp, maxlen,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
            "\r\n",
            cseq);

    } else if (strncmp(req, "DESCRIBE", 8) == 0) {
        char sdp[512];
        build_sdp(sdp, sizeof(sdp));
        return (size_t)snprintf(resp, maxlen,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Content-Base: rtsp://%s:%d/stream/\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%s",
            cseq, s_local_ip, s_rtsp_port, strlen(sdp), sdp);

    } else if (strncmp(req, "SETUP", 5) == 0) {
        int track = track_id_from(req);
        if (track < TRACK_AUDIO || track >= MAX_RTP_TRACKS)
            track = TRACK_AUDIO;

        if (c->session_id == 0)
            c->session_id = esp_random();
        c->rtp_port[track]   = client_rtp_port_from(req);
        c->is_playing        = false;
        c->use_tcp[track]    = transport_requests_tcp(req);

        if (c->use_tcp[track]) {
            int channel = interleaved_channel_from(req);
            if (channel < 0) {
                channel = (track == TRACK_VIDEO) ? VIDEO_TCP_CHANNEL : AUDIO_TCP_CHANNEL;
            }
            c->tcp_channel[track] = (uint8_t)channel;

            return (size_t)snprintf(resp, maxlen,
                "RTSP/1.0 200 OK\r\n"
                "CSeq: %d\r\n"
                "Session: %" PRIu32 "\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                "\r\n",
                cseq, c->session_id,
                c->tcp_channel[track], c->tcp_channel[track] + 1);
        }

        if (c->rtp_port[track] == 0) {
            return (size_t)snprintf(resp, maxlen,
                "RTSP/1.0 461 Unsupported Transport\r\n"
                "CSeq: %d\r\n"
                "\r\n",
                cseq);
        }

        return (size_t)snprintf(resp, maxlen,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %" PRIu32 "\r\n"
            "Transport: RTP/AVP/UDP;unicast;"
            "client_port=%d-%d;server_port=%d-%d\r\n"
            "\r\n",
            cseq, c->session_id,
            c->rtp_port[track], c->rtp_port[track] + 1,
            SERVER_RTP_PORT, SERVER_RTCP_PORT);

    } else if (strncmp(req, "PLAY", 4) == 0) {
        if (!c->is_playing) {
            for (int track = 0; track < MAX_RTP_TRACKS; ++track) {
                c->rtp_seq[track] = 0;
                c->rtp_ts[track] = 0;
            }
        }
        c->is_playing = true;
        ESP_LOGI(TAG, "PLAY: client %s", c->peer_ip);
        return (size_t)snprintf(resp, maxlen,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %" PRIu32 "\r\n"
            "RTP-Info: url=rtsp://%s:%d/stream;seq=0;rtptime=0\r\n"
            "\r\n",
            cseq, c->session_id,
            s_local_ip, s_rtsp_port);

    } else if (strncmp(req, "TEARDOWN", 8) == 0) {
        *do_teardown  = true;
        c->is_playing = false;
        ESP_LOGI(TAG, "TEARDOWN: client %s", c->peer_ip);
        return (size_t)snprintf(resp, maxlen,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %" PRIu32 "\r\n"
            "\r\n",
            cseq, c->session_id);

    } else {
        return (size_t)snprintf(resp, maxlen,
            "RTSP/1.0 501 Not Implemented\r\n"
            "CSeq: %d\r\n"
            "\r\n",
            cseq);
    }
}

// ── RTSP server task ───────────────────────────────────────────────────────

static void rtsp_server_task(void *pvParam)
{
    /* Resolve local IP — netif is up because wifi_start() waited for it. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            esp_ip4addr_ntoa(&ip_info.ip, s_local_ip, sizeof(s_local_ip));
    }

    /* UDP socket for outbound RTP packets. */
    s_rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_rtp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* TCP socket for RTSP control plane. */
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "TCP socket failed: %d", errno);
        close(s_rtp_sock);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in saddr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(s_rtsp_port),
    };
    if (bind(s_listen_sock, (struct sockaddr *)&saddr, sizeof(saddr)) != 0 ||
        listen(s_listen_sock, MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "bind/listen failed: %d", errno);
        close(s_rtp_sock);
        close(s_listen_sock);
        vTaskDelete(NULL);
        return;
    }
    fcntl(s_listen_sock, F_SETFL, O_NONBLOCK);

    ESP_LOGI(TAG, "RTSP server ready → rtsp://%s:%d/stream",
             s_local_ip, s_rtsp_port);

    char     req[RTSP_BUF_SIZE];
    char     resp[RESP_BUF_SIZE];

    while (true) {
        /* ── Build fd_set ──────────────────────────────────────────────── */
        fd_set rfds;
        int    maxfd = s_listen_sock;

        FD_ZERO(&rfds);
        FD_SET(s_listen_sock, &rfds);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (s_clients[i].sock >= 0) {
                FD_SET(s_clients[i].sock, &rfds);
                if (s_clients[i].sock > maxfd)
                    maxfd = s_clients[i].sock;
            }
        }
        xSemaphoreGive(s_mutex);

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; /* 100 ms */
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;

        /* ── Accept new connections ─────────────────────────────────────── */
        if (FD_ISSET(s_listen_sock, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t caddrlen = sizeof(caddr);
            int nsock = accept(s_listen_sock,
                               (struct sockaddr *)&caddr, &caddrlen);
            if (nsock >= 0) {
                /* Give RTSP response sends a 2-second deadline. */
                struct timeval sndtv = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(nsock, SOL_SOCKET, SO_SNDTIMEO,
                           &sndtv, sizeof(sndtv));

                xSemaphoreTake(s_mutex, portMAX_DELAY);
                int slot = free_slot();
                if (slot >= 0) {
                    s_clients[slot] = (rtsp_client_t){
                        .sock         = nsock,
                        .session_id   = 0,
                        .rtp_port     = { 0 },
                        .is_playing   = false,
                        .use_tcp      = { false, false },
                        .tcp_channel  = { 0, 0 },
                        .rtp_seq      = { 0, 0 },
                        .rtp_ts       = { 0, 0 },
                        .peer_ip      = { 0 },
                    };
                    inet_ntoa_r(caddr.sin_addr,
                                s_clients[slot].peer_ip,
                                sizeof(s_clients[slot].peer_ip));
                    ESP_LOGI(TAG, "New client: %s", s_clients[slot].peer_ip);
                } else {
                    ESP_LOGW(TAG, "Client limit reached — rejecting");
                    close(nsock);
                }
                xSemaphoreGive(s_mutex);
            }
        }

        /* ── Service existing RTSP connections ──────────────────────────── */
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            /* Read the socket fd under the mutex but do I/O outside it. */
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            int sock = s_clients[i].sock;
            xSemaphoreGive(s_mutex);

            if (sock < 0 || !FD_ISSET(sock, &rfds))
                continue;

            int n = recv(sock, req, sizeof(req) - 1, 0);
            if (n <= 0) {
                /* Client closed connection. */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_clients[i].sock == sock) {
                    ESP_LOGI(TAG, "Client %s gone", s_clients[i].peer_ip);
                    close(sock);
                    s_clients[i].sock       = -1;
                    s_clients[i].is_playing = false;
                }
                xSemaphoreGive(s_mutex);
                continue;
            }

            req[n] = '\0';

            /* Build response and update state — brief critical section. */
            bool   do_teardown = false;
            size_t resp_len    = 0;

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            /* Guard: socket might have been closed concurrently. */
            if (s_clients[i].sock == sock) {
                resp_len = build_response(&s_clients[i], req,
                                          resp, sizeof(resp),
                                          &do_teardown);
                if (do_teardown) {
                    s_clients[i].sock       = -1;
                    s_clients[i].is_playing = false;
                }
            }
            xSemaphoreGive(s_mutex);

            /* Send outside the mutex so audio task is never blocked. */
            if (resp_len > 0 && send_all(sock, (const uint8_t *)resp, resp_len) != 0) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_clients[i].sock == sock) {
                    ESP_LOGW(TAG, "RTSP send failed, dropping client %s", s_clients[i].peer_ip);
                    close(sock);
                    s_clients[i].sock       = -1;
                    s_clients[i].is_playing = false;
                }
                xSemaphoreGive(s_mutex);
                continue;
            }

            if (do_teardown)
                close(sock);
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

esp_err_t rtsp_audio_server_start(const rtsp_audio_server_config_t *cfg)
{
    s_sample_rate = cfg->sample_rate ? cfg->sample_rate : 16000;
    s_rtsp_port   = cfg->port        ? cfg->port        : 554;
    s_ssrc        = esp_random();

    memset(s_clients,  0,       sizeof(s_clients));
    strlcpy(s_local_ip, "0.0.0.0", sizeof(s_local_ip));
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        s_clients[i].sock = -1;
        for (int track = 0; track < MAX_RTP_TRACKS; ++track) {
            s_clients[i].rtp_port[track] = 0;
            s_clients[i].use_tcp[track] = false;
            s_clients[i].tcp_channel[track] = (track == TRACK_VIDEO) ? VIDEO_TCP_CHANNEL : AUDIO_TCP_CHANNEL;
            s_clients[i].rtp_seq[track] = 0;
            s_clients[i].rtp_ts[track] = 0;
        }
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    BaseType_t rc = xTaskCreate(rtsp_server_task, "rtsp_server",
                                RTSP_TASK_STACK, NULL,
                                RTSP_TASK_PRIORITY, NULL);
    return rc == pdPASS ? ESP_OK : ESP_FAIL;
}

static void send_rtp_packet_to_clients(const uint8_t *pkt, size_t pkt_len, int track)
{
    if (pkt_len == 0 || track < TRACK_AUDIO || track >= MAX_RTP_TRACKS)
        return;

    rtp_send_target_t targets[MAX_CLIENTS];
    int target_count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!s_clients[i].is_playing)
            continue;
        if (!s_clients[i].use_tcp[track] && s_clients[i].rtp_port[track] == 0)
            continue;

        targets[target_count].idx         = i;
        targets[target_count].sock        = s_clients[i].sock;
        targets[target_count].use_tcp     = s_clients[i].use_tcp[track];
        targets[target_count].tcp_channel = s_clients[i].tcp_channel[track];
        targets[target_count].rtp_port    = s_clients[i].rtp_port[track];
        strlcpy(targets[target_count].peer_ip,
                s_clients[i].peer_ip,
                sizeof(targets[target_count].peer_ip));
        ++target_count;
    }
    xSemaphoreGive(s_mutex);

    for (int t = 0; t < target_count; ++t) {
        if (targets[t].use_tcp) {
            uint8_t tcp_pkt[4 + RTSP_BUF_SIZE];
            tcp_pkt[0] = '$';
            tcp_pkt[1] = targets[t].tcp_channel;
            tcp_pkt[2] = (pkt_len >> 8) & 0xFF;
            tcp_pkt[3] = pkt_len & 0xFF;
            if (4 + pkt_len > sizeof(tcp_pkt))
                continue;
            memcpy(tcp_pkt + 4, pkt, pkt_len);
            if (send_all(targets[t].sock, tcp_pkt, 4 + pkt_len) != 0) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_clients[targets[t].idx].sock == targets[t].sock) {
                    ESP_LOGW(TAG, "TCP send failed, dropping client %s", s_clients[targets[t].idx].peer_ip);
                    close(s_clients[targets[t].idx].sock);
                    s_clients[targets[t].idx].sock = -1;
                    s_clients[targets[t].idx].is_playing = false;
                }
                xSemaphoreGive(s_mutex);
            }
        } else {
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port   = htons(targets[t].rtp_port),
            };
            inet_aton(targets[t].peer_ip, &dst.sin_addr);

            sendto(s_rtp_sock, pkt, pkt_len, 0,
                   (struct sockaddr *)&dst, sizeof(dst));
        }
    }
}

void rtsp_audio_server_send_video_packet(const uint8_t *pkt, size_t pkt_len)
{
    if (s_rtp_sock < 0 || !pkt || pkt_len == 0) return;
    send_rtp_packet_to_clients(pkt, pkt_len, TRACK_VIDEO);
}

void rtsp_audio_server_send_audio(const int16_t *samples, size_t n_samples)
{
    if (s_rtp_sock < 0 || !samples || n_samples == 0) return;

    rtp_send_target_t targets[MAX_CLIENTS];

    /* Fragment into MTU-safe RTP packets. */
    size_t offset = 0;
    while (offset < n_samples) {
        size_t chunk = n_samples - offset;
        if (chunk > MAX_RTP_PAYLOAD_SAMPLES)
            chunk = MAX_RTP_PAYLOAD_SAMPLES;

        const size_t payload_bytes = chunk * sizeof(int16_t);
        const size_t pkt_len       = 12 + payload_bytes;

        /* Stack-allocate the packet (max ~1440 + 12 bytes). */
        uint8_t pkt[12 + MAX_RTP_PAYLOAD_SAMPLES * sizeof(int16_t)];

        /* ── L16 payload — big-endian (network byte order) ──────────── */
        uint8_t *payload = pkt + 12;
        for (size_t j = 0; j < chunk; ++j) {
            int16_t s          = samples[offset + j];
            payload[j * 2]     = (uint8_t)((s >> 8) & 0xFF);
            payload[j * 2 + 1] = (uint8_t)( s       & 0xFF);
        }

        int target_count = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!s_clients[i].is_playing)
                continue;
            if (!s_clients[i].use_tcp[TRACK_AUDIO] && s_clients[i].rtp_port[TRACK_AUDIO] == 0)
                continue;

            targets[target_count].idx         = i;
            targets[target_count].sock        = s_clients[i].sock;
            targets[target_count].use_tcp     = s_clients[i].use_tcp[TRACK_AUDIO];
            targets[target_count].tcp_channel = s_clients[i].tcp_channel[TRACK_AUDIO];
            targets[target_count].rtp_port    = s_clients[i].rtp_port[TRACK_AUDIO];
            strlcpy(targets[target_count].peer_ip,
                    s_clients[i].peer_ip,
                    sizeof(targets[target_count].peer_ip));
            targets[target_count].rtp_seq = s_clients[i].rtp_seq[TRACK_AUDIO];
            targets[target_count].rtp_ts  = s_clients[i].rtp_ts[TRACK_AUDIO];
            ++target_count;

            ++s_clients[i].rtp_seq[TRACK_AUDIO];
            s_clients[i].rtp_ts[TRACK_AUDIO] += (uint32_t)chunk;
        }

        xSemaphoreGive(s_mutex);

        for (int t = 0; t < target_count; ++t) {
            /* Build per-client RTP header (seq/timestamp are client-specific). */
            pkt[0]  = 0x80;   /* V=2, P=0, X=0, CC=0         */
            pkt[1]  = RTP_PT_L16_DYNAMIC; /* M=0, PT=96             */
            pkt[2]  = (targets[t].rtp_seq >> 8) & 0xFF;
            pkt[3]  =  targets[t].rtp_seq       & 0xFF;
            pkt[4]  = (targets[t].rtp_ts >> 24) & 0xFF;
            pkt[5]  = (targets[t].rtp_ts >> 16) & 0xFF;
            pkt[6]  = (targets[t].rtp_ts >>  8) & 0xFF;
            pkt[7]  =  targets[t].rtp_ts        & 0xFF;
            pkt[8]  = (s_ssrc >> 24) & 0xFF;
            pkt[9]  = (s_ssrc >> 16) & 0xFF;
            pkt[10] = (s_ssrc >>  8) & 0xFF;
            pkt[11] =  s_ssrc        & 0xFF;

            if (targets[t].use_tcp) {
                /* ── RTP interleaved over RTSP/TCP ────────────────────── */
                /* RFC 2326 §10.12: $<channel><size(2 bytes)><RTP packet> */
                uint8_t tcp_pkt[4 + 12 + MAX_RTP_PAYLOAD_SAMPLES * sizeof(int16_t)];
                tcp_pkt[0] = '$';              /* magic byte           */
                tcp_pkt[1] = targets[t].tcp_channel;
                tcp_pkt[2] = (pkt_len >> 8) & 0xFF;  /* big-endian size  */
                tcp_pkt[3] =  pkt_len        & 0xFF;
                memcpy(tcp_pkt + 4, pkt, pkt_len);

                if (send_all(targets[t].sock, tcp_pkt, 4 + pkt_len) != 0) {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    if (s_clients[targets[t].idx].sock == targets[t].sock) {
                        ESP_LOGW(TAG, "TCP send failed, dropping client %s", s_clients[targets[t].idx].peer_ip);
                        close(s_clients[targets[t].idx].sock);
                        s_clients[targets[t].idx].sock = -1;
                        s_clients[targets[t].idx].is_playing = false;
                    }
                    xSemaphoreGive(s_mutex);
                }
            } else {
                /* ── RTP over UDP ─────────────────────────────────────── */
                struct sockaddr_in dst = {
                    .sin_family = AF_INET,
                    .sin_port   = htons(targets[t].rtp_port),
                };
                inet_aton(targets[t].peer_ip, &dst.sin_addr);

                sendto(s_rtp_sock, pkt, pkt_len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
            }
        }
        offset += chunk;
    }
}
