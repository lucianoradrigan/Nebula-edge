#include "nebulaedge_spi.h"
#include "esp_log.h"

static const char *TAG = "nebulaedge_spi";

static bool is_spi_bus_inited = false;

esp_err_t spi_bus_init(void) {
    esp_err_t ret;

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2, // For MAX6675
    };

    if (!is_spi_bus_inited) {
        ret = spi_bus_initialize(SPI_HOST_USED, &buscfg, SPI_DMA_DISABLED);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
            return ret;
        }
        is_spi_bus_inited = true;
    }
    return ESP_OK;
}

esp_err_t spi_bus_add_max6675_device_ext_cs(fxl6408_handle_t fxl_dev, uint8_t cs_pin_ext, spi_device_handle_t *spi_dev)
{
    if (!fxl_dev || !spi_dev) return ESP_ERR_INVALID_ARG;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = -1, // Manual CS
        .queue_size = 1,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags = 0,
    };
    return spi_bus_add_device(SPI_HOST_USED, &devcfg, spi_dev);
}

esp_err_t spi_bus_add_device_direct_cs(int cs_gpio_num, int spi_mode, int clock_speed_hz, spi_device_handle_t *spi_dev)
{
    if (!spi_dev) return ESP_ERR_INVALID_ARG;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = clock_speed_hz,
        .mode = spi_mode,
        .spics_io_num = cs_gpio_num, // Direct GPIO CS
        .queue_size = 7,
    };
    return spi_bus_add_device(SPI_HOST_USED, &devcfg, spi_dev);
}
