#ifndef NEBULAEDGE_SPI
#define NEBULAEDGE_SPI

#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "fxl6408.h"

// SPI Bus
#define SPI_HOST_USED  SPI3_HOST
#define PIN_NUM_MISO   47
#define PIN_NUM_MOSI   21
#define PIN_NUM_SCLK   38

esp_err_t spi_bus_init(void);
esp_err_t spi_bus_add_max6675_device_ext_cs(fxl6408_handle_t fxl_dev, uint8_t cs_pin_ext, spi_device_handle_t *spi_dev);
esp_err_t spi_bus_add_device_direct_cs(int cs_gpio_num, int spi_mode, int clock_speed_hz, spi_device_handle_t *spi_dev);


#endif // SPI_BUS_H
