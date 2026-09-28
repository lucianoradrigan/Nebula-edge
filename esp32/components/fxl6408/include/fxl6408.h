#ifndef FXL6408
#define FXL6408

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Driver del FXL6408 (expansor de 8 pines de propósito general por I2C).
 *
 * PORTABLE A PROPÓSITO
 *     No conoce el pinout de ninguna placa ni para qué se usa cada pin: la
 *     aplicación le pasa el bus y la dirección, y decide qué cuelga de cada
 *     IO. Su única dependencia del proyecto es nebulaedge_i2c.
 *
 * EN LA IM-V2
 *     El IO0 es el chip select de la microSD. Eso vive en nebulaedge_microsd,
 *     no acá: este driver solo sabe poner pines en alto y en bajo. */

/* Dirección I2C según cómo quede cableado el pin ADDR. Es propiedad de la
 * placa, por eso se pasa al inicializar en vez de fijarla acá. */
#define FXL6408_ADDR_GND 0x43
#define FXL6408_ADDR_VCC 0x44

/* Velocidad del slave. El datasheet del FXL6408 garantiza Fast-mode; el valor
 * de 1 MHz que traía el proyecto de bringup contradecía su propio comentario
 * ("400 kHz") y no está cubierto por la hoja de datos. */
#define FXL6408_I2C_SCL_SPEED_HZ 400000

/* Handle opaco: el driver soporta más de un expansor en el mismo bus, que es
 * la razón de que no guarde estado estático como los sensores. */
typedef struct fxl6408_dev_t* fxl6408_handle_t;

/* Datos que la aplicación tiene que entregar para crear un expansor. */
typedef struct {
    i2c_master_bus_handle_t bus;    // bus ya inicializado por i2c_master_init()
    uint8_t addr;                   // dirección de 7 bits (FXL6408_ADDR_*)
} fxl6408_cfg_t;

/* Agrega el expansor al bus I2C que entrega la aplicación y lo deja con todos
 * los pines como entrada con pull-down.
 *
 * Es la puerta de entrada normal. Hay que llamarla de nuevo cada vez que el
 * bus se rehace: destruir el bus invalida el handle del slave. */
esp_err_t fxl6408_init(i2c_master_bus_handle_t bus, uint8_t addr, fxl6408_handle_t *out_handle);

/* Crea el expansor sin tocar el estado de sus registros, para cuando la
 * aplicación quiere fijar ella misma la configuración inicial. */
esp_err_t fxl6408_create(const fxl6408_cfg_t *cfg, fxl6408_handle_t *out_handle);

/* Saca el expansor del bus y libera el handle. Hay que llamarla ANTES de
 * i2c_master_deinit(), por el mismo motivo que en los sensores. */
esp_err_t fxl6408_del(fxl6408_handle_t handle);

/* Deja un pin como salida: dirección, tipo de salida y nivel inicial de una.
 * `high_z` en true lo configura como drenador abierto. */
esp_err_t fxl6408_config_output(fxl6408_handle_t handle, uint8_t pin, bool initial_high, bool high_z);

esp_err_t fxl6408_set_pin_direction(fxl6408_handle_t handle, uint8_t pin, bool is_output);
esp_err_t fxl6408_set_output_type(fxl6408_handle_t handle, uint8_t pin, bool high_z);
esp_err_t fxl6408_set_output(fxl6408_handle_t handle, uint8_t pin, bool level);
esp_err_t fxl6408_read_input_state(fxl6408_handle_t handle, uint8_t pin, bool *level);

/* Acceso a los registros completos, para configurar los ocho pines de una sola
 * transacción en vez de ocho lecturas-modificación-escritura. */
esp_err_t fxl6408_read_output_state(fxl6408_handle_t handle, uint8_t *out_state);
esp_err_t fxl6408_read_direction_register(fxl6408_handle_t handle, uint8_t *out_direction);
esp_err_t fxl6408_read_input_status_register(fxl6408_handle_t handle, uint8_t *out_status);
esp_err_t fxl6408_read_pull_enable_register(fxl6408_handle_t handle, uint8_t *out_pull_enable);
esp_err_t fxl6408_read_pull_select_register(fxl6408_handle_t handle, uint8_t *out_pull_select);
esp_err_t fxl6408_set_direction_register(fxl6408_handle_t handle, uint8_t value);
esp_err_t fxl6408_set_output_type_register(fxl6408_handle_t handle, uint8_t value);
esp_err_t fxl6408_set_output_state_register(fxl6408_handle_t handle, uint8_t value);
esp_err_t fxl6408_set_pull_enable_register(fxl6408_handle_t handle, uint8_t value);
esp_err_t fxl6408_set_pull_select_register(fxl6408_handle_t handle, uint8_t value);

#endif
