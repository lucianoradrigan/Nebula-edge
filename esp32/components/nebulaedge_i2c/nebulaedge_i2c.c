#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "nebulaedge_defs.h"
#include "esp_log.h"
#include "esp_task.h"

i2c_master_bus_handle_t bus_handle;
i2c_master_dev_handle_t device_bmm350;
i2c_master_dev_handle_t device_bmi270;
i2c_master_dev_handle_t device_bme688;

/* Inicializa master I2C. */
esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle) {

    // Master initialization
    i2c_master_bus_config_t i2c_mst_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, bus_handle));
    vTaskDelay(50 /portTICK_PERIOD_MS);

    return ESP_OK;
}

/* Inicializa slave I2C*/
esp_err_t i2c_slave_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *device, int slave_addr, int i2c_master_freq) {

    // Slave initialization
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = slave_addr,
        .scl_speed_hz = i2c_master_freq,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(*bus_handle, &dev_cfg, device));
    vTaskDelay(50 /portTICK_PERIOD_MS);
    
    return ESP_OK;
}

/* Lee un registro en un device cualquiera vía I2C. Análogo a funciones bmm_read 
 * y bmi_read, pero para cualquier sensor. Device representa al sensor. */
esp_err_t device_read(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_rd, size_t size, const char *tag) {
    if (size == 0) {
        return ESP_OK;
    }
    esp_err_t ret;

    // Perform I2C read operation
    ret = i2c_master_transmit(device, data_address, 1, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Error al enviar dirección para lectura: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_master_receive(device, data_rd, size, pdMS_TO_TICKS(1000));

    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Error al recibir datos: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* Escribe sobre un registro en un device cualquiera vía I2C. Análogo a funciones bmm_read 
 * y bmi_read, pero para cualquier sensor. Device representa al sensor. */
esp_err_t device_write(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_wr, size_t size, const char *tag) {
    esp_err_t ret;

    // Perform I2C write operation
    uint8_t full_data[1 + size];
    full_data[0] = *data_address;
    memcpy(&full_data[1], data_wr, size);

    ret = i2c_master_transmit(device, full_data, sizeof(full_data), pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Error al escribir datos: %s", esp_err_to_name(ret));
    }

    return ret;
}