#ifndef BMM350
#define BMM350

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Driver del BMM350 (magnetómetro de 3 ejes).
 *
 * PORTABLE A PROPÓSITO
 *     No conoce protobuf ni el formato de cable de NebulaEdge: devuelve su
 *     propio tipo y deja que la aplicación lo traduzca. Su única dependencia
 *     del proyecto es nebulaedge_i2c. */

/* Dirección I2C del chip. Es propiedad del integrado, no de la placa. */
#define BMM350_SLAVE_ADDR 0x14

/* Largo del bloque OTP de calibración de fábrica. No confundir con el largo
 * del output de medición. */
#define BMM350_OTP_DATA_LENGTH 32

/* Output Data Rates que acepta este chip, con el prefijo del chip para que no
 * choquen con los de otro sensor si ambos drivers conviven en un proyecto. */
#define BMM350_ODR_1_5625   15625
#define BMM350_ODR_3_125    3125
#define BMM350_ODR_6_25     625
#define BMM350_ODR_12_5     125
#define BMM350_ODR_25       25
#define BMM350_ODR_50       50
#define BMM350_ODR_100      100
#define BMM350_ODR_200      200
#define BMM350_ODR_400      400

/* Una lectura del sensor, en unidades físicas. */
typedef struct {
    float mag_x_ut;         // campo magnético, microteslas
    float mag_y_ut;
    float mag_z_ut;
    float temperature_c;    // temperatura interna del chip, grados Celsius
} bmm350_reading_t;

/* Lee una medición completa.
 *
 * Retorna ESP_OK y llena `out` solo si había una muestra nueva lista. Si el
 * sensor no está inicializado retorna ESP_ERR_INVALID_STATE y si todavía no
 * hay dato nuevo ESP_ERR_NOT_FINISHED. En ninguno de esos casos toca `out`:
 * así el caller distingue un sensor caído de una medición legítima de cero. */
esp_err_t bmm350_read(bmm350_reading_t *out);

/* Agrega el sensor al bus I2C que entrega la aplicación y lo configura.
 * El driver se queda con su propio handle de slave: quien lo use no necesita
 * declarar handles de sensores ajenos. */
esp_err_t bmm350_init(i2c_master_bus_handle_t bus, int odr, int avg);

/* Saca el sensor del bus. Hay que llamarla ANTES de i2c_master_deinit(),
 * porque borrar el bus invalida los handles de sus slaves. */
esp_err_t bmm350_deinit(void);

#endif
