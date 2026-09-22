#ifndef NEBULAEDGE_I2C
#define NEBULAEDGE_I2C

#include <stdint.h>
#include "driver/i2c_master.h"
/* FreeRTOS.h ANTES que semphr.h: semphr.h aborta con un #error si no está.
 * Este header lo incluye él mismo para ser autosuficiente, y no depender de
 * que el .c que lo use lo haya incluido antes. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Bus I2C compartido por los sensores.
 *
 * PORTABLE A PROPÓSITO
 *     No conoce el pinout de ninguna placa ni los sensores concretos que
 *     cuelgan del bus: la aplicación le pasa los pines al inicializar y cada
 *     driver se queda con su propio handle de slave. Copiar esta carpeta a
 *     otro proyecto no arrastra nada de NebulaEdge. */

/* Pinout y velocidad del bus. Los provee la aplicación, que es la única que
 * sabe en qué placa corre. */
typedef struct {
    int      scl_io;        // GPIO del reloj
    int      sda_io;        // GPIO de datos
    uint32_t freq_hz;       // velocidad por defecto de los slaves de este bus
} i2c_bus_config_t;

/* El driver i2c_master de ESP-IDF ya serializa cada transacción por su cuenta,
 * así que este mutex NO está para eso. Lo que agrega es atomicidad de la
 * secuencia completa: `i2c_device_read` son dos transacciones (escribir la
 * dirección del registro, después leer), y sin el mutex otra task podría
 * colarse entre las dos si ambas hablan con el MISMO device.
 *
 * Nadie tiene que tomarlo a mano: `i2c_device_read`/`i2c_device_write` ya lo hacen por
 * dentro. Se expone por compatibilidad; no lo uses desde afuera para congelar
 * tasks — suspender una task desde otra es justamente lo que hay que evitar. */
extern SemaphoreHandle_t i2c_bus_mutex;

esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus, const i2c_bus_config_t *config);
esp_err_t i2c_master_deinit(i2c_master_bus_handle_t *bus);

/* Agrega un slave al bus. `freq_hz` en 0 usa la velocidad por defecto que se
 * pasó en `i2c_master_init`, para que un driver no tenga que conocerla. */
esp_err_t i2c_slave_init(i2c_master_bus_handle_t *bus, i2c_master_dev_handle_t *device,
                         int slave_addr, uint32_t freq_hz);
esp_err_t i2c_slave_deinit(i2c_master_dev_handle_t *device);

esp_err_t i2c_device_read(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_rd, size_t size, const char *tag);
esp_err_t i2c_device_write(i2c_master_dev_handle_t device, uint8_t *data_address, uint8_t *data_wr, size_t size, const char *tag);

/* Fuerza SDA en LOW con GPIO open-drain, para destrabar un bus colgado.
 * Hay que liberar el driver I2C antes de llamarla. */
esp_err_t i2c_force_sda_low(int sda_io);

#endif
