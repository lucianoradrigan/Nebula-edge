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

/* El expansor que maneja el chip select. Lo movemos nosotros, no SDSPI; el
 * porqué está explicado largo en sd_mount(). */
static fxl6408_handle_t s_cs_expander = NULL;

static const char *TAG = "nebulaedge_microsd";

/* Pone el chip select de la tarjeta en alto o en bajo. Es un IO del expansor,
 * o sea una transacción I2C: no se puede llamar desde un contexto que no pueda
 * bloquearse. Ver sd_mount(). */
static esp_err_t sd_cs_set(bool high) {
    if (s_cs_expander == NULL) {
        ESP_LOGE(TAG, "No hay expansor registrado para mover el chip select");
        return ESP_ERR_INVALID_STATE;
    }
    return fxl6408_set_output(s_cs_expander, s_pins.cs_expander_pin, high);
}

/* Reloj de arranque que pide la norma SD: al menos 74 ciclos con el chip select
 * en ALTO antes del primer comando, para que la tarjeta inicialice su
 * electrónica interna. Normalmente lo da SDSPI en go_idle_clockout(), pero eso
 * corre cuando el chip select ya es asunto nuestro, así que lo damos acá con un
 * dispositivo temporal que no maneja ningún chip select. */
static esp_err_t sd_clockout_before_select(const sd_pins_t *pins) {
    const spi_device_interface_config_t dummy_cfg = {
        .clock_speed_hz = 400000,      // frecuencia de sondeo, como la de SDSPI
        .mode            = 0,
        .spics_io_num    = -1,         // sin chip select: lo tenemos en alto
        .queue_size      = 1,
    };

    spi_device_handle_t dummy = NULL;
    esp_err_t ret = spi_bus_add_device(pins->spi_host, &dummy_cfg, &dummy);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo agregar el dispositivo temporal del reloj de arranque: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    uint8_t ones[10];               // 80 ciclos, con margen sobre los 74
    memset(ones, 0xFF, sizeof(ones));
    spi_transaction_t t = {
        .length    = sizeof(ones) * 8,
        .tx_buffer = ones,
    };
    ret = spi_device_polling_transmit(dummy, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falló el reloj de arranque de la tarjeta: %s", esp_err_to_name(ret));
    }

    spi_bus_remove_device(dummy);
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

    /* EL CHIP SELECT NO SE LE ENTREGA A SDSPI, Y ES A PROPÓSITO
     *
     * El proyecto de bringup le pasaba un pin virtual (100 + IO del expansor) y
     * lo atendía con -Wl,--wrap=gpio_set_level. El wrap funciona, pero no
     * alcanza: sdspi_host_init_device() además configura el chip select como
     * GPIO real, con
     *
     *     .pin_bit_mask = 1ULL << slot_config->gpio_cs
     *
     * y correr 100 lugares un valor de 64 bits es comportamiento indefinido. En
     * este chip queda enmascarado a 100 & 63 = 36, así que gpio_config()
     * reconfigura el GPIO 36. En la IM-V2 ese pin es una línea de datos de la
     * flash OCTAL (el arranque dice "Octal Flash Mode Enabled" y "flash io:
     * opi_str"), o sea que dejarlo como salida de propósito general rompe el bus
     * de la flash: el siguiente fetch de código no vuelve y el interrupt
     * watchdog reinicia el chip.
     *
     * Medido en banco: 22 reinicios seguidos con rst:0x8 (TG1WDT_SYS_RST), y
     * una sonda que solo llama a gpio_config() sobre el GPIO 36, sin tocar la
     * tarjeta, reproduce el reinicio por su cuenta.
     *
     * Así que se le dice a SDSPI que no hay chip select y lo movemos nosotros:
     * alto para el reloj de arranque, bajo mientras la tarjeta está montada. */
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

    /* El reloj de arranque va con el chip select en alto, que es como lo dejó
     * fxl6408_config_output() más arriba. Después queda en bajo y la tarjeta se
     * mantiene seleccionada todo el tiempo que esté montada: es el único
     * dispositivo que usamos de este bus. */
    ret = sd_clockout_before_select(pins);
    if (ret != ESP_OK) {
        spi_bus_free(host.slot);
        s_spi_bus_inited = false;
        return ret;
    }

    ret = sd_cs_set(false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo bajar el chip select de la tarjeta: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        s_spi_bus_inited = false;
        return ret;
    }

    /* Configuración del dispositivo SPI para la tarjeta SD. Sin chip select:
     * ver la explicación larga arriba. */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SDSPI_SLOT_NO_CS;
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
        sd_cs_set(true);
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
        /* Suelta la tarjeta antes de bajar el bus: el chip select quedó en bajo
         * todo el tiempo que estuvo montada. */
        sd_cs_set(true);

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
