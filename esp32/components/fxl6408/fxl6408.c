#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "fxl6408.h"

static const char *TAG = "fxl6408";

#define FXL6408_MUTEX_TIMEOUT_MS 1000

// FXL6408 Registers
#define FXL6408_REG_DEVID         0x01
#define FXL6408_REG_DIRECTION     0x03
#define FXL6408_REG_OUTPUT_STATE  0x05
#define FXL6408_REG_OUTPUT_TYPE   0x07
#define FXL6408_REG_PULL_ENABLE   0x09
#define FXL6408_REG_PULL_SELECT   0x0B
#define FXL6408_REG_INPUT_STATUS  0x0F

struct fxl6408_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    SemaphoreHandle_t mutex;
};

// Helper to read a register
static esp_err_t fxl6408_read_reg(fxl6408_handle_t handle, uint8_t reg, uint8_t *val)
{
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(FXL6408_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, val, 1, -1);
    xSemaphoreGive(handle->mutex);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Read reg 0x%02X = 0x%02X", reg, *val);
    } else {
        ESP_LOGE(TAG, "Failed to read reg 0x%02X", reg);
    }
    return err;
}

// Helper to write a register
static esp_err_t fxl6408_write_reg(fxl6408_handle_t handle, uint8_t reg, uint8_t val)
{
    uint8_t buffer[2] = {reg, val};
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(FXL6408_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2c_master_transmit(handle->i2c_dev, buffer, sizeof(buffer), -1);
    xSemaphoreGive(handle->mutex);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Wrote reg 0x%02X = 0x%02X", reg, val);
    } else {
        ESP_LOGE(TAG, "Failed to write reg 0x%02X", reg);
    }
    return err;
}

esp_err_t fxl6408_create(const fxl6408_cfg_t *cfg, fxl6408_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Creating device at addr 0x%02X", cfg->addr);

    esp_err_t err = ESP_OK;
    fxl6408_handle_t h = calloc(1, sizeof(struct fxl6408_dev_t));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }

    h->mutex = xSemaphoreCreateMutex();
    if (h->mutex == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Adding device to I2C bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->addr,
        .scl_speed_hz = 100000, // Standard mode
    };

    err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &h->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device");
        free(h);
        return err;
    }
    ESP_LOGI(TAG, "Device added to I2C bus");

    uint8_t dev_id = 0;
    err = fxl6408_read_reg(h, FXL6408_REG_DEVID, &dev_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID");
        i2c_master_bus_rm_device(h->i2c_dev);
        free(h);
        return err;
    }

    if ((dev_id & 0xE0) != 0xA0) { // Should be 0xA0
        ESP_LOGE(TAG, "Invalid device ID: 0x%02X", dev_id);
        i2c_master_bus_rm_device(h->i2c_dev);
        free(h);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FXL6408 found at 0x%02X (rev 0x%02X)", cfg->addr, dev_id >> 4);

    *out_handle = h;
    return ESP_OK;
}

esp_err_t fxl6408_del(fxl6408_handle_t handle)
{
    if (!handle) return ESP_OK;
    ESP_LOGI(TAG, "Deleting device");
    i2c_master_bus_rm_device(handle->i2c_dev);
    vSemaphoreDelete(handle->mutex);
    free(handle);
    return ESP_OK;
}

esp_err_t fxl6408_set_pin_direction(fxl6408_handle_t handle, uint8_t pin, bool is_output)
{
    if (!handle || pin > 7) return ESP_ERR_INVALID_ARG;

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
    if (!handle || pin > 7) return ESP_ERR_INVALID_ARG;

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
    if (!handle || pin > 7) return ESP_ERR_INVALID_ARG;

    esp_err_t err = fxl6408_set_pin_direction(handle, pin, true);
    if (err != ESP_OK) return err;

    err = fxl6408_set_output_type(handle, pin, high_z);
    if (err != ESP_OK) return err;

    return fxl6408_set_output(handle, pin, initial_high);
}

esp_err_t fxl6408_set_output(fxl6408_handle_t handle, uint8_t pin, bool level)
{
    if (!handle || pin > 7) return ESP_ERR_INVALID_ARG;

    ESP_LOGD(TAG, "Setting pin %d to %d", pin, level);

    uint8_t val, read_val;
    esp_err_t err = fxl6408_read_reg(handle, FXL6408_REG_OUTPUT_STATE, &val);
    if (err != ESP_OK) return err;

    if (level) {
        val |= (1 << pin);
    } else {
        val &= ~(1 << pin);
    }

    err = fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_STATE, val);
    if (err != ESP_OK) return err;

    // Verify write
    err = fxl6408_read_reg(handle, FXL6408_REG_OUTPUT_STATE, &read_val);
    if (err != ESP_OK) return err;

    if (read_val != val) {
        ESP_LOGE(TAG, "Failed to verify output state register. Wrote 0x%02X, read 0x%02X", val, read_val);
        return ESP_FAIL;
    } else {
        ESP_LOGD(TAG, "Output state register verified.");
    }

    return ESP_OK;
}

esp_err_t fxl6408_read_input_state(fxl6408_handle_t handle, uint8_t pin, bool *level)
{
    if (!handle || pin > 7 || !level) return ESP_ERR_INVALID_ARG;

    uint8_t reg_val;
    esp_err_t err = fxl6408_read_reg(handle, FXL6408_REG_INPUT_STATUS, &reg_val);
    if (err != ESP_OK) {
        return err;
    }

    *level = (reg_val & (1 << pin)) != 0;
    ESP_LOGD(TAG, "Read input pin %d = %d", pin, *level);

    return ESP_OK;
}

esp_err_t fxl6408_read_output_state(fxl6408_handle_t handle, uint8_t *out_state)
{
    if (!handle || !out_state) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_OUTPUT_STATE, out_state);
}

esp_err_t fxl6408_read_direction_register(fxl6408_handle_t handle, uint8_t *out_direction)
{
    if (!handle || !out_direction) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_DIRECTION, out_direction);
}

esp_err_t fxl6408_read_input_status_register(fxl6408_handle_t handle, uint8_t *out_status)
{
    if (!handle || !out_status) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_INPUT_STATUS, out_status);
}

esp_err_t fxl6408_read_pull_enable_register(fxl6408_handle_t handle, uint8_t *out_pull_enable)
{
    if (!handle || !out_pull_enable) return ESP_ERR_INVALID_ARG;
    return fxl6408_read_reg(handle, FXL6408_REG_PULL_ENABLE, out_pull_enable);
}

esp_err_t fxl6408_set_direction_register(fxl6408_handle_t handle, uint8_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_DIRECTION, value);
}

esp_err_t fxl6408_set_output_type_register(fxl6408_handle_t handle, uint8_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_TYPE, value);
}

esp_err_t fxl6408_set_output_state_register(fxl6408_handle_t handle, uint8_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return fxl6408_write_reg(handle, FXL6408_REG_OUTPUT_STATE, value);
}

fxl6408_handle_t fxl6408_init(void)
{
    // Init I2C bus
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = FXL6408_I2C_PORT,
        .sda_io_num = FXL6408_I2C_SDA_IO,
        .scl_io_num = FXL6408_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // Init FXL6408 device
    fxl6408_handle_t fxl_dev = NULL;
    fxl6408_cfg_t cfg = {
        .bus      = bus,
        .addr     = FXL6408_ADDR_VCC,
        .rst_gpio = -1
    };
    ESP_ERROR_CHECK(fxl6408_create(&cfg, &fxl_dev));
    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for device to stabilize

    // Configure extender pins for SPI CS
    uint8_t direction_mask = 0b11110001; // P0, P4, P5, P6, P7 as outputs
    ESP_ERROR_CHECK(fxl6408_set_direction_register(fxl_dev, direction_mask));
    ESP_ERROR_CHECK(fxl6408_set_output_type_register(fxl_dev, 0x00)); // All push-pull
    ESP_ERROR_CHECK(fxl6408_set_output_state_register(fxl_dev, 0xFF)); // All high (inactive)

    return fxl_dev;
}