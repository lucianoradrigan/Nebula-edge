/* Escaneo de buses para bring-up. Ver nebulaedge_busscan.h para el motivo y
 * para la advertencia del relé en cs_mask. */

#include <string.h>
#include "nebulaedge_busscan.h"
#include "driver/spi_master.h"
#include "fxl6408.h"
#include "esp_log.h"

static const char *TAG = "busscan";

/* Rango válido de direcciones I2C de 7 bits: 0x00-0x07 y 0x78-0x7F están
 * reservadas por la especificación, así que quedan 112 utilizables. */
#define I2C_ADDR_FIRST      0x08
#define I2C_ADDR_PAST_LAST  0x78
#define I2C_PROBE_TIMEOUT_MS  50

/* Parámetros de la transacción de sondeo SPI. Dos bytes alcanzan: solo
 * interesa si MISO cambia respecto de la línea base, no qué dice. */
#define SPI_PROBE_BITS      16
#define SPI_PROBE_MODE      0
#define SPI_PROBE_HZ        1000000

/* Prepara el bus y agrega un dispositivo SIN chip select automático: acá el CS
 * lo mueve el expansor a mano, que es el punto del escaneo.
 *
 * Estas dos cosas vivían en un componente aparte, nebulaedge_spi, cuyo único
 * cliente era este archivo y que traía su propia copia del pinout. Al estar
 * acá, el pinout entra por parámetro y no hay copia que desincronizar. */
static esp_err_t probe_device_open(const busscan_spi_pins_t *pins, spi_device_handle_t *out_dev) {
    const spi_bus_config_t buscfg = {
        .mosi_io_num   = pins->mosi_io,
        .miso_io_num   = pins->miso_io,
        .sclk_io_num   = pins->sclk_io,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(uint8_t) * 2,
    };

    esp_err_t ret = spi_bus_initialize(pins->host, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  no se pudo inicializar el bus SPI: %s", esp_err_to_name(ret));
        return ret;
    }

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_PROBE_HZ,
        .mode           = SPI_PROBE_MODE,
        .spics_io_num   = -1,       // sin CS automático: lo mueve el expansor
        .queue_size     = 1,
    };

    ret = spi_bus_add_device(pins->host, &devcfg, out_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  no se pudo agregar el dispositivo de sondeo: %s", esp_err_to_name(ret));
        spi_bus_free(pins->host);
    }
    return ret;
}

/* Deja el bus como estaba: el escaneo es diagnóstico de arranque y después la
 * microSD necesita inicializar el mismo host por su cuenta. */
static void probe_device_close(const busscan_spi_pins_t *pins, spi_device_handle_t dev) {
    spi_bus_remove_device(dev);
    spi_bus_free(pins->host);
}

esp_err_t nebulaedge_busscan_i2c(i2c_master_bus_handle_t bus) {
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    int found = 0;

    ESP_LOGW(TAG, "===== ESCANEO I2C =====");
    for (uint8_t addr = I2C_ADDR_FIRST; addr < I2C_ADDR_PAST_LAST; ++addr) {
        if (i2c_master_probe(bus, addr, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGW(TAG, "  0x%02X responde", addr);
            found++;
        }
    }
    ESP_LOGW(TAG, "===== %d dispositivo(s) en I2C =====", found);

    return ESP_OK;
}

esp_err_t nebulaedge_busscan_spi(i2c_master_bus_handle_t bus,
                                 const busscan_spi_pins_t *pins,
                                 uint8_t cs_mask) {
    if (!bus || !pins) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cs_mask == 0) {
        ESP_LOGW(TAG, "cs_mask en 0: no hay ningún CS que probar");
        return ESP_ERR_INVALID_ARG;
    }

    fxl6408_cfg_t cfg = { .bus = bus, .addr = FXL6408_ADDR_VCC };
    fxl6408_handle_t expander = NULL;
    spi_device_handle_t dev = NULL;
    uint8_t tx[2] = { 0x00, 0x00 };
    uint8_t base[2] = { 0 };
    esp_err_t ret;
    int found = 0;

    ESP_LOGW(TAG, "===== ESCANEO SPI (cs_mask=0x%02X) =====", cs_mask);

    if (fxl6408_create(&cfg, &expander) != ESP_OK) {
        ESP_LOGE(TAG, "  sin expansor en 0x%02X no hay chip selects", FXL6408_ADDR_VCC);
        return ESP_ERR_NOT_FOUND;
    }

    if (probe_device_open(pins, &dev) != ESP_OK) {
        fxl6408_del(expander);
        return ESP_FAIL;
    }

    // Todos los CS de la máscara arrancan en alto, o sea deseleccionados.
    for (uint8_t pin = 0; pin < 8; ++pin) {
        if (cs_mask & (1u << pin)) {
            fxl6408_config_output(expander, pin, true, false);
        }
    }

    // Línea base: qué devuelve MISO cuando nadie está seleccionado.
    spi_transaction_t probe = { .length = SPI_PROBE_BITS, .tx_buffer = tx, .rx_buffer = base };
    if (spi_device_polling_transmit(dev, &probe) != ESP_OK) {
        ESP_LOGE(TAG, "  no se pudo medir la línea base");
        probe_device_close(pins, dev);
        fxl6408_del(expander);
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "  linea base (ningun CS activo): %02X %02X", base[0], base[1]);

    for (uint8_t pin = 0; pin < 8; ++pin) {
        uint8_t rx[2] = { 0 };
        spi_transaction_t trans = { .length = SPI_PROBE_BITS, .tx_buffer = tx, .rx_buffer = rx };

        if (!(cs_mask & (1u << pin))) {
            continue;
        }

        fxl6408_set_output(expander, pin, false);   // CS bajo: selecciona
        ret = spi_device_polling_transmit(dev, &trans);
        fxl6408_set_output(expander, pin, true);    // CS alto: libera

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "  IO%u  transaccion fallo: %s", pin, esp_err_to_name(ret));
            continue;
        }

        /* Distinto de la base es el único indicio de que hay alguien. Se
         * imprimen los dos casos: un IO que siempre devuelve lo mismo que la
         * base también es información. */
        bool distinto = (rx[0] != base[0] || rx[1] != base[1]);
        ESP_LOGW(TAG, "  IO%u  %02X %02X%s", pin, rx[0], rx[1], distinto ? "  <-- distinto" : "");
        if (distinto) {
            found++;
        }
    }

    probe_device_close(pins, dev);
    fxl6408_del(expander);
    ESP_LOGW(TAG, "===== %d CS con respuesta distinta a la base =====", found);

    return ESP_OK;
}
