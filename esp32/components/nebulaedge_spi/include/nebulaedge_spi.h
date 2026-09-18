#ifndef NEBULAEDGE_SPI
#define NEBULAEDGE_SPI

/* Bus SPI genérico. Hoy NADIE en el firmware lo usa: el único periférico SPI
 * de la placa es la microSD, que se monta por su propio componente
 * (nebulaedge_microsd). Se conserva porque es el punto de entrada si se
 * agrega un periférico SPI más adelante. */

#include "driver/spi_master.h"
#include "driver/i2c_master.h"

// SPI Bus
#define SPI_HOST_USED  SPI3_HOST
#define PIN_NUM_MISO   47
#define PIN_NUM_MOSI   21
#define PIN_NUM_SCLK   38

esp_err_t spi_bus_init(void);
esp_err_t spi_bus_add_device_direct_cs(int cs_gpio_num, int spi_mode, int clock_speed_hz, spi_device_handle_t *spi_dev);


#endif // SPI_BUS_H
