#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nebulaedge_i2c.h"
#include "fxl6408.h"

static const char *TAG = "fxl6408";

// Registros del FXL6408
#define FXL6408_REG_DEVID         0x01
#define FXL6408_REG_DIRECTION     0x03
#define FXL6408_REG_OUTPUT_STATE  0x05
#define FXL6408_REG_OUTPUT_TYPE   0x07
#define FXL6408_REG_PULL_ENABLE   0x09
#define FXL6408_REG_PULL_SELECT   0x0B
#define FXL6408_REG_INPUT_STATUS  0x0F

// El registro de identificación devuelve 0xA0 en sus tres bits altos; los
// cuatro altos restantes son la revisión del silicio.
#define FXL6408_DEVID_MASK        0xE0
#define FXL6408_DEVID_EXPECTED    0xA0

struct fxl6408_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t output_state;           // espejo del registro, ver fxl6408_set_output()
};

/* Lectura y escritura de un registro.
 *
 * Van por i2c_device_read/i2c_device_write en vez de hablarle al driver de
 * ESP-IDF directo: esas dos toman i2c_bus_mutex, que es lo que impide que un
 * sensor se cuele entre las dos transacciones de una lectura. Importa más acá
 * que en un sensor, porque en la IM-V2 el chip select de la microSD cuelga de
 * este expansor: la escritura sale desde el contexto del driver SPI, en
 * cualquier momento, y sin el mutex partiría la lectura de otro driver. */
static esp_err_t fxl6408_read_reg(fxl6408_handle_t handle, uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_device_read(handle->i2c_dev, &reg, val, 1, TAG);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Registro 0x%02X leído = 0x%02X", reg, *val);
    }
    return err;
}

static esp_err_t fxl6408_write_reg(fxl6408_handle_t handle, uint8_t reg, uint8_t val)
{
    esp_err_t err = i2c_device_write(handle->i2c_dev, &reg, &val, 1, TAG);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Registro 0x%02X escrito = 0x%02X", reg, val);
    }
    return err;
}

esp_err_t fxl6408_create(const fxl6408_cfg_t *cfg, fxl6408_handle_t *out_handle)
{
    if (cfg == NULL || cfg->bus == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fxl6408_handle_t handle = calloc(1, sizeof(struct fxl6408_dev_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    handle->output_state = 0xFF;    // todos en alto, que es el estado inactivo

    i2c_master_bus_handle_t bus = cfg->bus;
    esp_err_t err = i2c_slave_init(&bus, &handle->i2c_dev, cfg->addr, FXL6408_I2C_SCL_SPEED_HZ);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    uint8_t dev_id = 0;
    err = fxl6408_read_reg(handle, FXL6408_REG_DEVID, &dev_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer el ID del expansor en 0x%02X: %s", cfg->addr, esp_err_to_name(err));
        fxl6408_del(handle);
        return err;
    }

    if ((dev_id & FXL6408_DEVID_MASK) != FXL6408_DEVID_EXPECTED) {
        ESP_LOGE(TAG, "ID inesperado en 0x%02X: 0x%02X", cfg->addr, dev_id);
        fxl6408_del(handle);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FXL6408 encontrado en 0x%02X (revisión 0x%02X)", cfg->addr, dev_id >> 4);
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t fxl6408_del(fxl6408_handle_t handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }
    i2c_slave_deinit(&handle->i2c_dev);
    free(handle);
    return ESP_OK;
}

esp_err_t fxl6408_init(i2c_master_bus_handle_t bus, uint8_t addr, fxl6408_handle_t *out_handle)
{
    fxl6408_cfg_t cfg = {
        .bus  = bus,
        .addr = addr,
    };

    fxl6408_handle_t handle = NULL;
    esp_err_t err = fxl6408_create(&cfg, &handle);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // el chip necesita estabilizarse antes de configurarlo

    /* Estado conocido de partida: los ocho pines como entrada con pull-down.
     * Quien vaya a usar uno como salida lo pide después con
     * fxl6408_config_output(), que es lo que hace nebulaedge_microsd con el
     * IO0 del chip select. */
    err = fxl6408_set_direction_register(handle, 0x00);
    if (err == ESP_OK) {
        err = fxl6408_set_pull_enable_register(handle, 0xFF);
    }
    if (err == ESP_OK) {
        err = fxl6408_set_pull_select_register(handle, 0x00);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo dejar el expansor en su estado inicial: %s", esp_err_to_name(err));
        fxl6408_del(handle);
        return err;
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t fxl6408_set_pin_direction(fxl6408_handle_t handle, uint8_t pin, bool is_output)
{
    if (handle == NULL || pin > 7) return ESP_ERR_INVALID_ARG;

    uint8_t val;
    esp_err_t err = fxl6408_read_reg(handle, FXL6408_REG_DIRECTION, &val);
    if (err != ESP_OK) return err;

    if (is_output) {
        val |= (1 << pin);
    } else {
        val &= ~(1 << pin);
    }

    return fxl6408_write_reg(handle, FXL6408_REG_DIRECTION, val);
}

esp_err_t fxl6408_set_output_type(fxl6408_handle_t handle, uint8_t pin, bool high_z)
{
    if (handle == NULL || pin > 7) return ESP_ERR_INVALID_ARG;

    uint8_t val;
    esp_err_t err = fxl6408_read_reg(handle, FXL6408_REG_OUTPUT_TYPE, &val);
    if (err != ESP_OK) return err;

    if (high_z) {
        val |= (1 << pin);
    } else {
        val &= ~(1 << pin);
    }

    return fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_TYPE, val);
}

esp_err_t fxl6408_config_output(fxl6408_handle_t handle, uint8_t pin, bool initial_high, bool high_z)
{
    if (handle == NULL || pin > 7) return ESP_ERR_INVALID_ARG;

    esp_err_t err = fxl6408_set_pin_direction(handle, pin, true);
    if (err != ESP_OK) return err;

    err = fxl6408_set_output_type(handle, pin, high_z);
    if (err != ESP_OK) return err;

    return fxl6408_set_output(handle, pin, initial_high);
}

/* El FXL6408 no deja escribir un pin suelto: el registro de salida se escribe
 * entero. Por eso el driver mantiene el espejo `output_state`, en vez de leer
 * el registro antes de cada escritura — con el chip select de la microSD
 * encima eso serían dos transacciones I2C por cada toggle en vez de una. */
esp_err_t fxl6408_set_output(fxl6408_handle_t handle, uint8_t pin, bool level)
{
    if (handle == NULL || pin > 7) return ESP_ERR_INVALID_ARG;

    if (level) {
        handle->output_state |= (1 << pin);
    } else {
        handle->output_state &= ~(1 << pin);
    }

    return fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_STATE, handle->output_state);
}

esp_err_t fxl6408_read_input_state(fxl6408_handle_t handle, uint8_t pin, bool *level)
{
    if (handle == NULL || pin > 7 || level == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t reg_val;
    esp_err_t err = fxl6408_read_reg(handle, FXL6408_REG_INPUT_STATUS, &reg_val);
    if (err != ESP_OK) return err;

    *level = (reg_val & (1 << pin)) != 0;
    return ESP_OK;
}

esp_err_t fxl6408_read_output_state(fxl6408_handle_t handle, uint8_t *out_state)
{
    if (handle == NULL || out_state == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_OUTPUT_STATE, out_state);
}

esp_err_t fxl6408_read_direction_register(fxl6408_handle_t handle, uint8_t *out_direction)
{
    if (handle == NULL || out_direction == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_DIRECTION, out_direction);
}

esp_err_t fxl6408_read_input_status_register(fxl6408_handle_t handle, uint8_t *out_status)
{
    if (handle == NULL || out_status == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_INPUT_STATUS, out_status);
}

esp_err_t fxl6408_read_pull_enable_register(fxl6408_handle_t handle, uint8_t *out_pull_enable)
{
    if (handle == NULL || out_pull_enable == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_PULL_ENABLE, out_pull_enable);
}

esp_err_t fxl6408_read_pull_select_register(fxl6408_handle_t handle, uint8_t *out_pull_select)
{
    if (handle == NULL || out_pull_select == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_PULL_SELECT, out_pull_select);
}

esp_err_t fxl6408_set_direction_register(fxl6408_handle_t handle, uint8_t value)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_DIRECTION, value);
}

esp_err_t fxl6408_set_output_type_register(fxl6408_handle_t handle, uint8_t value)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_TYPE, value);
}

esp_err_t fxl6408_set_output_state_register(fxl6408_handle_t handle, uint8_t value)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    handle->output_state = value;
    return fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_STATE, value);
}

esp_err_t fxl6408_set_pull_enable_register(fxl6408_handle_t handle, uint8_t value)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_PULL_ENABLE, value);
}

esp_err_t fxl6408_set_pull_select_register(fxl6408_handle_t handle, uint8_t value)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_PULL_SELECT, value);
}
