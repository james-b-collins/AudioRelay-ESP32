// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "config.h"
#include "storage.h"
#include "audio.h"
#include "led.h"
#include "buttons.h"

// Delete the last 2 minutes of audio files from the SD card
static void DeleteLastTwoMinutes() {
    int start = writeIndex - FILES_TO_DELETE;
    if (start < 0) start = 0;

    for (int i = start; i < writeIndex; i++) {
        const char *filePath = filenameTable[i % FILENAME_TABLE_SIZE];
        if (remove(filePath) == 0) {
            printf("Deleted %s\n", filePath);
        } else {
            printf("Failed to delete %s\n", filePath);
        }
    }
    printf("Last 2 minutes of audio deleted (%d file(s))\n", writeIndex - start);
}

// Configure all button GPIOs as inputs with internal pull-ups (buttons connect pin to GND)
void InitButton() {
    gpio_config_t ioConf = {
        .pin_bit_mask = (1ULL << REWIND_BUTTON_PIN) | (1ULL << START_BUTTON_PIN) | (1ULL << STOP_BUTTON_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&ioConf);
}

// Task to monitor the rewind button and delete the last 2 minutes of audio when held for 3 seconds.
void RewindButtonTask(void *pvParameters) {
    for (;;) {
        // Ignore buttons once LED tamper is detected.
        if (atomic_load(&tamperDetected)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        // Wait for button press
        if (gpio_get_level(REWIND_BUTTON_PIN) == 0) {
            int elapsed = 0;

            // Flash LED between blue and yellow while the button is held
            while (gpio_get_level(REWIND_BUTTON_PIN) == 0 && elapsed < BUTTON_HOLD_MS) {
                if ((elapsed / 250) % 2 == 0) {
                    SetLED(0, 0, 255);   // Blue
                } else {
                    SetLED(255, 255, 0); // Yellow
                }
                vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                elapsed += BUTTON_POLL_MS;
            }

            //When the button is pressed long enough trigger the deletion of the last 2 minutes
            if (elapsed >= BUTTON_HOLD_MS) {
                printf("Button held for 3 seconds, deleting last 2 minutes of audio...\n");
                DeleteLastTwoMinutes();

                // Flash green to confirm the deletion
                SetLED(0, 255, 0); // Green
                vTaskDelay(pdMS_TO_TICKS(1000));

                // Wait for the button to be released before continuing to avoid multiple deletions
                while (gpio_get_level(REWIND_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                }
            }

            // Restore LED to red (recording/standby state)
            SetLED(255, 0, 0); // Red
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS)); // Poll every 50ms
    }
}

// Task to monitor the stop button. Pauses recording and deletes any file currently being written.
void StopButtonTask(void *pvParameters) {
    for (;;) {
        // Ignore buttons once LED tamper is detected.
        if (atomic_load(&tamperDetected)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (gpio_get_level(STOP_BUTTON_PIN) == 0 && recordingActive) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS)); // Debounce
            if (gpio_get_level(STOP_BUTTON_PIN) == 0) {
                printf("Stop button pressed, pausing recording...\n");

                // Suspend the audio task so no new data enters the queue
                recordingActive = false;
                vTaskSuspend(audioTaskHandle);

                // Flush any remaining buffers from the queue
                int16_t* stale;
                while (xQueueReceive(audioQueue, &stale, 0) == pdTRUE) {
                    free(stale);
                }

                // Set LED to white to indicate recording is paused
                SetLED(255, 255, 255); // White

                // Wait for button release
                while (gpio_get_level(STOP_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                }

                // Enter light sleep — the start button press will wake the device
                gpio_wakeup_enable(START_BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
                esp_sleep_enable_gpio_wakeup();
                printf("Entering light sleep, press start to resume...\n");
                esp_light_sleep_start();

                // Disable GPIO wakeup
                esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS)); // Poll every 50ms
    }
}

// Task to monitor the start button. Resumes recording after it has been paused.
void StartButtonTask(void *pvParameters) {
    for (;;) {
        // Ignore buttons once LED tamper is detected.
        if (atomic_load(&tamperDetected)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (gpio_get_level(START_BUTTON_PIN) == 0 && !recordingActive) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS)); // Debounce
            if (gpio_get_level(START_BUTTON_PIN) == 0) {
                printf("Start button pressed, resuming recording...\n");

                // Resume the audio task and signal WriteToSD to start a new file
                recordingActive = true;
                vTaskResume(audioTaskHandle);

                // Restore LED to red (recording state)
                SetLED(255, 0, 0); // Red

                // Wait for button release
                while (gpio_get_level(START_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS)); // Poll every 50ms
    }
}
