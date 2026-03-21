#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "nebulaedge_defs.h"
#include "schema.pb-c.h"

sdmmc_card_t *card;
sdmmc_host_t host = SDSPI_HOST_DEFAULT();

/* Monta la tarjeta SD asignando recursos correspondientes. */
void mount_sd(void) {
    esp_err_t ret;
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    // Inicializar el bus SPI
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE("SPI", "Failed to initialize SPI bus");
        return;
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
        ESP_LOGE("SPI", "Failed to mount filesystem. Error: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI("SPI", "Filesystem mounted");
}

/* Desmonta la tarjeta SD y libera recursos asociados. */
void unmount_sd(void) {
    // Desmontar el sistema de archivos
    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    ESP_LOGI("SPI", "Filesystem unmounted");

    // Liberar el bus SPI
    spi_bus_free(host.slot);
}

/* Guarda UN paquete protobuf de medición (tipo Measure) en la tarjeta SD.
 * Esto lo hace modularmente, si hay un archivo existente escribe concatenando 
 * los bytes del paquete sobre el archivo en la SD. Esta función está diseñada 
 * para ser llamada una vez por cada ventana de medidas.
 * 
 * IMPORTANTE: los primeros 4 bytes del paquete consisten en el largo 
 * del payload, y los bytes siguientes son el payload. De esta
 * forma a la hora de desempaquetar se puede saber el fin de cada paquete.
 * 
 * La SD DEBE haber sido montada anteriormente. */
void save_measure_to_sd(Measure *m, FILE *f, char *file_path) {
    size_t packed_size = measure__get_packed_size(m);
    uint8_t buffer[packed_size];

    // Serializar el mensaje
    measure__pack(m, buffer);

    // Escribir primero el tamaño del mensaje
    fwrite(&packed_size, sizeof(packed_size), 1, f);

    // Escribir el mensaje serializado
    fwrite(buffer, 1, packed_size, f);
    
    ESP_LOGI("SPI", "Medida guardada en la SD (%d bytes)", (int)packed_size);
}

/* Función para debugging: lee el archivo en file_path
 * y desempaqueta los protobuf. Imprime medidas. La SD 
 * debe haber sido montada con anterioridad. */
void read_measures_from_sd(char *file_path) {
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        printf("Error: No se pudo abrir el archivo en la SD.\n");
        return;
    }

    while (!feof(f)) {
        size_t packed_size;
        
        // Leer el tamaño del siguiente mensaje
        if (fread(&packed_size, sizeof(packed_size), 1, f) != 1) {
            break; // Se alcanzó el final del archivo o hubo un error
        }

        uint8_t buffer[packed_size];

        // Leer el mensaje serializado
        if (fread(buffer, 1, packed_size, f) != packed_size) {
            ESP_LOGE("SPI", "Error leyendo mensaje desde la SD");
            break;
        }

        // Deserializar el mensaje
        Measure *m = measure__unpack(NULL, packed_size, buffer);
        if (m == NULL) {
            ESP_LOGE("SPI", "Error al deserializar mensaje");
            break;
        }

        // Imprimir los valores
        printf("Medida:\n");
        printf("  Gyro:  x=%f, y=%f, z=%f\n", m->gyr_x_rads, m->gyr_y_rads, m->gyr_z_rads);
        printf("  Accel: x=%f, y=%f, z=%f\n", m->acc_x_ms2, m->acc_y_ms2, m->acc_z_ms2);
        printf("  Mag:   x=%f, y=%f, z=%f\n", m->mag_x_ut, m->mag_y_ut, m->mag_z_ut);
        printf("  Temp: %f°C, Hum: %f%%, Pres: %f hPa, Q_Air: %f\n",
               m->temp_c, m->hum_percent, m->press_hpa, m->gas_res_ohms);
        printf("----------------------------------\n");

        // Liberar memoria
        measure__free_unpacked(m, NULL);
    }

    fclose(f);
}