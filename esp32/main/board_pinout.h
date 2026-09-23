#ifndef BOARD_PINOUT
#define BOARD_PINOUT

#include "driver/gpio.h"

/* Pinout de la placa. ÚNICA fuente de verdad.
 *
 * POR QUÉ VIVE EN main/ Y NO EN UN COMPONENTE
 *     La aplicación es la única parte del firmware que sabe en qué placa
 *     corre. Los componentes (nebulaedge_i2c, nebulaedge_microsd) reciben los
 *     pines inyectados, para que copiar cualquiera de esas carpetas a otro
 *     proyecto no arrastre la placa de NebulaEdge.
 *
 *     Antes estos números vivían en nebulaedge_defs.h, o sea que la aplicación
 *     leía el pinout de su propia placa desde un componente.
 *
 * PLACA ACTUAL: im-v2
 *     Los comentarios de cada línea guardan el valor que tenía im-v1, que es
 *     lo que había antes en nebulaedge_defs.h. Sirven para volver atrás sin
 *     tener que buscar el esquemático.
 */

/* --- Bus I2C (sensores) --- */
#define I2C_MASTER_FREQ_HZ      100000
#define I2C_MASTER_SCL_IO       GPIO_NUM_2      // im-v1: GPIO_NUM_47
#define I2C_MASTER_SDA_IO       GPIO_NUM_42     // im-v1: GPIO_NUM_48

/* --- Bus SPI (microSD) ---
 *
 * OJO: estos cuatro estaban TRIPLICADOS y las copias no coincidían.
 * nebulaedge_microsd.c se definía los suyos con los valores de im-v1
 * (MOSI GPIO_NUM_2, MISO GPIO_NUM_44, CLK GPIO_NUM_43) y nunca incluyó
 * nebulaedge_defs.h, así que la microSD quedó compilando contra el pinout de
 * la placa vieja. No rompía nada porque sd_mount() está comentado en main.c.
 *
 * Acá quedan los valores de im-v2, que son los que documentaba defs.h. Hay que
 * CONTRASTARLOS CON EL ESQUEMÁTICO antes de volver a habilitar la SD: en im-v2
 * el GPIO_NUM_2 que usaba la copia vieja como MOSI es el reloj del I2C.
 */
#define PIN_NUM_CS              GPIO_NUM_1      // im-v1: GPIO_NUM_1 (igual)
#define PIN_NUM_MOSI            GPIO_NUM_21     // im-v1: GPIO_NUM_2
#define PIN_NUM_CLK             GPIO_NUM_38     // im-v1: GPIO_NUM_43
#define PIN_NUM_MISO            GPIO_NUM_47     // im-v1: GPIO_NUM_44

#endif
