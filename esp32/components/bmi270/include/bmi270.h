#ifndef BMI270
#define BMI270

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Driver del BMI270 (acelerómetro + giroscopio).
 *
 * PORTABLE A PROPÓSITO
 *     No conoce protobuf ni el formato de cable de NebulaEdge: devuelve su
 *     propio tipo y deja que la aplicación lo traduzca. Su única dependencia
 *     del proyecto es nebulaedge_i2c. */

/* Dirección I2C del chip. Es propiedad del integrado, no de la placa. */
#define BMI270_SLAVE_ADDR 0x68

/* Output Data Rates que acepta este chip, con el prefijo del chip para que no
 * choquen con los de otro sensor si ambos drivers conviven en un proyecto. */
#define BMI270_ODR_12_5     125
#define BMI270_ODR_25       25
#define BMI270_ODR_50       50
#define BMI270_ODR_100      100
#define BMI270_ODR_200      200
#define BMI270_ODR_400      400
#define BMI270_ODR_800      800
#define BMI270_ODR_1600     1600
#define BMI270_ODR_3200     3200

/* Una lectura del sensor, en unidades físicas. */
typedef struct {
    float acc_x_ms2;    // aceleración, m/s^2
    float acc_y_ms2;
    float acc_z_ms2;
    float gyr_x_rads;   // velocidad angular, rad/s
    float gyr_y_rads;
    float gyr_z_rads;
} bmi270_reading_t;

/* Lee una medición completa.
 *
 * Retorna ESP_OK y llena `out` solo si había una muestra nueva lista. Si el
 * sensor no está inicializado retorna ESP_ERR_INVALID_STATE, si todavía no hay
 * dato nuevo ESP_ERR_NOT_FINISHED, y si el bus falla el error del bus. En
 * ninguno de esos casos toca `out`: así el caller distingue un sensor caído de
 * una medición legítima de cero. */
esp_err_t bmi270_read(bmi270_reading_t *out);

/* Agrega el sensor al bus I2C que entrega la aplicación y lo configura.
 * El driver se queda con su propio handle de slave: quien lo use no necesita
 * declarar handles de sensores ajenos. */
esp_err_t bmi270_init(i2c_master_bus_handle_t bus, int acc_odr, int acc_avg, int acc_range, int gyr_odr, int gyr_range);

/* Saca el sensor del bus. Hay que llamarla ANTES de i2c_master_deinit(),
 * porque borrar el bus invalida los handles de sus slaves. */
esp_err_t bmi270_deinit(void);

#endif
