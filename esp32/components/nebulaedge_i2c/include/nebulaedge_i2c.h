#ifndef NEBULAEDGE_I2C
#define NEBULAEDGE_I2C

#include "driver/i2c_master.h"
#include "freertos/semphr.h"

extern i2c_master_bus_handle_t bus_handle;
extern i2c_master_dev_handle_t device_bmm350;
extern i2c_master_dev_handle_t device_bmi270;
extern i2c_master_dev_handle_t device_bme688;
extern SemaphoreHandle_t i2c_bus_mutex;

esp_err_t force_sda_low(void);
esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle);
esp_err_t i2c_master_deinit(i2c_master_bus_handle_t *bus_handle);
esp_err_t i2c_slave_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *device, int slave_addr, int i2c_master_freq);
esp_err_t i2c_slave_deinit(i2c_master_dev_handle_t *device);
esp_err_t device_read(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_rd, size_t size, const char *tag);
esp_err_t device_write(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_wr, size_t size, const char *tag);

#endif