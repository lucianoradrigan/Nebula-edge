/* WiFi station connect */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "nebulaedge_wifi.h"

// FreeRTOS event group to signal when we are connected
static EventGroupHandle_t s_wifi_event_group;

// The event group allows multiple bits for each event, but we only care about two events:
// 0. we are connected to the AP with an IP
// 1. we failed to connect after the maximum amount of retries
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "nebulaedge_wifi";
static int s_retry_num = 0;
static bool s_wifi_active = false;
static bool s_netif_inited = false;
static esp_netif_t *s_sta_netif = NULL;
static bool s_handlers_registered = false;
static esp_event_handler_instance_t s_instance_any_id;
static esp_event_handler_instance_t s_instance_got_ip;

/* Copia propia de la config, con las cadenas adentro. El handler de eventos
 * recibe un puntero a ella y puede correr en cualquier momento, también
 * durante wifi_deinit_sta(). Antes recibía el puntero que pasaba el caller:
 * una variable local de main.c, cuyas cadenas además apuntaban dentro de
 * current_config. Al terminar el bloque y liberarse la config, el handler leía
 * memoria muerta (se vio como un SSID con basura en el log). */
static global_wifi_config s_cfg;
static char s_ssid[33];
static char s_password[65];

/* Tope para conectarse. Con portMAX_DELAY, una red que no genera ni
 * "conectado" ni "desconectado" (p.ej. un SSID vacío, que esp_wifi_connect()
 * rechaza sin evento) dejaba al device esperando para siempre, sin procesar
 * siquiera la config de rescate por BLE. Se vio 4 h en la prueba del 05-11. */
#define WIFI_CONNECT_TIMEOUT_MS 30000

/* Pérdida de la red CON el device ya en operación. Pasada la conexión inicial
 * nadie espera WIFI_FAIL_BIT: la task principal está bloqueada hasta un cambio
 * de protocolo. Antes, al agotar los reintentos el device quedaba para siempre
 * mandando a una red que ya no estaba ("Host is unreachable" con UDP, visto el
 * 04-11 al bajar el AP). Ahora reintenta durante WIFI_LOST_RESTART_MS y, si no
 * vuelve, se reinicia para recibir config por BLE. */
#define WIFI_LOST_RESTART_MS 60000
static bool s_connected_once = false;   // ya hubo IP en esta sesión WiFi
static bool s_stopping = false;         // wifi_deinit_sta() en curso: las desconexiones son esperadas
static TickType_t s_lost_at = 0;        // cuándo se perdió la red en operación (0 = conectado)

/**
 * @brief Event handler for Wi-Fi and IP events.
 *
 * This function handles events related to Wi-Fi and IP operations. It is 
 * triggered when specific events occur, such as changes in Wi-Fi connection 
 * status or IP address configuration.
 *
 * @param[in] arg        User-defined data passed to the event handler.
 * @param[in] event_base Base identifier for the event (e.g., Wi-Fi or IP events).
 * @param[in] event_id   Identifier for the specific event being handled.
 * @param[in] event_data Pointer to data associated with the event.
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    
    // Cast the argument to the custom Wi-Fi configuration structure
    global_wifi_config *config = (global_wifi_config *)arg;                 

    // Handle the event when the station starts
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Attempt to connect to the configured Wi-Fi network
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() rechazó la red: %s", esp_err_to_name(err));
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } 

    // Handle the event when the station is disconnected
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        if (s_stopping) {
            return;
        }

        // En operación: reintentar con plazo y, si no vuelve, reiniciar.
        if (s_connected_once) {
            TickType_t now = xTaskGetTickCount();
            if (s_lost_at == 0) {
                s_lost_at = now ? now : 1;
                ESP_LOGW(TAG, "Se perdió la red %s: se reintenta hasta %d s", config->ssid,
                         WIFI_LOST_RESTART_MS / 1000);
            }
            if (now - s_lost_at > pdMS_TO_TICKS(WIFI_LOST_RESTART_MS)) {
                ESP_LOGE(TAG, "La red %s no volvió. Restarting...", config->ssid);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
            esp_wifi_connect();
            return;
        }

        // Check if the maximum number of retries has not been reached
        if (s_retry_num < config->max_retry) {
            /* La contraseña NO se loguea. Se imprimía en claro en cada intento
             * de reconexión, y la salida serie se suele redirigir a archivo
             * durante las pruebas, así que terminaba en disco. Se deja el largo
             * porque es lo único útil para depurar: distingue "llegó vacía" de
             * "llegó y no autentica". */
            ESP_LOGI(TAG, "trying to connect to the AP (SSID: %s, PASS: %u chars): attempt %d",
                     config->ssid,
                     (unsigned)strlen((const char *)config->password),
                     s_retry_num + 1);

            // Attempt to reconnect
            esp_wifi_connect();

            // Log the delay before the next retry
            ESP_LOGI(TAG, "failed. Retrying in %d milliseconds...", config->retry_delay_ms);
            vTaskDelay(pdMS_TO_TICKS(config->retry_delay_ms));

            // Increment the retry counter
            s_retry_num++;
        }
        else {
            // Set the failure bit in the event group if maximum retries are reached
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }

    // Handle the event when the station gets an IP address
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        // Extract the IP address information from the event data
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;

        // Log the obtained IP address
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));

        // Reset the retry counter
        s_retry_num = 0;
        if (s_lost_at != 0) {
            ESP_LOGI(TAG, "Red recuperada");
        }
        s_connected_once = true;
        s_lost_at = 0;

        // Set the connected bit in the event group
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Initializes the Wi-Fi station (STA) mode with the provided configuration.
 *
 * This function sets up the Wi-Fi station mode using the given global Wi-Fi configuration.
 * Before calling this function, the following components must be initialized:
 * - Non-Volatile Storage (NVS) by calling `nvs_flash_init()`.
 * - Network interface (netif) by calling `esp_netif_init()`.
 * - Default event loop by calling `esp_event_loop_create_default()`.
 *
 * @param global_wifi_config Pointer to the global Wi-Fi configuration structure.
 */
static void wifi_init_sta(global_wifi_config *caller_config) {
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    // Copia propia: ver s_cfg.
    strlcpy(s_ssid, caller_config->ssid ? caller_config->ssid : "", sizeof s_ssid);
    strlcpy(s_password, caller_config->password ? caller_config->password : "", sizeof s_password);
    s_cfg = *caller_config;
    s_cfg.ssid = s_ssid;
    s_cfg.password = s_password;
    global_wifi_config *global_wifi_config = &s_cfg;
    s_retry_num = 0;
    s_connected_once = false;
    s_lost_at = 0;

    /* Sin SSID no hay a qué conectarse: se reinicia para volver por BLE, igual
     * que cuando se agotan los reintentos. */
    if (s_ssid[0] == '\0') {
        ESP_LOGE(TAG, "La config no trae SSID. Restarting...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }

    // Create an event group to handle Wi-Fi connection states
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    // Create the default network interface for station mode
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    // Initial Wi-Fi configuration with default values
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Declaration of instances to handle events
    // Register an event handler for any Wi-Fi event, and one for obtaining an IP
    // A pointer to the custom configuration structure is passed.
    if (!s_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            global_wifi_config,
                                                            &s_instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            global_wifi_config,
                                                            &s_instance_got_ip));
        s_handlers_registered = true;
    }

    // Wi-Fi configuration
    wifi_config_t wifi_config = {
        .sta = {
        // Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
        // If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
        // to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
        // WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
        .threshold.authmode = global_wifi_config->auth_mode,
        },
    };

    // Copy the SSID and password values from the custom structure to the Wi-Fi configuration
    strncpy((char *)wifi_config.sta.ssid, global_wifi_config->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, global_wifi_config->password, sizeof(wifi_config.sta.password));
    
    // Set the Wi-Fi mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Apply the Wi-Fi configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi initialization finished.");

    // Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    // number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    // xEventGroupWaitBits() returns the bits before the call returned, hence 
    // we can test which event actually happened.
    if (bits & WIFI_CONNECTED_BIT) {
        // Conexión exitosa a la red wifi. La contraseña NO se loguea: ver la
        // nota en el handler de desconexión, más arriba.
        ESP_LOGI(TAG, "Connection succesful. SSID:%s", global_wifi_config->ssid);
    } 
    else if (bits & WIFI_FAIL_BIT) {
        /* Error al conectar. Acá el largo de la contraseña sí sirve: distingue
         * "llegó vacía" de "llegó y el AP la rechaza", que es justo la duda
         * cuando la conexión falla. */
        ESP_LOGI(TAG, "Failed to connect to SSID: %s (PASS: %u chars)",
                 global_wifi_config->ssid,
                 (unsigned)strlen((const char *)global_wifi_config->password));
        ESP_LOGI(TAG, "Restarting...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } 
    else {
        // Venció el tope sin conectar ni fallar: mismo camino que el fallo.
        ESP_LOGE(TAG, "Sin conexión a SSID %s tras %d ms. Restarting...", global_wifi_config->ssid,
                 WIFI_CONNECT_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
}

void wifi_start_if_needed(global_wifi_config *global_wifi_config) {
    if (s_wifi_active) {
        ESP_LOGI(TAG, "wifi_start_if_needed: already active");
        return;
    }
    if (!s_netif_inited) {
        ESP_LOGI(TAG, "wifi_start_if_needed: init netif/event loop");
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_netif_inited = true;
    }
    ESP_LOGI(TAG, "wifi_start_if_needed: init STA");
    wifi_init_sta(global_wifi_config);
    s_wifi_active = true;
}

void wifi_deinit_sta(void) {
    if (!s_wifi_active) {
        ESP_LOGI(TAG, "wifi_deinit_sta: already inactive");
        return;
    }
    ESP_LOGI(TAG, "wifi_deinit_sta: stop/deinit wifi");
    s_stopping = true;          // la desconexión que sigue es pedida, no una pérdida
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_handlers_registered) {
        ESP_LOGI(TAG, "wifi_deinit_sta: unregister handlers");
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_instance_any_id);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_instance_got_ip);
        s_handlers_registered = false;
    }
    if (s_sta_netif != NULL) {
        ESP_LOGI(TAG, "wifi_deinit_sta: destroy netif");
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_wifi_event_group != NULL) {
        ESP_LOGI(TAG, "wifi_deinit_sta: delete event group");
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    s_wifi_active = false;
    s_stopping = false;
    s_connected_once = false;
    s_lost_at = 0;
}