/* Componente encargado de gestión de tarjetas SD. Montar, desmontar y escritura. 
 * Script modular que debería ser fácil de testear por separado. 
 *
 * Dependencias: 
 * - esp_driver_sdspi 
 * - fatfs 
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_err.h"

#define PIN_NUM_MOSI                        GPIO_NUM_2         // GPIO pin
#define PIN_NUM_CLK                         GPIO_NUM_43        // GPIO pin
#define PIN_NUM_MISO                        GPIO_NUM_44        // GPIO pin
#define PIN_NUM_CS                          GPIO_NUM_1         // GPIO pin
#define FORMAT_IF_MOUNT_FAILED              true
#define SD_NEAR_FULL_THRESHOLD_BYTES        128 * 1024 * 1000  // 64 MB de threshold

sdmmc_card_t *card;
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
static bool s_spi_bus_inited = false;
static bool s_sd_mounted = false;
static const char *TAG = "nebulaedge_microsd";

/* Responde si la sd está montada o no. */
bool sd_is_mounted(void) {
    return s_sd_mounted;
}

/* Monta la tarjeta SD en /sdcard asignando recursos correspondientes. */
esp_err_t sd_mount(void) {
    esp_err_t ret;

    if (s_sd_mounted) {
        ESP_LOGI(TAG, "Filesystem already mounted");
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    // Inicializar el bus SPI
    if (!s_spi_bus_inited) {
        ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
            return ret;
        }
        s_spi_bus_inited = true;
    }

    // Configuración del dispositivo SPI para la tarjeta SD
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    // Opciones para el sistema de archivos
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = FORMAT_IF_MOUNT_FAILED,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        // Si falla el mount, liberamos bus para permitir retry limpio.
        spi_bus_free(host.slot);
        s_spi_bus_inited = false;
        card = NULL;
        ESP_LOGW(TAG, "Failed to mount filesystem. Error: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "SDCARD mounted succesfully on /sdcard");
    return ESP_OK;
}

/* Formatea la tarjeta SD montada en /sdcard. */
esp_err_t sd_format(void) {
    esp_err_t ret;

    if (!s_sd_mounted) {
        ret = sd_mount();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Cannot format SD: mount failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ret = esp_vfs_fat_sdcard_format("/sdcard", card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD card formatted successfully");
    return ESP_OK;
}

/* Verifica espacio libre y formatea preventivamente si queda muy poco. */
esp_err_t sd_format_if_no_space(void) {
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    esp_err_t ret = esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cannot query SD free space: %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    if (free_bytes > SD_NEAR_FULL_THRESHOLD_BYTES) {
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "SD near full: free=%u bytes (threshold=%u). Formatting card.",
             (unsigned)free_bytes,
             (unsigned)SD_NEAR_FULL_THRESHOLD_BYTES);

    ret = sd_format();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD formatted");
    return ESP_OK;
}

/* Desmonta la tarjeta SD y libera recursos asociados. */
esp_err_t sd_unmount(void) {
    esp_err_t ret = ESP_OK;

    if (!s_sd_mounted && !s_spi_bus_inited) {
        ESP_LOGI(TAG, "Filesystem already unmounted");
        return ESP_OK;
    }

    if (s_sd_mounted) {
        // Desmontar el sistema de archivos
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        s_sd_mounted = false;
        card = NULL;
        ESP_LOGI(TAG, "Filesystem unmounted");
    }

    if (s_spi_bus_inited) {
        // Liberar el bus SPI
        ret = spi_bus_free(host.slot);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to free SPI bus: %s", esp_err_to_name(ret));
            return ret;
        }
        s_spi_bus_inited = false;
    }

    return ESP_OK;
}