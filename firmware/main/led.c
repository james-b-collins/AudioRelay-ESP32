// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#include <stdio.h>
#include <stdatomic.h>
#include "driver/gpio.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"
#include "led.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "audio.h"
#if LED_INTEGRITY_ENABLED
#include "led_integrity.h"  // LI_PROP_DELAY_MIN/MAX (config.h) must be defined before this include
#endif

static SemaphoreHandle_t ledMutex; // Prevents concurrent LED access from multiple tasks

atomic_bool tamperDetected = false; // Stays false unless LED_INTEGRITY_ENABLED latches it

#if LED_INTEGRITY_ENABLED
static li_state_t      liGuard;                 // LED integrity guard state
static volatile bool   liInProbeTx  = false;    // True while a probe frame is being clocked out
static volatile bool   liActive     = false;    // True once the guard has been initialised

// Platform hook: send a probe frame down the LED chain to toggle the DO line and log the time it was sent
void li_send_probe(void)
{
    led_strip_set_pixel(led, 1, 1, 0, 0);
    liGuard.probe_send_time = li_get_tick();
    liInProbeTx = true;
    led_strip_refresh(led);
    liInProbeTx = false;
    led_strip_set_pixel(led, 1, 0, 0, 0);
}

// Platform hook: current time in microseconds.
uint32_t li_get_tick(void)
{
    return (uint32_t)esp_timer_get_time();
}

// Rising-edge ISR on the DO sense pin.
static void IRAM_ATTR LEDDoPinISR(void *arg)
{
    li_state_t *state = (li_state_t *)arg;
    // Ignore the probe frame's trailing bit-edges once the window has closed.
    if (liInProbeTx && !state->window_open) {
        return;
    }
    li_signal_edge(state, (uint32_t)esp_timer_get_time());
}

// Tamper callback.
static void IRAM_ATTR OnLEDTamper(void)
{
    atomic_store(&tamperDetected, true);
}

// Latch tamper and permanently halt recording and syncing until the device restarts.
static void HandleTamper(void)
{
    printf("TAMPER DETECTED. halting recording and syncing until restart\n");
    recordingActive = false;   
    esp_wifi_stop(); 
}

// Task to manage the LED integrity guard. Periodically probes the LED chain and checks for tampering.
static void LEDIntegrityTask(void *pvParameters)
{
    for (;;) {
        xSemaphoreTake(ledMutex, portMAX_DELAY);
        li_tick(&liGuard);
        li_probe(&liGuard, li_get_tick());
        xSemaphoreGive(ledMutex);

        // If tamper is detected, halt recording and syncing, and flash magenta indefinitely.
        if (atomic_load(&tamperDetected)) {
            HandleTamper();
            for (;;) {
                SetLED(255, 0, 255);
                vTaskDelay(pdMS_TO_TICKS(500));
                SetLED(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LED_INTEGRITY_PROBE_INTERVAL_MS));
    }
}
#endif

//Initialize the status LED (WS2812)
void InitLED() {
    ledMutex = xSemaphoreCreateMutex(); // Create before first LED call
    // Configure the LED strip (WS2812) on LED_PIN. One extra pixel is allocated for the
    // led-integrity probe when tamper detection is enabled.
    led_strip_config_t stripCfg = {
        .strip_gpio_num = LED_PIN,
        .max_leds = 1 + LED_INTEGRITY_ENABLED,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB
    };
    // RMT config for WS2812 timing
    led_strip_rmt_config_t rmtCfg = {
        .resolution_hz = 10 * 1000 * 1000   // 10MHz resolution for WS2812
    };
    led_strip_new_rmt_device(&stripCfg, &rmtCfg, &led);
    SetLED(0, 0, 0);
    SetLED(255, 0, 0); // Set LED to red to indicate startup
}

// Initialize the LED integrity library and configure the DO pin for rising-edge sensing
void InitLEDIntegrity(void) {
#if LED_INTEGRITY_ENABLED
    // Configure the DO pin for rising-edge sensing and attach the ISR
    gpio_config_t doConf = {
        .pin_bit_mask = (1ULL << LED_DO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&doConf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(LED_DO_PIN, LEDDoPinISR, &liGuard);

    li_init(&liGuard, LED_INTEGRITY_PROBE_WINDOW_US, LED_INTEGRITY_MAX_MISSED_PROBES, OnLEDTamper);
    liActive = true;

    xTaskCreate(LEDIntegrityTask, "LEDIntegrity", 2560, NULL, 2, NULL);
    printf("LED integrity protection active on DO pin %d\n", LED_DO_PIN);
#endif
}

// Set the LED colour, scaling each channel down by LED_BRIGHTNESS_SHIFT to control brightness
void SetLED(uint32_t red, uint32_t green, uint32_t blue) {
    xSemaphoreTake(ledMutex, portMAX_DELAY);
    led_strip_set_pixel(led, 0, red >> LED_BRIGHTNESS_SHIFT, green >> LED_BRIGHTNESS_SHIFT, blue >> LED_BRIGHTNESS_SHIFT);
#if LED_INTEGRITY_ENABLED
    // If the LED integrity guard is active and tamper has not been detected, probe the LED chain. Otherwise, just refresh the LED strip.
    if (liActive && !liGuard.tampered) {
        li_probe(&liGuard, li_get_tick());
    } else {
        led_strip_refresh(led);
    }
#else
    led_strip_refresh(led);
#endif
    xSemaphoreGive(ledMutex);
}
