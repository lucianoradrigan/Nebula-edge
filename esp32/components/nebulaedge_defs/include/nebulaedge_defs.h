#ifndef NEBULAEDGE_DEFS
#define NEBULAEDGE_DEFS

#include "esp_wifi.h"
#include "driver/gpio.h"

/*****************************************************************/
/************************* PROTOCOLOS ****************************/
/*****************************************************************/

typedef struct {
    size_t size;
    uint8_t *data;
} packet_t;

typedef struct {
    char *ssid;
    char *password;
    wifi_auth_mode_t auth_mode;
    int max_retry;
    int retry_delay_ms;
} global_wifi_config;

// Estructura de configuración global MQTT esto se puede extender muchísimo, por ahora
// implementado así por simpleza. Mirar campos de la estructura esp_mqtt_client_config_t
typedef struct {
    const char *broker;
} mqtt_config_global;

typedef enum {
    IPV4,
    IPV6
} ip_version_t;

typedef struct {
    char *ip_host;            // Dirección IP (IPv4)
    int port;                 // Puerto
    ip_version_t ip_version;  // tipo. IPv4 o IPv6
} udp_params_t;

typedef struct {
    char *ip_host;            // Dirección IP (IPv4)
    int port;                 // Puerto
    ip_version_t ip_version;  // tipo. IPv4 o IPv6
} tcp_params_t;

#define DEEP_SLEEP_FLAG_LEN 3U
extern const uint8_t DEEP_SLEEP_FLAG[DEEP_SLEEP_FLAG_LEN];

extern SemaphoreHandle_t semaphore;
extern SemaphoreHandle_t semaphore_ble;


/*****************************************************************/
/*************************** SENSORES ****************************/
/*****************************************************************/

/* Frecuencia MASTER */
#define I2C_MASTER_FREQ_HZ              100000

/* Pines I2C */
#define I2C_MASTER_SCL_IO				GPIO_NUM_2			    // GPIO pin I2C master GPIO_NUM_47 imv1 GPIO_NUM_2 imv2
#define I2C_MASTER_SDA_IO				GPIO_NUM_42				// GPIO pin I2C master GPIO_NUM_48 imv1 GPIO_NUM_42 imv2

/* Direcciones slave de sensores (I2C) */
#define BMM350_SLAVE_ADDR           0x14
#define BMI270_SLAVE_ADDR           0x68
#define BME688_SLAVE_ADDR           0x76

/* Pines SPI (microsd)*/
#define PIN_NUM_CS                          GPIO_NUM_1         // GPIO pin GPIO_NUM_1 im-v1
#define PIN_NUM_MOSI                        GPIO_NUM_21        // GPIO pin GPIO_NUM_2 im-v1
#define PIN_NUM_CLK                         GPIO_NUM_38        // GPIO pin GPIO_NUM_43 im-v1
#define PIN_NUM_MISO                        GPIO_NUM_47        // GPIO pin GPIO_NUM_44 im-v1

/* SD */
#define FORMAT_IF_MOUNT_FAILED              true

/* Data length de BMM350 */
#define BMM350_OTP_DATA_LENGTH              32                 // No confundir con el largo del output

/* Frecuencias */
#define ODR_1_5625                          15625
#define ODR_3_125                           3125
#define ODR_6_25                            625
#define ODR_12_5                            125
#define ODR_25                              25
#define ODR_50                              50
#define ODR_100                             100
#define ODR_200                             200
#define ODR_400                             400
#define ODR_800                             800
#define ODR_1600                            1600
#define ODR_3200                            3200

/* Concatena bytes */
#define CONCAT_BYTES(msb, lsb)      (((uint16_t)msb << 8) | (uint16_t)lsb)

#endif