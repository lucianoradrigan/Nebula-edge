#ifndef BOARD_PINOUT
#define BOARD_PINOUT

#include "driver/gpio.h"
#include "driver/spi_common.h"   // SPI3_HOST

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
 * OJO: estos pines estaban TRIPLICADOS y las copias no coincidían.
 * nebulaedge_microsd.c se definía los suyos con los valores de im-v1
 * (MOSI GPIO_NUM_2, MISO GPIO_NUM_44, CLK GPIO_NUM_43) y nunca incluyó
 * nebulaedge_defs.h, así que la microSD quedó compilando contra el pinout de
 * la placa vieja. Había una cuarta copia en nebulaedge_spi.h; ese componente se
 * borró y el escaneo de buses ahora recibe estos pines por parámetro.
 *
 * Los tres valores de abajo coinciden con los del proyecto de bringup de la
 * IM-V2, que es el único código de la microSD probado contra la placa.
 */
#define PIN_NUM_MOSI            GPIO_NUM_21     // im-v1: GPIO_NUM_2
#define PIN_NUM_CLK             GPIO_NUM_38     // im-v1: GPIO_NUM_43
#define PIN_NUM_MISO            GPIO_NUM_47     // im-v1: GPIO_NUM_44

/* Periférico SPI y reloj de la tarjeta. El bringup fija los dos explícitamente
 * en vez de tomar los de SDSPI_HOST_DEFAULT() (SPI2_HOST a 20 MHz), que es lo
 * que este firmware venía usando sin que nadie lo decidiera. Con el chip
 * select saliendo por I2C a 400 kHz, 20 MHz de SPI es optimista. */
#define SD_SPI_HOST             SPI3_HOST
#define SD_MAX_FREQ_KHZ         4000

/* --- Expansor de IO (FXL6408) ---
 *
 * En la im-v2 el chip select de la microSD dejó de ser un GPIO: cuelga del IO0
 * de este expansor. Por eso ya no hay PIN_NUM_CS; el de im-v1 era GPIO_NUM_1,
 * que en esta placa está ocupado.
 *
 * El bringup además deja los IO 4, 6 y 7 en alto para desactivar otros
 * dispositivos del bus SPI, lo que sugiere que el expansor maneja más
 * periféricos que la sola tarjeta. */
#define FXL6408_I2C_ADDR        0x44            // pin ADDR a VCC
#define SD_CS_EXPANDER_PIN      0               // IO0
#define SPI_DISABLED_EXPANDER_PINS  { 4, 6, 7 }

#endif
