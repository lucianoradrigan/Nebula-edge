#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FXL6408 I2C address when ADDR pin is connected to GND.
 */
#define FXL6408_ADDR_GND 0x43
#define FXL6408_ADDR_VCC 0x44

#define FXL6408_I2C_PORT I2C_NUM_0
#define FXL6408_I2C_SDA_IO 42
#define FXL6408_I2C_SCL_IO 2
/**
 * @brief Opaque handle for the FXL6408 device.
 */
typedef struct fxl6408_dev_t* fxl6408_handle_t;

/**
 * @brief Configuration structure for creating an FXL6408 device.
 */
typedef struct {
    i2c_master_bus_handle_t bus;      /*!< I2C master bus handle */
    uint8_t addr;                     /*!< 7-bit I2C device address */
    int rst_gpio;                     /*!< GPIO for hardware reset, or -1 if not used */
} fxl6408_cfg_t;

/**
 * @brief Create and initialize an FXL6408 device.
 *
 * @param cfg Pointer to the configuration structure.
 * @param out_handle Pointer to store the new device handle.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_create(const fxl6408_cfg_t *cfg, fxl6408_handle_t *out_handle);

/**
 * @brief Delete an FXL6408 device.
 *
 * @param handle The device handle to delete.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_del(fxl6408_handle_t handle);

/**
 * @brief Configure a pin on the FXL6408 as an output.
 * This is a helper that calls fxl6408_set_pin_direction, fxl6408_set_output_type and fxl6408_set_output.
 *
 * @param handle The device handle.
 * @param pin The pin number (0-7).
 * @param initial_high The initial output level (true for high, false for low).
 * @param high_z Whether the pin should be high-impedance when the output is high (open-drain).
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_config_output(fxl6408_handle_t handle, uint8_t pin, bool initial_high, bool high_z);

/**
 * @brief Set the direction of a pin (input or output).
 *
 * @param handle The device handle.
 * @param pin The pin number (0-7).
 * @param is_output True for output, false for input.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_set_pin_direction(fxl6408_handle_t handle, uint8_t pin, bool is_output);

/**
 * @brief Set the output type of a pin (push-pull or open-drain).
 *
 * @param handle The device handle.
 * @param pin The pin number (0-7).
 * @param high_z True for open-drain (high impedance), false for push-pull.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_set_output_type(fxl6408_handle_t handle, uint8_t pin, bool high_z);

/**
 * @brief Set the output level of a pin.
 *
 * @param handle The device handle.
 * @param pin The pin number (0-7).
 * @param level The output level (true for high, false for low).
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_set_output(fxl6408_handle_t handle, uint8_t pin, bool level);

/**
 * @brief Read the input level of a pin.
 *
 * @param handle The device handle.
 * @param pin The pin number (0-7).
 * @param level Pointer to store the input level (true for high, false for low).
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_read_input_state(fxl6408_handle_t handle, uint8_t pin, bool *level);

/**
 * @brief Read the output state register.
 *
 * @param handle The device handle.
 * @param out_state Pointer to store the output state register value.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_read_output_state(fxl6408_handle_t handle, uint8_t *out_state);

/**
 * @brief Read the direction register.
 *
 * @param handle The device handle.
 * @param out_direction Pointer to store the direction register value.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_read_direction_register(fxl6408_handle_t handle, uint8_t *out_direction);

/**
 * @brief Read the input status register.
 *
 * @param handle The device handle.
 * @param out_status Pointer to store the input status register value.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_read_input_status_register(fxl6408_handle_t handle, uint8_t *out_status);

/**
 * @brief Read the pull-enable register.
 *
 * @param handle The device handle.
 * @param out_pull_enable Pointer to store the pull-enable register value.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_read_pull_enable_register(fxl6408_handle_t handle, uint8_t *out_pull_enable);

/**
 * @brief Set the direction register.
 *
 * @param handle The device handle.
 * @param value The value to write to the direction register.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_set_direction_register(fxl6408_handle_t handle, uint8_t value);

/**
 * @brief Set the output type register.
 *
 * @param handle The device handle.
 * @param value The value to write to the output type register.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_set_output_type_register(fxl6408_handle_t handle, uint8_t value);

/**
 * @brief Set the output state register.
 *
 * @param handle The device handle.
 * @param value The value to write to the output state register.
 * @return ESP_OK on success, or an error code otherwise.
 */
esp_err_t fxl6408_set_output_state_register(fxl6408_handle_t handle, uint8_t value);

/**
 * @brief High-level initialization for the FXL6408.
 *
 * This function initializes the I2C bus, creates the FXL6408 device,
 * and configures the pins for SPI CS operation.
 *
 * @return A handle to the configured FXL6408 device, or NULL on failure.
 */
fxl6408_handle_t fxl6408_init(void);

#ifdef __cplusplus
}
#endif
