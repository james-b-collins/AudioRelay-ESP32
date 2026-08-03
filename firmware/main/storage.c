// Copyright (c) 2026 James Collins
// Licensed freely under the MIT License. See LICENSE in the project root for details.

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_task_wdt.h"
#include "config.h"
#include "storage.h"
#include "audio.h"
#include "led.h"
#include "clock.h"

static DIR *existingFilesDir = NULL;        // Kept open between QueueExistingFiles and TryRefillQueue — NULL when empty
static SemaphoreHandle_t writeIndexMutex;   // Serialises writeIndex writes between WriteToSD and TryRefillQueue
static int nextFileNumber = 0;              // Highest file number found on SD + 1, used by WriteToSD to avoid collisions with existing files

// Initialize SD card and mount the filesystem.
void InitSD(){
    //configure SPI bus for SD card
    spi_bus_config_t busCfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1
    };
    spi_bus_initialize(SPI3_HOST, &busCfg, SPI_DMA_CH_AUTO);

    // Configure the SD card slot
    sdspi_device_config_t slotCfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slotCfg.gpio_cs = SD_CS;
    slotCfg.host_id = SPI3_HOST;

    // Mount the SD card filesystem
    sdmmc_card_t* card;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    //Configure FAT filesystem mount options
    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // Attempt to mount the SD card and handle errors
    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slotCfg, &mountCfg, &card);
    if (ret != ESP_OK) {
        printf("SD mount failed: %s\n", esp_err_to_name(ret));
        return;
    }
    printf("SD card mounted. Size: %lluMB\n", ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));

}

// Scan the SD card for existing .raw files and load into filenameTable
// Must be called before InitTasks() so writeIndex can be set without a race.
void QueueExistingFiles(void) {
    writeIndexMutex = xSemaphoreCreateMutex();

    DIR *dir = opendir(SD_MOUNT);
    if (dir == NULL) {
        printf("QueueExistingFiles: failed to open SD card directory\n");
        return;
    }

    // First pass: find the highest file number to avoid collisions with existing files
    struct dirent *entry;
    int maxFileNumber = -1;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".raw") != 0) continue;
        char *lastUnderscore = strrchr(entry->d_name, '_');
        if (lastUnderscore == NULL) continue;
        int num = -1;
        sscanf(lastUnderscore + 1, "%d.raw", &num);
        if (num > maxFileNumber) maxFileNumber = num;
    }
    nextFileNumber = maxFileNumber + 1;
    rewinddir(dir);

    int count = 0;
    bool dirEmpty = false;
    char fullPath[280]; // Sized for the theoretical maximum path (SD_MOUNT + "/" + NAME_MAX + null)

    // Second pass: load .raw files into filenameTable until it's full or runs out of files
    while (count < FILENAME_TABLE_SIZE) {
        entry = readdir(dir);
        if (entry == NULL) { dirEmpty = true; break; }

        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".raw") != 0) continue;

        // Construct the full path and add to filenameTable
        int pathLen = snprintf(fullPath, sizeof(fullPath), "%s/%s", SD_MOUNT, entry->d_name);
        if (pathLen + 1 > (int)sizeof(filenameTable[0])) {
            printf("QueueExistingFiles: skipping %s, path too long\n", entry->d_name);
            continue;
        }

        memcpy(filenameTable[count], fullPath, pathLen + 1);
        count++;
    }

    //If empty close directory immediately. Otherwise keep it open for TryRefillQueue to load more files as space frees up in filenameTable
    if (dirEmpty) {
        closedir(dir);
        printf("QueueExistingFiles: loaded %d file(s)\n", count);
    } else {
        existingFilesDir = dir; // Keep open — TryRefillQueue will load the rest
        printf("QueueExistingFiles: loaded %d file(s), more files pending\n", count);
    }

    writeIndex = count;
}

// Load more existing files from the previous session into filenameTable as upload slots free up.
void TryRefillQueue(int uploadIndex) {
    if (existingFilesDir == NULL) return;

    xSemaphoreTake(writeIndexMutex, portMAX_DELAY);
    char fullPath[280];
    struct dirent *entry;
    while (writeIndex - uploadIndex < FILENAME_TABLE_SIZE) {
        // Find the next .raw file in the directory
        bool found = false;
        while ((entry = readdir(existingFilesDir)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len < 4 || strcmp(entry->d_name + len - 4, ".raw") != 0) continue;
            found = true;
            break;
        }

        // No more files to load
        if (!found) {
            closedir(existingFilesDir);
            existingFilesDir = NULL;
            printf("TryRefillQueue: all existing files queued\n");
            break;
        }

        // Construct the full path and add to filenameTable
        int pathLen = snprintf(fullPath, sizeof(fullPath), "%s/%s", SD_MOUNT, entry->d_name);
        if (pathLen + 1 > (int)sizeof(filenameTable[0])) {
            printf("TryRefillQueue: skipping %s, path too long\n", entry->d_name);
            continue;
        }

        int idx = writeIndex;
        memcpy(filenameTable[idx % FILENAME_TABLE_SIZE], fullPath, pathLen + 1);
        writeIndex = idx + 1;
    }

    xSemaphoreGive(writeIndexMutex);
}

// Task to read data from the audio queue and write it to the SD card as a buffer
void WriteToSD(void *pvParameters)
{
    // Subscribe this task to the watchdog
    esp_task_wdt_add(NULL);

    int fileCount = nextFileNumber; //Start after the highest numbered file found by QueueExistingFiles
    char fileName[64];  //Buffer to hold the generated file name
    char timestamp[16]; //Buffer to hold the timestamp for the file name ("YYYYMMDD_HHMMSS\0")
    int16_t *received;  //Pointer to hold the received audio buffer from the queue

    for (;;)
    {
        // Stop writing permanently once LED tamper is detected
        if (atomic_load(&tamperDetected)) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        //Generate a unique file name using timestamp
        GetTimestamp(timestamp, sizeof(timestamp));
        snprintf(fileName, sizeof(fileName), "/sdcard/audio_%s_%04d.raw", timestamp, fileCount);

        //Attempt to open the file for writing. If it fails, print an error and retry after a delay
        FILE *f = fopen(fileName, "wb");
        if (f == NULL)
        {
            if (errno == ENOSPC) {
                // if SD card is full, pause recording and flash orange until StreamToServer frees space
                printf("SD card full, pausing recording until space is available...\n");
                recordingActive = false;
                vTaskSuspend(audioTaskHandle);

                // Flush stale buffers from the queue
                int16_t* stale;
                while (xQueueReceive(audioQueue, &stale, 0) == pdTRUE) {
                    free(stale);
                }

                // Flash orange until fopen succeeds
                while (f == NULL) {
                    SetLED(255, 80, 0); // Orange
                    vTaskDelay(pdMS_TO_TICKS(500));
                    SetLED(0, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_task_wdt_reset(); // update watchdog while waiting for space to free up
                    f = fopen(fileName, "wb");
                }

                // Space freed, resume recording
                printf("SD card space available, resuming recording...\n");
                recordingActive = true;
                vTaskResume(audioTaskHandle);
                SetLED(255, 0, 0);
            } else {
                printf("Failed to open file for writing: %s\n", fileName);
                vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retrying
                esp_task_wdt_reset(); // update watchdog during retry delay
                continue;
            }
        }

        //Write audio data from the queue to the file in chunks
        bool aborted = false;
        int chunksWritten = 0;
        while (chunksWritten < 500)
        { // Write 500 chunks of audio data

            // Check if recording has been stopped mid-file
            if (!recordingActive) {
                fclose(f);
                remove(fileName);
                printf("Recording stopped, deleted in-progress file %s\n", fileName);
                // Wait until recording resumes before starting a new file
                while (!recordingActive) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_task_wdt_reset(); // update watchdog while paused
                }
                aborted = true;
                break;
            }

            // Timeout so the recordingActive check above isn't blocked
            if (xQueueReceive(audioQueue, &received, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                fwrite(received, sizeof(int16_t), 1023, f);
                free(received);
                chunksWritten++;
            }

            esp_task_wdt_reset(); // update the watchdog
        }

        if (aborted) continue; // Start a new file

        fclose(f);
        xSemaphoreTake(writeIndexMutex, portMAX_DELAY);
        strncpy(filenameTable[fileCount % FILENAME_TABLE_SIZE], fileName, sizeof(filenameTable[0])); // Store before incrementing writeIndex so StreamToServer doesn't race to read an unset entry
        fileCount++;
        writeIndex = fileCount;
        xSemaphoreGive(writeIndexMutex);
        printf("Saved %s\n", fileName);
    }
}
