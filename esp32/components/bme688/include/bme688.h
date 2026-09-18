#ifndef BME688
#define BME688

#include "esp_err.h"
#include <stdint.h>
#include "driver/i2c_master.h"

/* Driver del BME688 (ambiental: temperatura, presión, humedad y gas).
 *
 * PORTABLE A PROPÓSITO
 *     No conoce protobuf ni el formato de cable de NebulaEdge: devuelve su
 *     propio tipo y deja que la aplicación lo traduzca. Su única dependencia
 *     del proyecto es nebulaedge_i2c, así que copiar esta carpeta y esa a otro
 *     proyecto alcanza para tener el sensor funcionando. */

/* Dirección I2C del chip. Es propiedad del integrado, no de la placa. */
#define BME688_SLAVE_ADDR 0x76

/* Concatena dos bytes en un uint16 (MSB primero). Lo usan las rutinas de
 * calibración, que leen los coeficientes de fábrica en pares de registros. */
#define CONCAT_BYTES(msb, lsb)  (((uint16_t)msb << 8) | (uint16_t)lsb)

/* Una lectura del sensor, en unidades físicas. */
typedef struct {
    float   temperature_c;       // grados Celsius
    int32_t pressure_pa;         // pascales
    int32_t humidity_pct;        // humedad relativa, en %
    float   gas_resistance_ohm;  // resistencia del sensor de gas, en ohms.
                                 // OJO: NO es una concentración de CO ni de
                                 // ningún gas concreto; convertirla requiere
                                 // calibración aparte.
} bme688_reading_t;

/* Lee una medición completa.
 *
 * Retorna ESP_OK y llena `out` si la lectura salió bien. Si el sensor no fue
 * inicializado correctamente retorna ESP_ERR_INVALID_STATE y NO toca `out`:
 * así el caller distingue un sensor caído de una medición legítima de cero. */
esp_err_t bme688_read(bme688_reading_t *out);

/* Agrega el sensor al bus I2C que entrega la aplicación y lo configura.
 * El driver se queda con su propio handle de slave: quien lo use no necesita
 * declarar handles de sensores ajenos. */
esp_err_t bme688_init(i2c_master_bus_handle_t bus, int temp_ovs, int press_ovs, int hum_ovs);

/* Saca el sensor del bus. Hay que llamarla ANTES de i2c_master_deinit(),
 * porque borrar el bus invalida los handles de sus slaves. */
esp_err_t bme688_deinit(void);

#endif
