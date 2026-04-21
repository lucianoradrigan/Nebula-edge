#include <string.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2c_master.h"
#include "nebulaedge_defs.h"
#include "esp_log.h"
#include "esp_task.h"
#include "freertos/semphr.h"

i2c_master_bus_handle_t bus_handle;
i2c_master_dev_handle_t device_bmm350;
i2c_master_dev_handle_t device_bmi270;
i2c_master_dev_handle_t device_bme688;

/* Aclaración: las funciones i2c de por sí son thread safe. Se implementa mutex para que cuando
 * se suspenda una task desde afuera de sí misma no esté realizando ninguna operación i2c. */
SemaphoreHandle_t i2c_bus_mutex;

static esp_err_t i2c_mutex_init_if_needed(void) {
    if (i2c_bus_mutex == NULL) {
        i2c_bus_mutex = xSemaphoreCreateMutex();
        if (i2c_bus_mutex == NULL) {
            ESP_LOGE("nebulaedge_i2c", "No se pudo crear mutex I2C");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t i2c_mutex_take(TickType_t timeout_ticks) {
    esp_err_t ret = i2c_mutex_init_if_needed();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(i2c_bus_mutex, timeout_ticks) != pdTRUE) {
        ESP_LOGW("nebulaedge_i2c", "Timeout tomando mutex I2C");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void i2c_mutex_give(void) {
    if (i2c_bus_mutex != NULL) {
        xSemaphoreGive(i2c_bus_mutex);
    }
}

/* Inicializa master I2C. */
esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle) {
    esp_err_t ret = i2c_mutex_take(pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        return ret;
    }

    // Master initialization
    i2c_master_bus_config_t i2c_mst_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ret = i2c_new_master_bus(&i2c_mst_config, bus_handle);
    vTaskDelay(50 /portTICK_PERIOD_MS);
    i2c_mutex_give();

    return ret;
}

/* Inicializa slave I2C*/
esp_err_t i2c_slave_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *device, int slave_addr, int i2c_master_freq) {
    esp_err_t ret = i2c_mutex_take(pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        return ret;
    }

    if (bus_handle == NULL || *bus_handle == NULL || device == NULL) {
        ESP_LOGE("nebulaedge_i2c", "Parámetros inválidos en i2c_slave_init");
        i2c_mutex_give();
        return ESP_ERR_INVALID_ARG;
    }

    // Slave initialization
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = slave_addr,
        .scl_speed_hz = i2c_master_freq,
    };

    ret = i2c_master_bus_add_device(*bus_handle, &dev_cfg, device);
    vTaskDelay(50 /portTICK_PERIOD_MS);
    i2c_mutex_give();

    if (ret != ESP_OK) {
        ESP_LOGE("nebulaedge_i2c", "No se pudo añadir slave I2C 0x%02X: %s", slave_addr, esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

/* Deinicializa un slave I2C genérico. */
esp_err_t i2c_slave_deinit(i2c_master_dev_handle_t *device) {
    esp_err_t lock_ret = i2c_mutex_take(pdMS_TO_TICKS(2000));
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    if (device == NULL || *device == NULL) {
        ESP_LOGI("nebulaedge_i2c", "Slave I2C ya estaba deinicializado");
        i2c_mutex_give();
        return ESP_OK;
    }

    ESP_LOGI("nebulaedge_i2c", "deinicializando DEVICE");
    esp_err_t ret = i2c_master_bus_rm_device(*device);
    if (ret != ESP_OK) {
        ESP_LOGW("nebulaedge_i2c", "No se pudo liberar el slave I2C: %s", esp_err_to_name(ret));
        i2c_mutex_give();
        return ret;
    }

    *device = NULL;
    ESP_LOGI("nebulaedge_i2c", "Slave I2C deinicializado correctamente");
    i2c_mutex_give();
    return ESP_OK;
}

/* Deinicializa un bus I2C genérico. */
esp_err_t i2c_master_deinit(i2c_master_bus_handle_t *bus_handle) {
    esp_err_t lock_ret = i2c_mutex_take(pdMS_TO_TICKS(2000));
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    if (bus_handle == NULL || *bus_handle == NULL) {
        ESP_LOGI("nebulaedge_i2c", "Bus I2C ya estaba deinicializado");
        device_bmm350 = NULL;
        device_bme688 = NULL;
        device_bmi270 = NULL;
        i2c_mutex_give();
        return ESP_OK;
    }

    esp_err_t ret = i2c_del_master_bus(*bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW("nebulaedge_i2c", "No se pudo liberar el bus I2C: %s", esp_err_to_name(ret));
        i2c_mutex_give();
        return ret;
    }

    *bus_handle = NULL;
    device_bmm350 = NULL;
    device_bme688 = NULL;
    device_bmi270 = NULL;

    ESP_LOGI("nebulaedge_i2c", "Bus I2C deinicializado correctamente");
    i2c_mutex_give();
    return ESP_OK;
}

esp_err_t force_sda_low(void) {
    // Tras liberar el driver I2C, forzar SDA en LOW con GPIO open-drain.
    gpio_reset_pin((gpio_num_t)I2C_MASTER_SDA_IO);
    gpio_set_direction((gpio_num_t)I2C_MASTER_SDA_IO, GPIO_MODE_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)I2C_MASTER_SDA_IO, GPIO_FLOATING);
    gpio_set_level((gpio_num_t)I2C_MASTER_SDA_IO, 0);
    return ESP_OK;
}


/* Lee un registro en un device cualquiera vía I2C. Análogo a funciones bmm_read 
 * y bmi_read, pero para cualquier sensor. Device representa al sensor. */
esp_err_t device_read(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_rd, size_t size, const char *tag) {
    if (device == NULL || data_address == NULL || data_rd == NULL) {
        ESP_LOGE(tag, "device_read con parámetros inválidos");
        return ESP_ERR_INVALID_ARG;
    }

    if (size == 0) {
        return ESP_OK;
    }
    esp_err_t ret = i2c_mutex_take(pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        return ret;
    }

    // Perform I2C read operation
    ret = i2c_master_transmit(device, data_address, 1, pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Error al enviar dirección para lectura: %s", esp_err_to_name(ret));
        i2c_mutex_give();
        return ret;
    }

    ret = i2c_master_receive(device, data_rd, size, pdMS_TO_TICKS(2000));

    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Error al recibir datos: %s", esp_err_to_name(ret));
    }

    i2c_mutex_give();

    return ret;
}

/* Escribe sobre un registro en un device cualquiera vía I2C. Análogo a funciones bmm_read 
 * y bmi_read, pero para cualquier sensor. Device representa al sensor. */
esp_err_t device_write(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_wr, size_t size, const char *tag) {
    if (device == NULL || data_address == NULL || data_wr == NULL) {
        ESP_LOGE(tag, "device_write con parámetros inválidos");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_mutex_take(pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        return ret;
    }

    // Perform I2C write operation
    uint8_t full_data[1 + size];
    full_data[0] = *data_address;
    memcpy(&full_data[1], data_wr, size);

    ret = i2c_master_transmit(device, full_data, sizeof(full_data), pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Error al escribir datos: %s", esp_err_to_name(ret));
    }

    i2c_mutex_give();

    return ret;
}