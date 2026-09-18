#ifndef NEBULAEDGE_CONFIG_STORE
#define NEBULAEDGE_CONFIG_STORE

#include "schema.pb-c.h"

/* Persistencia del Config en la NVS.
 *
 * La única razón por la que esto existe es el deep sleep: al despertar, la
 * ESP32 arranca de cero y no tiene con qué protocolo hablar ni a qué servidor.
 * El Config se guarda antes de dormir y se recupera al volver.
 *
 * Todo lo que necesita saber es serializar un Config y encontrarlo después; no
 * conoce tasks, protocolos ni sensores. */

/* Guarda el Config serializado. Si `cfg` es NULL no hace nada.
 * Los errores se registran en el log y se ignoran: no poder guardar significa
 * que al despertar se pedirá config por BLE, que es el camino de arranque en
 * frío y funciona igual. */
void config_store_save(const Config *cfg);

/* Recupera el Config guardado, o NULL si no hay ninguno o está corrupto.
 * El caller se queda con la propiedad: hay que liberarlo con
 * config__free_unpacked(). */
Config *config_store_load(void);

/* Borra el Config guardado. Se llama en el arranque en frío (reset o
 * encendido), para que un reinicio manual no reviva una config vieja y el
 * dispositivo espere una nueva por BLE. */
void config_store_clear(void);

#endif
