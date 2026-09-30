#ifndef NEBULAEDGE_MICROSD
#define NEBULAEDGE_MICROSD

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "fxl6408.h"

/* Tarjeta microSD por SPI.
 *
 * PORTABLE A PROPÓSITO
 *     No conoce el pinout de ninguna placa: la aplicación le pasa los pines,
 *     el periférico SPI y el reloj al montar, igual que con nebulaedge_i2c.
 *
 * EL CHIP SELECT NO ES UN GPIO
 *     En la IM-V2 cuelga del IO0 del expansor FXL6408, por I2C. Por eso acá se
 *     entrega un handle de expansor y un número de IO en vez de un pin. A SDSPI
 *     se le dice que no hay chip select y lo mueve este componente: queda en
 *     bajo mientras la tarjeta está montada. El porqué, que no es obvio, está
 *     explicado en sd_mount(). */
typedef struct {
    int      mosi_io;               // master out, slave in
    int      clk_io;                // reloj
    int      miso_io;               // master in, slave out
    spi_host_device_t spi_host;     // periférico SPI que maneja la tarjeta
    int      max_freq_khz;          // reloj de la SD; 0 usa el default de ESP-IDF (20 MHz)
    fxl6408_handle_t cs_expander;   // expansor del que cuelga el chip select
    uint8_t  cs_expander_pin;       // IO del expansor que hace de chip select
    bool     format_if_mount_failed;  // formatear la tarjeta si no se puede montar
} sd_pins_t;

esp_err_t sd_mount(const sd_pins_t *pins);
esp_err_t sd_format(void);
esp_err_t sd_format_if_no_space(void);
esp_err_t sd_unmount(void);
bool sd_is_mounted(void);

/* Prueba de banco: monta, escribe un archivo, lo relee, verifica el espacio
 * libre y desmonta, logueando cada paso. Viene del proyecto de bringup de la
 * IM-V2 y sirve para validar la tarjeta sin levantar el resto del firmware. */
void sd_selftest(const sd_pins_t *pins);

#endif
