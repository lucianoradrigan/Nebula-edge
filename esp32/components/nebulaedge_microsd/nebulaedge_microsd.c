/* Componente encargado de gestión de tarjetas SD. Montar, desmontar y escritura. 
 * Script modular que debería ser fácil de testear por separado. 
 *
 * Dependencias: 
 * - esp_driver_sdspi 
 * - fatfs 
 * - fxl6408 (expansor de IO del que cuelga el chip select)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_err.h"

#include "nebulaedge_microsd.h"

/* El pinout lo entrega la aplicación en sd_mount(); ver el header. */
#define SD_NEAR_FULL_THRESHOLD_BYTES        (128ULL * 1024 * 1000)  // 128 MB de threshold

/* Los GPIO reales del ESP32-S3 llegan hasta el 48. De 100 para arriba son
 * pines del expansor: 100 + n es el IOn del FXL6408. La traducción la hace
 * __wrap_gpio_set_level(), más abajo. */
#define GPIO_EXTENDER_BASE                  100
#define VIRTUAL_GPIO_NUM(n)                 ((gpio_num_t)(GPIO_EXTENDER_BASE + (n)))

/* Tiempo que tarda la alimentación de la tarjeta en estabilizarse después de
 * energizarla. Medido en el banco de la IM-V2: sin esta espera el montaje
 * falla de forma intermitente. */
#define SD_POWER_SETTLE_MS                  300

static sdmmc_card_t *card = NULL;
static sdmmc_host_t host;
static bool s_spi_bus_inited = false;

/* Último pinout que entregó la aplicación en sd_mount(). Lo guardamos porque
 * sd_format() remonta por su cuenta cuando la tarjeta no está montada, y este
 * componente no tiene otra forma de saber en qué pines está la SD. `s_has_pins`
 * distingue "nunca se llamó a sd_mount" de un struct en cero, que serían pines
 * válidos (GPIO 0). */
static sd_pins_t s_pins;
static bool s_has_pins = false;
static bool s_sd_mounted = false;

/* El expansor que maneja el chip select. Está en una estática porque
 * __wrap_gpio_set_level() no recibe contexto: el driver SDSPI la llama con un
 * número de pin y nada más. */
static fxl6408_handle_t s_cs_expander = NULL;

static const char *TAG = "nebulaedge_microsd";

/* Declaración de la función real de ESP-IDF, que el wrap deja accesible. */
esp_err_t __real_gpio_set_level(gpio_num_t gpio_num, uint32_t level);

/* Intercepta gpio_set_level() para todo el binario. Ver la explicación larga en
 * el CMakeLists.txt raíz, que es donde vive la opción de enlace.
 *
 * Solo desvía los números de pin virtuales; cualquier GPIO real del chip cae en
 * __real_gpio_set_level() sin cambios, que es lo que hacen las otras llamadas
 * del proyecto y de ESP-IDF que este wrap también redirige. */
esp_err_t __attribute__((used)) __wrap_gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if ((int)gpio_num < GPIO_EXTENDER_BASE) {
        return __real_gpio_set_level(gpio_num, level);
    }

    uint8_t pin = (uint8_t)((int)gpio_num - GPIO_EXTENDER_BASE);
    if (s_cs_expander == NULL) {
        ESP_LOGE(TAG, "No se puede mover el IO%d: no hay expansor registrado", pin);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = fxl6408_set_output(s_cs_expander, pin, level != 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo poner el IO%d del expansor en %ld: %s",
                 pin, (long)level, esp_err_to_name(ret));
    }
    return ret;
}

/* Responde si la sd está montada o no. */
bool sd_is_mounted(void) {
    return s_sd_mounted;
}

/* Monta la tarjeta SD en /sdcard asignando recursos correspondientes.
 * `pins` lo entrega la aplicación: este componente no conoce la placa. */
esp_err_t sd_mount(const sd_pins_t *pins) {
    esp_err_t ret;

    if (pins == NULL || pins->cs_expander == NULL) {
        ESP_LOGE(TAG, "sd_mount sin pinout o sin expansor para el chip select");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sd_mounted) {
        ESP_LOGI(TAG, "Filesystem already mounted");
        return ESP_OK;
    }

    s_pins = *pins;
    s_has_pins = true;

    /* El expansor queda registrado ANTES de tocar SDSPI: el propio
     * sdspi_host_init_device() pone el chip select en alto al inicializar, y
     * esa llamada ya pasa por __wrap_gpio_set_level(). */
    s_cs_expander = pins->cs_expander;

    ret = fxl6408_config_output(s_cs_expander, pins->cs_expander_pin, true, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar el IO%d del expansor como chip select: %s",
                 pins->cs_expander_pin, esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(SD_POWER_SETTLE_MS));

    host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host.slot = pins->spi_host;
    if (pins->max_freq_khz > 0) {
        host.max_freq_khz = pins->max_freq_khz;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = pins->mosi_io,
        .miso_io_num = pins->miso_io,
        .sclk_io_num = pins->clk_io,
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

        /* Pull-ups internos en las tres líneas del bus. Vienen del bringup de
         * la IM-V2: la placa no trae pull-ups externos. */
        gpio_set_pull_mode(pins->miso_io, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(pins->mosi_io, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(pins->clk_io, GPIO_PULLUP_ONLY);
    }

    /* Configuración del dispositivo SPI para la tarjeta SD.
     * El chip select va como pin virtual para que lo atienda el wrap.
     *
     * PENDIENTE DE VERIFICAR EN BANCO
     *     El wrap cubre gpio_set_level(), pero no gpio_config(), y
     *     sdspi_host_init_device() configura el chip select como GPIO real
     *     antes de usarlo, con .pin_bit_mask = 1ULL << gpio_cs. Con gpio_cs a
     *     100 ese corrimiento es comportamiento indefinido; siguiendo
     *     __ashldi3 en xtensa queda enmascarado a 100 & 63 = 36, o sea que
     *     probablemente deje el GPIO 36 como salida sin que nadie se lo pida.
     *     Si la máscara diera 0, gpio_config() devolvería ESP_ERR_INVALID_ARG
     *     y el montaje fallaría acá mismo, así que el síntoma distingue los
     *     dos casos. Hay que contrastar el GPIO 36 con el esquemático. */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = VIRTUAL_GPIO_NUM(pins->cs_expander_pin);
    slot_config.host_id = host.slot;

    // Opciones para el sistema de archivos
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = pins->format_if_mount_failed,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        // Si falla el mount, liberamos bus para permitir retry limpio.
        spi_bus_free(host.slot);
        s_spi_bus_inited = false;
        card = NULL;
        ESP_LOGE(TAG, "Failed to mount filesystem on /sdcard: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "SDCARD mounted successfully on /sdcard");
    return ESP_OK;
}

/* Formatea la tarjeta SD montada en /sdcard. */
esp_err_t sd_format(void) {
    esp_err_t ret;

    if (!s_sd_mounted) {
        if (!s_has_pins) {
            ESP_LOGE(TAG, "Cannot format SD: la aplicación nunca llamó a sd_mount()");
            return ESP_ERR_INVALID_STATE;
        }
        ret = sd_mount(&s_pins);
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
             "SD near full: free=%llu bytes (threshold=%llu). Formatting card.",
             (unsigned long long)free_bytes,
             (unsigned long long)SD_NEAR_FULL_THRESHOLD_BYTES);

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

    /* El expansor lo creó la aplicación, así que no se borra acá; solo se deja
     * de atender el chip select. */
    s_cs_expander = NULL;

    return ESP_OK;
}

/* Prueba de banco. Ver el header. */
void sd_selftest(const sd_pins_t *pins)
{
    const char *test_file_path = "/sdcard/test_nebula.txt";
    const char *test_data = "Hello, Nebula MicroSD driver test!";

    ESP_LOGI(TAG, "=== Prueba de la microSD ===");

    esp_err_t ret = sd_mount(pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sd_mount() falló: %s", esp_err_to_name(ret));
        return;
    }
    if (!sd_is_mounted()) {
        ESP_LOGE(TAG, "sd_is_mounted() dice false después de un sd_mount() exitoso");
        return;
    }
    ESP_LOGI(TAG, "Tarjeta montada");

    FILE *f = fopen(test_file_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "No se pudo abrir %s para escritura", test_file_path);
    } else {
        fprintf(f, "%s\n", test_data);
        fclose(f);
        ESP_LOGI(TAG, "Archivo escrito");
    }

    f = fopen(test_file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "No se pudo abrir %s para lectura", test_file_path);
    } else {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f) != NULL) {
            buf[strcspn(buf, "\r\n")] = 0;
            if (strcmp(buf, test_data) == 0) {
                ESP_LOGI(TAG, "Lectura verificada: '%s'", buf);
            } else {
                ESP_LOGE(TAG, "Los datos no coinciden. Esperado '%s', leído '%s'", test_data, buf);
            }
        } else {
            ESP_LOGE(TAG, "No se pudo leer el archivo");
        }
        fclose(f);
    }

    ret = sd_format_if_no_space();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sd_format_if_no_space() devolvió error: %s", esp_err_to_name(ret));
    }

    ret = sd_unmount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sd_unmount() falló: %s", esp_err_to_name(ret));
    } else if (sd_is_mounted()) {
        ESP_LOGE(TAG, "sd_is_mounted() dice true después de sd_unmount()");
    } else {
        ESP_LOGI(TAG, "Tarjeta desmontada");
    }

    ESP_LOGI(TAG, "=== Fin de la prueba de la microSD ===");
}
