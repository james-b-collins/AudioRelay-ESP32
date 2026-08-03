// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_task_wdt.h"
#include "config.h"
#include "audio.h"
#include "led.h"

//configure DMA for I2S audio input (INMP441). Returns true on success, false on failure.
bool ConfigDMA(){
    // Configure DMA for audio data transfer
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 6; // Number of DMA descriptors
    chanCfg.dma_frame_num = 1023; // Number of frames per DMA buffer

    esp_err_t ret = i2s_new_channel(&chanCfg, NULL, &rxChannel);
    if (ret != ESP_OK) {
        printf("Failed to create I2S channel: %s\n", esp_err_to_name(ret));
        return false;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BIT_DEPTH, I2S_SLOT_MODE_MONO), // Mono — INMP441 uses left channel only
        //Pin configuration for I2S signals
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DIN,
            .invert_flags = {
                .mclk_inv = 0,
                .bclk_inv = 0,
                .ws_inv = 0
            }
        }
    };

    ret = i2s_channel_init_std_mode(rxChannel, &stdCfg);
    if (ret != ESP_OK) {
        printf("Failed to initialise I2S standard mode: %s\n", esp_err_to_name(ret));
        return false;
    }

    return true;
}

atomic_bool clippingActive = false; // Set true when a clipped sample is detected — read by ClippingIndicatorTask

// Task to read audio data from INMP441(I2S), convert it to 16-bit PCM, and send it to the audio queue
void ReadAudioInput(void* pvParameters) {
    // Subscribe this task to the watchdog
    esp_task_wdt_add(NULL);

    int32_t buffer[1024]; // Buffer to hold raw data
    size_t bytesRead;     // Variable to hold the number of bytes read from the I2S channel
    for(;;) {
        // Stop capturing if led tampering is detected
        if (atomic_load(&tamperDetected)) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        i2s_channel_read(rxChannel, buffer, sizeof(buffer), &bytesRead, portMAX_DELAY);

        // Notify the watchdog that task is still alive
        esp_task_wdt_reset();

        int16_t* audioBuffer = malloc(1023 * sizeof(int16_t));
        if(audioBuffer == NULL) {
            printf("Failed to allocate memory for audio buffer\n");
            continue;
        }

        //Convert the 32-bit raw data from the INMP441 to 16-bit PCM format by right-shifting and truncating
        int16_t peak = 0;
        for (int i = 0; i < 1023; i++) {
            audioBuffer[i] = (int16_t)(buffer[i] >> 16);
            if (audioBuffer[i] > CLIPPING_THRESHOLD || audioBuffer[i] < -CLIPPING_THRESHOLD) {
                atomic_store(&clippingActive, true); // Flag clipping for ClippingIndicatorTask
            }
            int16_t abs_val = audioBuffer[i] < 0 ? -audioBuffer[i] : audioBuffer[i];
            if (abs_val > peak) peak = abs_val;
        }

        // Mic disconnect detection: a real INMP441 always has some noise — sustained near-zero peak likely means the mic is not connected
        static int silentBuffers = 0;
        if (peak < MIC_SILENCE_THRESHOLD) {
            silentBuffers++;
            if (silentBuffers == MIC_SILENCE_BUFFERS) {
                printf("Warning: microphone may be disconnected (peak amplitude below %d for ~%dms)\n",
                       MIC_SILENCE_THRESHOLD, (MIC_SILENCE_BUFFERS * 1023 * 1000) / I2S_SAMPLE_RATE);
            }
        } else {
            silentBuffers = 0;
        }

        //Send the audio buffer to the queue for processing by other tasks. If the queue is full, print an error and free the buffer
        if(xQueueSend(audioQueue, &audioBuffer, 0) != pdTRUE) {
            printf("Failed to send audio buffer to queue\n");
            free(audioBuffer);
        }
    }
}

// Task to flash the LED rapidly when audio clipping is detected
void ClippingIndicatorTask(void *pvParameters) {
    for (;;) {
        if (recordingActive && atomic_load(&clippingActive)) {
            atomic_store(&clippingActive, false);

            // Flash red rapidly to warn of clipping
            for (int i = 0; i < 6; i++) {
                SetLED(255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
                SetLED(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            SetLED(255, 0, 0); // Restore red after flashing
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
