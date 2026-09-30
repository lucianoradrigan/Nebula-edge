/* Escaneo de buses para bring-up. Ver nebulaedge_busscan.h para el motivo y
 * para la advertencia del relé en cs_mask. */

#include "nebulaedge_busscan.h"
#include "nebulaedge_spi.h"
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

esp_err_t nebulaedge_busscan_spi(i2c_master_bus_handle_t bus, uint8_t cs_mask) {
    if (!bus) {
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

    /* CS = -1: dispositivo SIN chip select automático. Acá lo maneja el
     * expansor a mano, que es justamente el punto del escaneo. Antes esto usaba
     * spi_bus_add_max6675_device_ext_cs(), que ya no existe en el componente. */
    if (spi_bus_init() != ESP_OK ||
        spi_bus_add_device_direct_cs(-1, SPI_PROBE_MODE, SPI_PROBE_HZ, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "  no se pudo preparar el bus SPI");
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
        spi_bus_remove_device(dev);
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

    spi_bus_remove_device(dev);
    fxl6408_del(expander);
    ESP_LOGW(TAG, "===== %d CS con respuesta distinta a la base =====", found);

    return ESP_OK;
}
