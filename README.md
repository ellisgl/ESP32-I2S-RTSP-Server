# ESP32-I2S-RTSP-Server

A minimal RTSP/RTP audio-only streaming server for the ESP32, designed to capture audio from I2S microphones and stream it to multiple clients via RTSP protocol.

## Description

This project implements a lightweight RTSP 1.0 (RFC 2326) server on the ESP32 that captures audio from an I2S digital microphone and streams it to up to 4 simultaneous RTSP clients. The audio is encoded in RTP L16 PCM mono format (big-endian) and transmitted over UDP, with RTSP control signaling over TCP.

The server features:
- **Real-time audio capture** from I2S digital microphones (e.g., INMP441, ICS-43434)
- **Configurable sample rates**: 16 kHz or 44.1 kHz
- **Audio processing**: DC blocking and click limiting to improve audio quality
- **Multi-client support**: Up to 4 concurrent RTSP sessions
- **WiFi integration**: Connects to your network for remote streaming
- **Minimal footprint**: Optimized for resource-constrained ESP32 devices

## Features

- ✅ RTSP 1.0 protocol compliance
- ✅ RTP payload type 96 (L16 PCM mono, 16-bit big-endian)
- ✅ Configurable audio parameters (sample rate, frame size, bit depth)
- ✅ DC blocking filter to remove DC offset from microphone input
- ✅ Click limiting to suppress audio spikes and clicks
- ✅ Left/Right channel slot selection for stereo microphones
- ✅ UDP unicast streaming
- ✅ ESP-IDF 5.x / 6.x compatible
- ✅ Non-blocking I/O using select()
- ✅ Thread-safe audio frame handling with mutex protection

## Requirements

### Hardware

- **ESP32 microcontroller** (ESP32-WROOM-32 or similar)
- **I2S digital microphone** (INMP441, ICS-43434, or compatible)
- **WiFi connectivity** (built-in to ESP32)
- **USB serial adapter** (for flashing and debugging)
- **Power supply** (5V recommended)

### Microphone Wiring

The project is configured for the following I2S pin assignments (customizable in `app_config.h`):
- **I2S_SCK_PIN** (GPIO 14): Bit clock (BCLK)
- **I2S_WS_PIN** (GPIO 15): Word select / LRCLK
- **I2S_SD_PIN** (GPIO 13): Serial data input

### Software

- **ESP-IDF** v5.0 or later (tested with v6.0)
- **GCC** toolchain for ESP32
- **Python 3.x** with esptool (for flashing)

## Installation

### 1. Prerequisites

Install ESP-IDF v6.0 or later:
```bash
git clone https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v6.0
./install.sh
source export.sh
```

### 2. Clone the Repository

```bash
git clone https://github.com/your-username/ESP32-I2S-RTSP-Server.git
cd ESP32-I2S-RTSP-Server
```

### 3. Configure the Project

Edit `main/app_config.h` to set:
- **WiFi credentials**: `WIFI_SSID` and `WIFI_PASSWORD`
- **I2S pin assignments**: `I2S_SCK_PIN`, `I2S_WS_PIN`, `I2S_SD_PIN`
- **Audio parameters**: `SAMPLE_RATE`, `FRAME_SAMPLES`, `INPUT_BITS`, `OUTPUT_BITS`
- **Microphone channel**: `MIC_USE_LEFT_SLOT` (1 for left, 0 for right)
- **Audio processing**: `DC_BLOCK_SHIFT`, `CLICK_LIMIT_DELTA`

### 4. Build the Project

```bash
idf.py build
```

### 5. Flash the ESP32

```bash
idf.py flash
```

### 6. Monitor Serial Output

```bash
idf.py monitor
```

The device will connect to WiFi and start the RTSP server. Watch for the IP address in the logs.

## Usage

### Starting the Server

Once flashed and powered on, the ESP32 will:
1. Connect to the configured WiFi network
2. Initialize the I2S audio input
3. Start the RTSP server on port 554
4. Wait for client connections

### Streaming Audio

Connect an RTSP client to the server:

```bash
# Using ffplay
ffplay rtsp://<ESP32_IP>/audio

# Using vlc
vlc rtsp://<ESP32_IP>/audio

# Using gstreamer
gst-launch-1.0 rtspsrc location=rtsp://<ESP32_IP>/audio ! queue ! rtpL16depay ! audioconvert ! alsasink

# Using curl to test RTSP protocol
curl -v rtsp://<ESP32_IP>/audio
```

Replace `<ESP32_IP>` with the actual IP address of your ESP32 (visible in the serial monitor).

### Multiple Clients

The server supports up to 4 simultaneous RTSP clients. Each client can independently control playback via RTSP commands (PLAY, PAUSE, TEARDOWN).

## Configuration

All configuration is done in `main/app_config.h`:

### WiFi Settings
```c
#define WIFI_SSID       "YourSSID"
#define WIFI_PASSWORD   "YourPassword"
```

### Audio Parameters
```c
#define SAMPLE_RATE     44100   // Hz (8000-48000)
#define FRAME_SAMPLES   512     // Samples per read call (128-2048)
#define INPUT_BITS      32      // Input bit depth from microphone
#define OUTPUT_BITS     16      // Output bit depth for RTP
```

### Audio Processing
```c
#define DC_BLOCK_SHIFT      10      // DC blocker strength (larger = lower cutoff)
#define CLICK_LIMIT_DELTA   12000   // Max sample jump (0 to disable)
```

### RTSP Settings
```c
#define RTSP_PORT       554     // RTSP port (default)
```

## Troubleshooting

**Q: No WiFi connection**
- Verify SSID and password in `app_config.h`
- Check WiFi signal strength
- Monitor logs for connection errors

**Q: Audio glitching or dropouts**
- Reduce `FRAME_SAMPLES` for lower latency
- Increase `DC_BLOCK_SHIFT` for stronger filtering
- Check for CPU overload (monitor free heap)

**Q: RTSP client connection fails**
- Verify ESP32 IP address in serial monitor
- Ensure ESP32 and client are on the same network
- Check firewall settings (port 554 and UDP ports 5004-5005)

**Q: Poor audio quality**
- Adjust `CLICK_LIMIT_DELTA` to suppress clicks
- Check microphone placement and wiring
- Verify I2S pin configuration matches your hardware

## License

This project is licensed under the BSD 3-Clause License. See [LICENSE](LICENSE) for details.

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests to improve the project.
