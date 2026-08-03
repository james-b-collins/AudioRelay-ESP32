# AudioRelay ESP32

An ESP32 audio recorder that captures from an INMP441 MEMS microphone, buffers to a microSD card, and uploads to a server over WiFi.

This was a project I made to record my band's rehearsals and gigs and automate uploading them to my server. I recently cleaned it up and figured I'd put it up here in case anyone finds it useful.

## Features

- Continuous audio capture via I2S (16kHz, 16-bit mono PCM)
- Buffered writes to microSD (~32 seconds per file)
- Automatic upload to an HTTP server
- WS2812 status LED
- Start/stop recording via hardware buttons
- Rewind button to delete the last 2 minutes of audio (hold 3 seconds)
- Light sleep when stopped to save power
- WiFi backoff: shuts the radio down and retries after 10 minutes if uploads keep failing (saves power)
- Watchdog on the audio and SD write tasks
- If the SD card fills up, recording pauses until uploads free some space

## Hardware

| Component | Part |
|---|---|
| Microcontroller | ESP32 (tested on ESP32-WROOM-32) |
| Microphone | INMP441 MEMS (I2S) |
| Storage | MicroSD card via SPI |
| Status LED | WS2812 addressable LED |

## Pin Mapping

All pin definitions are in `main/config.h` and can be changed to match your wiring.

### INMP441 Microphone (I2S)

| Signal | GPIO |
|---|---|
| BCLK | 15 |
| WS | 16 |
| DIN | 4 |

### MicroSD Card (SPI)

| Signal | GPIO |
|---|---|
| MOSI | 18 |
| MISO | 21 |
| CLK | 19 |
| CS | 5 |

### Buttons

| Button | GPIO | Behaviour |
|---|---|---|
| Start | 12 | Resume recording |
| Stop | 14 | Pause recording, delete in-progress file |
| Rewind | 13 | Hold 3s to delete last ~2 minutes |

### LED

| Pin | GPIO |
|---|---|
| Data | 23 |

### LED Status Colours

| Colour | Meaning |
|---|---|
| Red | Recording |
| White | Paused |
| Blue / Yellow flashing | Rewind hold warning |
| Green (1s) | Rewind confirmed |
| Orange flashing | SD card full, waiting for space |
| Magenta flashing | Fatal error (I2S init failed) or LED tamper detected |

## Configuration

### WiFi, server, and API key

Rename `main/secrets.h.example` to `main/secrets.h` and fill in your details:

```c
#define WIFI_SSID  "your-network"
#define WIFI_PASS  "your-password"
#define SERVER_URL "http://your-server/upload"
#define API_KEY    "your-api-key"  // must match the API_KEY set in docker-compose.yml
```

`main/secrets.h` is in `.gitignore` so it won't be committed if you decide to fork or contribute.

### Other settings

Everything else is in `main/config.h`:

```c
#define I2S_SAMPLE_RATE     16000       // Hz
#define UPLOAD_FAIL_LIMIT   10          // consecutive failures before WiFi backoff
#define WIFI_RETRY_DELAY_MS 600000      // backoff duration (ms)
#define HTTP_TIMEOUT_MS     10000       // HTTP request timeout (ms)
#define AUDIO_WDT_TIMEOUT_S 10          // watchdog timeout (seconds)
```

## Audio Format

Files are saved to the SD card as raw 16-bit signed PCM, 16kHz mono (`/sdcard/audio_NNNN.raw`). The INMP441 outputs 32-bit MSB-aligned data which gets right-shifted to 16-bit in firmware.

To open a file in [Audacity](https://www.audacityteam.org/):

1. **File > Import > Raw Data**
2. Encoding: **Signed 16-bit PCM**
3. Byte order: **Little-endian**
4. Channels: **1 (Mono)**
5. Sample rate: **16000 Hz**

## LED Integrity
The [LED-Integrity](https://github.com/james-b-collins/led-integrity) library is supported as a way to ensure the LED has not been removed or tampered with. You can enable and configure this library in the config.h file. The defaults are already configured for a WS2812B LED. Enabling this setting will cause the device to immediately stop recording and uploading audio until restarted.
```C
// LED tamper detection (led-integrity library). 1 to enable, 0 to disable
#define LED_INTEGRITY_ENABLED 0

#if LED_INTEGRITY_ENABLED
#define LED_DO_PIN                       GPIO_NUM_22
#define LED_INTEGRITY_PROBE_WINDOW_US    10000      // Time before timeout on the DO line after a probe frame is sent (us)
#define LED_INTEGRITY_MAX_MISSED_PROBES  3          // number of consecutive missed probes before tamper is latched
#define LED_INTEGRITY_PROBE_INTERVAL_MS  10000      // Time between probes (ms)
#define LI_PROP_DELAY_MIN                10         // Minimum valid delay after sending a probe frame (us)
#define LI_PROP_DELAY_MAX                100        // Maximum valid delay after sending a probe frame (us)
#endif
```

## Build & Flash

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/).

```bash
idf.py build
idf.py -p PORT flash monitor
```

## Architecture

Seven FreeRTOS tasks run concurrently:

| Task | Priority | Description |
|---|---|---|
| `ReadAudioInput` | 5 | Reads I2S DMA, converts to 16-bit PCM, pushes to queue |
| `WriteToSD` | 3 | Drains queue, writes 500-chunk files to SD |
| `StreamToServer` | 1 | Uploads completed files via HTTP POST, deletes on success |
| `RewindButtonTask` | 2 | Monitors rewind button (3s hold to delete) |
| `StopButtonTask` | 2 | Monitors stop button, triggers light sleep |
| `StartButtonTask` | 2 | Monitors start button, wakes and resumes recording |
| `ClippingIndicatorTask` | 2 | Flashes LED rapidly when audio clipping is detected |

Audio goes: `ReadAudioInput` -> queue -> `WriteToSD` -> SD card -> `StreamToServer` -> server. Uploading runs independently and never blocks recording.

## Server

A minimal Flask server is included in `server/docker-compose.yml`. It accepts POST uploads authenticated with an API key and saves files to a local volume.

### Deployment

1. Edit `docker-compose.yml` and set `API_KEY` to a random string, and uncomment/update the volume path to point to where you want audio files stored:

   ```yaml
   volumes:
     - /mnt/your-pool/AudioRelay:/data/audio
   ```

2. Start the server:

   ```bash
   docker compose -f server/docker-compose.yml up -d
   ```

3. The server listens on port `5000`. Set `SERVER_URL` in `secrets.h` to `http://your-server:5000/upload` and `API_KEY` to the same value you set in `docker-compose.yml`.

A `/health` endpoint is available at `http://your-server:5000/health` and returns the number of files received and total size.
