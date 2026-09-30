#ifndef NEBULAEDGE_BUSSCAN
#define NEBULAEDGE_BUSSCAN

/* Escaneo de buses para bring-up de placa. CÓDIGO DE DIAGNÓSTICO.
 *
 * Existe para responder una sola pregunta: ¿qué hay realmente conectado? Un
 * driver que falla solo puede decir "nadie contesta en mi dirección", y eso no
 * distingue "el chip está en otra dirección" de "el chip no está montado". Con
 * esto se distingue: en la im-v2 azul el escaneo I2C devolvió 0x14, 0x44 y
 * 0x76, ni 0x68/0x69 (BMI270) ni 0x18/0x19 (BMA530), y ahí se cerró el
 * diagnóstico de que la placa no trae acelerómetro.
 *
 * Es un componente aparte y no código en main.c justamente porque es temporal:
 * cuando el bring-up se cierre, se borra la carpeta y se quitan las dos
 * llamadas. Nada más depende de esto.
 *
 * Las dos funciones son GENÉRICAS a propósito: no traen tabla de nombres de
 * dispositivos. La placa va a seguir cambiando y una tabla fija se
 * desactualiza y miente.
 */

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Barre las 112 direcciones válidas de I2C e informa cuáles responden.
 *
 * Es seguro: i2c_master_probe() manda solo la dirección y espera el ACK, sin
 * escribir ningún registro, así que no puede dejar a un dispositivo en mal
 * estado. Lo que sí cuesta es tiempo: 112 direcciones por el timeout de cada
 * una, unos pocos segundos con el bus vacío. */
esp_err_t nebulaedge_busscan_i2c(i2c_master_bus_handle_t bus);

/* Escaneo del bus SPI a través de los chip select del expansor FXL6408.
 *
 * En SPI no existe el descubrimiento: no hay direcciones ni ACK, y sin chip
 * select nadie contesta. Lo que se puede hacer es medir una línea base con
 * todos los CS en alto y después bajar uno por uno: si MISO cambia respecto de
 * la base, hay algo escuchando ahí. Es un INDICIO, no una prueba.
 *
 * `cs_mask` dice qué IO del expansor probar, un bit por IO. Es parámetro y no
 * constante porque es lo único que depende de la placa, y equivocarse tiene
 * consecuencias físicas: bajar un IO que no sea un chip select acciona lo que
 * tenga conectado, y en la im-v2 el IO1 va a un RELÉ. Por eso no hay valor por
 * defecto: el caller tiene que decir qué está dispuesto a mover. */
esp_err_t nebulaedge_busscan_spi(i2c_master_bus_handle_t bus, uint8_t cs_mask);

#endif // NEBULAEDGE_BUSSCAN
