// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#include <stdio.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "config.h"
#include "audio.h"
#include "storage.h"
#include "networking.h"
#include "led.h"
#include "buttons.h"
#include "clock.h"
#include "esp_task_wdt.h"

// Global variables
EventGroupHandle_t wifiEventGroup;              // Event group to manage WiFi connection status
i2s_chan_handle_t rxChannel;                    // I2S channel handle for audio input
QueueHandle_t audioQueue;                       // Queue to hold audio data buffers
led_strip_handle_t led;                         // LED strip handle for status indication
atomic_int writeIndex = 0;                      // Shared between WriteToSD and StreamToServer — atomic to prevent data race on concurrent read/write
char filenameTable[FILENAME_TABLE_SIZE][64];    // Circular buffer of generated file paths, indexed by writeIndex / uploadIndex
TaskHandle_t audioTaskHandle;                   // Handle for the ReadAudioInput task (used by start/stop buttons)
atomic_bool recordingActive = true;             // Shared between button tasks and WriteToSD — atomic to prevent data race on concurrent read/write

// Initialize FreeRTOS tasks
void InitTasks() {
    audioQueue = xQueueCreate(10, sizeof(int16_t*)); // Create a queue for audio data
    xTaskCreate(ReadAudioInput, "ReadAudioInput", 8192, NULL, 5, &audioTaskHandle);
    xTaskCreate(WriteToSD, "WriteToSD", 4096, NULL, 3, NULL);
    xTaskCreate(StreamToServer, "StreamToServer", 8192, NULL, 1, NULL);
    xTaskCreate(RewindButtonTask, "RewindButtonTask", 2048, NULL, 2, NULL);
    xTaskCreate(StopButtonTask, "StopButtonTask", 2048, NULL, 2, NULL);
    xTaskCreate(StartButtonTask, "StartButtonTask", 2048, NULL, 2, NULL);
    xTaskCreate(ClippingIndicatorTask, "ClippingIndicatorTask", 2048, NULL, 2, NULL);
}

void InitWatchdog() {
    // Initialise the watchdog for ReadAudioInput task. Trigger a reboot if the task fails
    esp_task_wdt_config_t wdtCfg = {
        .timeout_ms    = AUDIO_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t wdtErr = esp_task_wdt_init(&wdtCfg);
    if (wdtErr == ESP_ERR_INVALID_STATE) {
        // TWDT was already enabled via sdkconfig, just reconfigure it
        esp_task_wdt_reconfigure(&wdtCfg);
    }
}

static void FatalErrorLoop(void) {
      for (;;) {
          SetLED(255, 0, 255);
          vTaskDelay(pdMS_TO_TICKS(100));
          SetLED(0, 0, 0);
          vTaskDelay(pdMS_TO_TICKS(100));
      }
  }

// Main application entry point
void app_main(void)
{
    printf("AudioRelay ESP32 starting...\n");
    InitLED(); // Initialise first so the LED can signal any fatal errors below
    InitLEDIntegrity(); // Initialize the LED integrity library and configure the DO pin for rising-edge sensing
    if (!ConfigDMA()) {
        // I2S init failed — flash magenta and abort. Watchdog will reboot the device.
        printf("Fatal: I2S init failed\n");
        FatalErrorLoop();
    }
    InitButton();
    InitSD();
    QueueExistingFiles();
    InitWifi();
    InitSNTP();
    esp_err_t err = i2s_channel_enable(rxChannel);
    if (err != ESP_OK) {
        printf("Fatal: failed to enable I2S channel: %s\n", esp_err_to_name(err));
        FatalErrorLoop();
    }
    InitWatchdog();
    InitTasks();
}
