// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#pragma once

#include "secrets.h"  // WiFi credentials and server URL (not tracked by git)
#define WIFI_CONNECTED_BIT BIT0

// Button configuration
#define REWIND_BUTTON_PIN GPIO_NUM_13
#define START_BUTTON_PIN GPIO_NUM_12
#define STOP_BUTTON_PIN GPIO_NUM_14
#define BUTTON_HOLD_MS 3000 // How long the rewind button must be held (ms)
#define BUTTON_POLL_MS 50   // How often to poll the button states(ms)
#define FILES_TO_DELETE 4   // Number of audio files to delete when the rewind button is held


// WiFi backoff configuration
#define UPLOAD_FAIL_LIMIT 10
#define WIFI_RETRY_DELAY_MS 600000  // 10 minutes

// Watchdog timeout for the ReadAudioInput task (seconds).
#define AUDIO_WDT_TIMEOUT_S 10

// HTTP client timeout for StreamToServer (milliseconds)
#define HTTP_TIMEOUT_MS 10000

// SNTP / RTC configuration
#define NTP_SERVER  "pool.ntp.org"
#define TIMEZONE    "AEST-10AEDT,M10.1.0,M4.1.0/3"  // Sydney, Australia (change to your timezone as needed)

// Clipping detection threshold (absolute value)
#define CLIPPING_THRESHOLD 32000

// Microphone disconnect detection
#define MIC_SILENCE_THRESHOLD 10    // Peak amplitude (absolute) below which a buffer is considered silent
#define MIC_SILENCE_BUFFERS   10    // Consecutive silent buffers before logging a disconnect warning (~640ms at 16kHz)

// I2S audio configuration (INMP441 microphone)
#define I2S_SAMPLE_RATE     16000               // Sample rate in Hz
#define I2S_BIT_DEPTH       I2S_DATA_BIT_WIDTH_32BIT  // INMP441 outputs 32-bit, MSB-aligned — shifted to 16-bit PCM in ReadAudioInput

// I2S pin definitions (INMP441 microphone)
#define I2S_BCLK    GPIO_NUM_15
#define I2S_WS      GPIO_NUM_16
#define I2S_DIN     GPIO_NUM_4

// LED pin definition (WS2812)
#define LED_PIN     GPIO_NUM_23

// Defines the bitshift to apply to the LED color values to achieve the desired brightness (0 = full brightness, 1 = half brightness, etc.)
#define LED_BRIGHTNESS_SHIFT 4 // 1/16 brightness

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



// SD card pin definitions
#define SD_MOSI  GPIO_NUM_18
#define SD_MISO  GPIO_NUM_21
#define SD_CLK   GPIO_NUM_19
#define SD_CS    GPIO_NUM_5
#define SD_MOUNT "/sdcard"
