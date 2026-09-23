#ifndef NEBULAEDGE_MICROSD
#define NEBULAEDGE_MICROSD

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

/* Tarjeta microSD por SPI.
 *
 * PORTABLE A PROPÓSITO
 *     No conoce el pinout de ninguna placa: la aplicación le pasa los cuatro
 *     pines al montar, igual que con nebulaedge_i2c. Antes este .c se definía
 *     los suyos, con los valores de im-v1, distintos de los que documentaba
 *     nebulaedge_defs.h; eran dos copias que nadie mantenía sincronizadas. */
typedef struct {
    int cs_io;              // chip select
    int mosi_io;            // master out, slave in
    int clk_io;             // reloj
    int miso_io;            // master in, slave out
    bool format_if_mount_failed;  // formatear la tarjeta si no se puede montar
} sd_pins_t;

esp_err_t sd_mount(const sd_pins_t *pins);
esp_err_t sd_format(void);
esp_err_t sd_format_if_no_space(void);
esp_err_t sd_unmount(void);
bool sd_is_mounted(void);

#endif