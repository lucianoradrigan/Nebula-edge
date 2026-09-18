#ifndef NEBULAEDGE_DEVICE
#define NEBULAEDGE_DEVICE

#include <stdint.h>

/* Lo que el dispositivo sabe de sí mismo: quién es y qué hora es.
 *
 * No es un cajón de sastre. Son las dos cosas que van en TODOS los paquetes de
 * telemetría (`id_device` y `time_client`), y las dos tienen la misma
 * particularidad: hay que reconstruirlas en cada arranque, porque la ESP32
 * despierta del deep sleep sin reloj y sin haber leído todavía su MAC.
 *
 * Si mañana hace falta guardar otra cosa del dispositivo, el criterio para
 * meterla acá es ese: que la lleve cada paquete y que haya que reconstruirla al
 * despertar. Cualquier otra cosa va en otro lado. */

/* ---------------------------------------------------------------- IDENTIDAD */

/* Lee la MAC Bluetooth y la deja como ID del dispositivo. Se llama una vez al
 * arrancar, antes de que cualquier task arme un paquete. */
void device_id_init(void);

/* ID del dispositivo, con formato "AA:BB:CC:DD:EE:FF".
 *
 * El buffer es del módulo: no hay que liberarlo ni escribirlo. Si todavía no se
 * llamó a device_id_init() devuelve "00:00:00:00:00:00", nunca NULL, para que
 * un paquete armado antes de tiempo salga con un ID reconociblemente falso en
 * vez de reventar. */
char *device_id(void);

/* -------------------------------------------------------------------- RELOJ */

/* La ESP32 no tiene RTC con batería: la hora llega desde el servidor en el
 * campo `time_client` del Config, y se pierde en cada reinicio. Lo único que
 * sobrevive al deep sleep es la memoria RTC, así que la hora se estaciona ahí
 * antes de dormir y se restaura al volver. */

/* Pone el reloj del sistema en el epoch UNIX dado, y deja copia en memoria RTC.
 * Ignora valores <= 0 (un `time_client` sin poner), registrándolo en el log. */
void device_clock_set(int64_t unix_time_s);

/* Epoch actual en segundos, o 0 si el reloj todavía no está en hora. El 0 es
 * deliberado: un paquete con time_client = 0 es identificable como "sin hora"
 * del lado del servidor, en vez de traer una fecha de 1970 que parece real. */
uint32_t device_clock_now_s(void);

/* Estaciona en memoria RTC la hora que va a ser al despertar (ahora + lo que
 * se va a dormir). Se llama justo antes de esp_deep_sleep_start(). */
void device_clock_save_before_deep_sleep(uint64_t sleep_us);

/* Restaura el reloj desde la memoria RTC. Se llama al arrancar, solo si el
 * wake-up fue por timer de deep sleep. */
void device_clock_restore_after_deep_sleep(void);

#endif
