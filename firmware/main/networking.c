// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "config.h"
#include "networking.h"
#include "storage.h"
#include "led.h"

// Flag set during the backoff window to prevent the event handler from reconnecting
static atomic_bool wifiBackoffActive = false;
static atomic_int connectFailures = 0;
static TimerHandle_t wifiRestartTimer = NULL;

// Called after the backoff delay. clears the flag and restarts the WiFi driver
static void WifiRestartCallback(TimerHandle_t xTimer) {
    atomic_store(&wifiBackoffActive, false);
    esp_wifi_start();
}

// WiFi event handler to manage connection. Reconnect on disconnect and set event bits on successful connection.
// After UPLOAD_FAIL_LIMIT consecutive failures, backs off for WIFI_RETRY_DELAY_MS before retrying.
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifiEventGroup, WIFI_CONNECTED_BIT);
        if (atomic_load(&wifiBackoffActive)) return;
        if (atomic_fetch_add(&connectFailures, 1) + 1 >= UPLOAD_FAIL_LIMIT) {
            printf("WiFi: failed to connect %d times, backing off for %d minutes...\n",
                UPLOAD_FAIL_LIMIT, WIFI_RETRY_DELAY_MS / 60000);
            atomic_store(&wifiBackoffActive, true);
            atomic_store(&connectFailures, 0);
            esp_wifi_stop();
            xTimerStart(wifiRestartTimer, 0);
        } else {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&connectFailures, 0);
        xEventGroupSetBits(wifiEventGroup, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi in station mode and connect to the home network
void InitWifi(){
    wifiEventGroup = xEventGroupCreate();

    nvs_flash_init();                       // Initialize NVS for WiFi credentials storage
    esp_netif_init();                       // Initialize the TCP/IP stack
    esp_event_loop_create_default();        // Create default event loop
    esp_netif_create_default_wifi_sta();    // Create default WiFi station

    //Initialize WiFi with default configuration and register event handlers
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    // Set WiFi configuration with SSID and password
    wifi_config_t wifiConfig = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    wifiRestartTimer = xTimerCreate("wifiRestart", pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS), pdFALSE, NULL, WifiRestartCallback);

    // Set WiFi mode to station and start the WiFi driver
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    esp_wifi_start();
}

// Task to read audio files from SD card and upload them to a server via HTTP POST
void StreamToServer(void* pvParameters) {
    char filePath[64];          //Buffer to hold the file path of the audio file to be uploaded
    int uploadIndex = 0;        //Counter to keep track of which audio file to upload next
    int consecutiveFailures = 0;//Counter for consecutive upload failures

    for(;;) {
        // Stop uploading once LED tamper is detected
        if (atomic_load(&tamperDetected)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        //wait for wifi connection
        xEventGroupWaitBits(wifiEventGroup, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

        //check if files have been created by the WriteToSD task. If not, wait and retry
        if (uploadIndex >= writeIndex) {
            TryRefillQueue(uploadIndex);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        // Skip upload if no server URL is configured
        if (strlen(SERVER_URL) == 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        //Get the path of the next audio file to upload from the shared filenameTable
        strncpy(filePath, filenameTable[uploadIndex % FILENAME_TABLE_SIZE], sizeof(filePath));

        //Attempt to open the audio file for reading
        FILE* f = fopen(filePath, "rb");
        if(f == NULL) {
            if (errno == ENOENT) {
                // File was deleted by the rewind button before it could be uploaded — skip it
                printf("Skipping %s, file was deleted\n", filePath);
                uploadIndex++;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000)); // Error, wait before retrying
            }
            continue;
        }
        // Get file size for Content-Length header
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);

        // Upload via HTTP POST, streaming the file in chunks to avoid loading it all into heap
        esp_http_client_config_t config = {
            .url = SERVER_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = HTTP_TIMEOUT_MS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        esp_http_client_set_header(client, "X-API-Key", API_KEY);

        //extract filename from path and strip audio_ prefix
        char* filename = strrchr(filePath, '/');
        filename = filename ? filename + 1 : filePath;
        if (strncmp(filename, "audio_", 6) == 0) {
            filename += 6;
        }
        esp_http_client_set_header(client, "X-Filename", filename);

        char chunk[1024];
        bool uploadOk = false;

        esp_err_t err = esp_http_client_open(client, fileSize);
        if (err != ESP_OK) {
            printf("Failed to open HTTP connection for %s: %s\n", filePath, esp_err_to_name(err));
        } else {
            // Stream file contents to server in 1KB chunks
            size_t bytesRead;
            bool writeFailed = false;
            while ((bytesRead = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (esp_http_client_write(client, chunk, bytesRead) < 0) {
                    printf("Failed to write chunk for %s\n", filePath);
                    writeFailed = true;
                    break;
                }
            }

            if (!writeFailed) {
                int statusCode = esp_http_client_fetch_headers(client);
                if (statusCode >= 0 && esp_http_client_get_status_code(client) == 200) {
                    uploadOk = true;
                }
            }
        }

        fclose(f);

        if (uploadOk) {
            printf("Uploaded %s\n", filePath);
            remove(filePath);
            uploadIndex++;
            consecutiveFailures = 0;   // Reset failure counter on success
        } else {
            printf("Failed to upload %s\n", filePath);
            consecutiveFailures++;

            // If too many consecutive failures, shut down WiFi and retry after backoff delay (saves power)
            if (consecutiveFailures >= UPLOAD_FAIL_LIMIT && !atomic_load(&wifiBackoffActive)) {
                printf("Upload failed %d consecutive times, shutting down WiFi for %d minutes...\n",
                    UPLOAD_FAIL_LIMIT, WIFI_RETRY_DELAY_MS / 60000);
                atomic_store(&wifiBackoffActive, true);
                esp_wifi_stop();
                xTimerStart(wifiRestartTimer, 0);
                consecutiveFailures = 0;
            }
        }
        esp_http_client_cleanup(client);
    }
}
