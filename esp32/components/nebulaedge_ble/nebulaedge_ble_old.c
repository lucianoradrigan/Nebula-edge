#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

#define TAG "nebulaedge_gatts"

static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event,
                                          esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t *param);

#define GATTS_SERVICE_UUID_TEST_A 0x00FF
#define GATTS_CHAR_UUID_TEST_A 0xFF01
#define GATTS_DESCR_UUID_TEST_A 0x3333
#define GATTS_NUM_HANDLE_TEST_A 4

#define TEST_DEVICE_NAME "ESP_GATTS_DEMO"
#define TEST_MANUFACTURER_DATA_LEN 17

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0x40

#define PREPARE_BUF_MAX_SIZE 1024

static uint8_t char1_str[] = {0x11, 0x22, 0x33};
static esp_gatt_char_prop_t a_property = 0;

static esp_attr_value_t gatts_demo_char1_val = {
    .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX,
    .attr_len = sizeof(char1_str),
    .attr_value = char1_str,
};

static uint8_t adv_config_done = 0;
#define adv_config_flag (1 << 0)
#define scan_rsp_config_flag (1 << 1)

static uint8_t adv_service_uuid128[32] = {
    /* LSB
       <-------------------------------------------------------------------------------->
       MSB */
    // first uuid, 16bit, [12],[13] is the value
    0xfb,
    0x34,
    0x9b,
    0x5f,
    0x80,
    0x00,
    0x00,
    0x80,
    0x00,
    0x10,
    0x00,
    0x00,
    0xEE,
    0x00,
    0x00,
    0x00,
    // second uuid, 32bit, [12], [13], [14], [15] is the value
    0xfb,
    0x34,
    0x9b,
    0x5f,
    0x80,
    0x00,
    0x00,
    0x80,
    0x00,
    0x10,
    0x00,
    0x00,
    0xFF,
    0x00,
    0x00,
    0x00,
};

// The length of adv data must be less than 31 bytes
// static uint8_t test_manufacturer[TEST_MANUFACTURER_DATA_LEN] =  {0x12, 0x23,
// 0x45, 0x56}; adv data
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,  // slave connection min interval, Time =
                             // min_interval * 1.25 msec
    .max_interval = 0x0010,  // slave connection max interval, Time =
                             // max_interval * 1.25 msec
    .appearance = 0x00,
    .manufacturer_len = 0,        // TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL,  //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/* Datos de respuesta al escaneo de un cliente */
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    //.min_interval = 0x0006,
    //.max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,        // TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL,  //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// Parámetros de advertising
static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#define PROFILE_NUM 2
#define PROFILE_A_APP_ID 0
#define PROFILE_B_APP_ID 1

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;

    // acá se extiende para tener múltiples características
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;

    
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

/* Perfil para cada aplicación. Cada uno tiene su propio perfil, y todos
 * se deben escribir aquí. Esta implementación soporta un cliente a la vez. */
static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    // Perfil 1
    [PROFILE_A_APP_ID] = {

        // Callback de eventos para este perfil
        .gatts_cb = gatts_profile_a_event_handler,

        // Interfaz GATT (se asigna después del registro)
        .gatts_if = ESP_GATT_IF_NONE, 

        // Propiedades de la característica principal
        .property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE |
                    ESP_GATT_CHAR_PROP_BIT_NOTIFY, 
    }
};

typedef struct {
    uint8_t *prepare_buf;
    int prepare_len;
} prepare_type_env_t;

static prepare_type_env_t a_prepare_write_env;

void example_write_event_env(esp_gatt_if_t gatts_if,
                             prepare_type_env_t *prepare_write_env,
                             esp_ble_gatts_cb_param_t *param);
void example_exec_write_event_env(prepare_type_env_t *prepare_write_env,
                                  esp_ble_gatts_cb_param_t *param);

// Manejador de eventos BLE: esto se deja tal cual, por el momento
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    
    switch (event) {
        // Llega este evento cuando el set de configuración de advertisement termina
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~adv_config_flag);
            if (adv_config_done == 0) {
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;

        // Llega este evento cuando los datos de response
        // son completados
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~scan_rsp_config_flag);
            if (adv_config_done == 0) {
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;
        
        // Llega este evento cuando se inicia el advertisement
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            // advertising start complete event to indicate advertising start
            // successfully or failed
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising start failed");
            }
            break;

        // Evento llega cuando se detiene el advertising.
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising stop failed");
            } else {
                ESP_LOGI(TAG, "Stop adv successfully");
            }
            break;

        // Evento llega cuando se actualizan los parámetros de conexión BLE.
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG,
                     "update connection params status = %d, min_int = %d, "
                     "max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                     param->update_conn_params.status,
                     param->update_conn_params.min_int,
                     param->update_conn_params.max_int,
                     param->update_conn_params.conn_int,
                     param->update_conn_params.latency,
                     param->update_conn_params.timeout);
            break;

        // Cualquier otro evento GAP no manejado explícitamente.
        default:
            break;
    }
}

// Función write de uso transversal: no se modifica
void example_write_event_env(esp_gatt_if_t gatts_if,
                             prepare_type_env_t *prepare_write_env,
                             esp_ble_gatts_cb_param_t *param) {
    esp_gatt_status_t status = ESP_GATT_OK;
    if (param->write.need_rsp) {
        if (param->write.is_prep) {
            if (prepare_write_env->prepare_buf == NULL) {
                prepare_write_env->prepare_buf =
                    (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE * sizeof(uint8_t));
                prepare_write_env->prepare_len = 0;
                if (prepare_write_env->prepare_buf == NULL) {
                    ESP_LOGE(TAG, "Gatt_server prep no mem");
                    status = ESP_GATT_NO_RESOURCES;
                }
            } else {
                if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
                    status = ESP_GATT_INVALID_OFFSET;
                } else if ((param->write.offset + param->write.len) >
                           PREPARE_BUF_MAX_SIZE) {
                    status = ESP_GATT_INVALID_ATTR_LEN;
                }
            }

            esp_gatt_rsp_t *gatt_rsp =
                (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value,
                   param->write.len);
            esp_err_t response_err = esp_ble_gatts_send_response(
                gatts_if, param->write.conn_id, param->write.trans_id, status,
                gatt_rsp);
            if (response_err != ESP_OK) {
                ESP_LOGE(TAG, "Send response error");
            }
            free(gatt_rsp);
            if (status != ESP_GATT_OK) {
                return;
            }
            memcpy(prepare_write_env->prepare_buf + param->write.offset,
                   param->write.value, param->write.len);
            prepare_write_env->prepare_len += param->write.len;

        } else {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, status, NULL);
        }
    }
}

esp_err_t set_characteristic_value(uint16_t char_handle, const uint8_t *value, uint16_t length) {
    esp_err_t status = esp_ble_gatts_set_attr_value(char_handle, length, value);

    if (status == ESP_GATT_OK) {
        printf("Characteristic value set successfully.");
    } else {
        printf("Failed to set characteristic value, error: %d", status);
    }

    return status;
}

esp_err_t get_characteristic_value(uint16_t char_handle, uint8_t **result, uint16_t *length) {
    const uint8_t *value;
    esp_err_t status = esp_ble_gatts_get_attr_value(char_handle, length, &value);

    if (status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Get characteristic value successfully.");
        *result = (uint8_t *)malloc(*length + 1);  // Allocate memory for string, including null-terminator
        if (*result != NULL) {
            memcpy(*result, value, *length);  // Copy the data
            (*result)[*length] = '\0';        // Null-terminate the string
            printf("Characteristic value: %s", (char *)*result);
        } 
        else {
            printf("Failed to allocate memory for the characteristic value");
        }
    } else {
        printf("Could not get characteristic value, error: %d", status);
    }

    return status;
}

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param) {
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) {
        esp_log_buffer_hex(TAG, prepare_write_env->prepare_buf,
                           prepare_write_env->prepare_len);
    } else {
        ESP_LOGI(TAG, "ESP_GATT_PREP_WRITE_CANCEL");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}


// Handler de eventos GATTS
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param) {

    // If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        }
        else {
            ESP_LOGI(
                TAG, "Reg app failed, app_id %04x, status %d", 
                param->reg.app_id, 
                param->reg.status
            );
            return;
        }
    }

    // If the gatts_if equal to profile A, call profile A cb handler,
    // so here call each profile's callback
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gatts_if ==
                    ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a
                                           certain gatt_if, need to call
                                           every profile cb function */
                gatts_if == gl_profile_tab[idx].gatts_if) {
                if (gl_profile_tab[idx].gatts_cb) {
                    gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}


// Event handler del perfil de la aplicación A
// One gatt-based profile one app_id and one gatts_if, 
// this array will store the gatts_if returned by ESP_GATTS_REG_EVT
static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event,
                                          esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t *param) {
    switch (event) {

        // Evento de registro de app: se llamó la función register_app
        case ESP_GATTS_REG_EVT:
            ESP_LOGI(TAG, "REGISTER_APP_EVT, status %d, app_id %d", 
                param->reg.status, param->reg.app_id);

            // Configuración del perfil de la aplicación (confirmar)
            gl_profile_tab[PROFILE_A_APP_ID].service_id.is_primary = true;
            gl_profile_tab[PROFILE_A_APP_ID].service_id.id.inst_id = 0x00;
            gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
            gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.uuid.uuid16 = GATTS_SERVICE_UUID_TEST_A;

            // Setea del nombre del dispositivo
            esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(TEST_DEVICE_NAME);
            if (set_dev_name_ret) {
                ESP_LOGE(TAG, "set device name failed, error code = %x", set_dev_name_ret);
            }

            // Configuración datos de advertising: desde aquí se gatilla el evento GAP
            // para iniciar el advertising, y una vez finalizado la ESP queda lista para
            // realizar una conexión BLE
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret) {
                ESP_LOGE(TAG, "config adv data failed, error code = %x", ret);
            }
            adv_config_done |= adv_config_flag;

            // Configura los datos de scan response para el BLE advertising.
            // Es decir, qué cosa enviará como respuesta cuando un cliente BLE
            // escanee y pida información sobre el dispositivo: nombre, UUID, etc
            ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
            if (ret) {
                ESP_LOGE(TAG, "config scan response data failed, error code = %x", ret);
            }
            adv_config_done |= scan_rsp_config_flag;

            // Crea un servicio!
            esp_ble_gatts_create_service(
                gatts_if, 
                &gl_profile_tab[PROFILE_A_APP_ID].service_id, 
                GATTS_NUM_HANDLE_TEST_A
            );
            break;

    
        // Evento de lectura: cliente pidió leer datos. ESP envía los datos.
        case ESP_GATTS_READ_EVT:
            ESP_LOGI(
                TAG,
                "GATT_READ_EVT, conn_id %d, trans_id %" PRIu32 ", handle %d",
                param->read.conn_id,    /* Connection ID */
                param->read.trans_id,   /* Transfer ID */
                param->read.handle      /* Característica que intenta leer el cliente */
            );


            // if (param->read.handle == gl_profile_tab[PROFILE_A_APP_ID].char_handle) {
            //     // El cliente quiere leer la característica principal
            // }
            // else if (param->read.handle == gl_profile_tab[PROFILE_A_APP_ID].descr_handle) {
            //     // El cliente quiere leer el descriptor
            // }


            // Tipo respuesta para una solicitud de lectura remota GATT
            esp_gatt_rsp_t rsp;
            esp_err_t response_ret;

            uint8_t *value_ptr = NULL;
            uint16_t value_length = 0;
            
            // Obtiene valor de la característica A y lo asigna a un puntero
            get_characteristic_value(
                gl_profile_tab[PROFILE_A_APP_ID].char_handle, 
                &value_ptr, 
                &value_length
            );

            // Obtiene timestamp
            // int32_t timestamp = get_timestamp_from_custom_epoch();

            // set timestamp in value_ptr on
            // uint16_t pos_to_write = sizeof(hd_01234_t) + 2 * sizeof(char);


            // memcpy(value_ptr + pos_to_write, &timestamp, sizeof(int32_t));


            // Prepara la estructura rsp (respuesta)
            // Copia el paquete al campo value de rsp
            memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
            rsp.attr_value.len = value_length;  
            memcpy(rsp.attr_value.value, value_ptr, value_length);  // Copy string to value field

            // Realiza respuesta al cliente, enviándole los datos
            response_ret = esp_ble_gatts_send_response(
                gatts_if, 
                param->read.conn_id, 
                param->read.trans_id, 
                ESP_GATT_OK, 
                &rsp
            );
                
            if (response_ret != ESP_OK) {
                ESP_LOGE(TAG, "Send response error");
            } 
            else {
                ESP_LOGI(TAG, "Send response success");
            }

            // Libera puntero
            free(value_ptr);
            break;
        
        // Evento de escritura: cliente desea escribir un dato en la ESP32
        case ESP_GATTS_WRITE_EVT:
            ESP_LOGI(
                TAG,
                "GATT_WRITE_EVT, conn_id %d, trans_id %" PRIu32", handle %d",
                param->write.conn_id,   /* Connection ID */
                param->write.trans_id,  /* Transfer ID */
                param->write.handle     /* Característica que intenta escribir el cliente */
            );

            // Cuando la escritura es parte de una operación prepared write
            if (!param->write.is_prep) {
                ESP_LOGI(TAG, "GATT_WRITE_EVT, value len %d, value :", param->write.len);
                esp_log_buffer_hex(TAG, param->write.value, param->write.len);

                if (gl_profile_tab[PROFILE_A_APP_ID].descr_handle == 
                    param->write.handle && param->write.len == 2) {

                        uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];

                    if (descr_value == 0x0001) {

                        if (a_property & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                            ESP_LOGI(TAG, "notify enable");
                            uint8_t notify_data[15];

                            for (int i = 0; i < sizeof(notify_data); ++i) {
                                notify_data[i] = i % 0xff;
                            }

                            param->write.need_rsp = 1;
                            // the size of notify_data[] need less than MTU size
                            esp_ble_gatts_send_indicate(
                                gatts_if, param->write.conn_id,
                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                sizeof(notify_data), 
                                notify_data, 
                                false
                            );
                        }
                    } 
                    else if (descr_value == 0x0002) {
                        if (a_property & ESP_GATT_CHAR_PROP_BIT_INDICATE) {
                            ESP_LOGI(TAG, "indicate enable");
                            uint8_t indicate_data[15];
                            for (int i = 0; i < sizeof(indicate_data); ++i) {
                                indicate_data[i] = i % 0xff;
                            }
                            // the size of indicate_data[] need less than MTU
                            // size
                            esp_ble_gatts_send_indicate(
                                gatts_if, 
                                param->write.conn_id,
                                gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                sizeof(indicate_data), 
                                indicate_data, 
                                true
                            );
                        }
                    } 
                    else if (descr_value == 0x0000) {
                        ESP_LOGI(TAG, "notify/indicate disable ");
                    } 
                    else {
                        ESP_LOGE(TAG, "unknown descr value");
                        esp_log_buffer_hex(TAG, param->write.value, param->write.len);
                    }
                }
            }
          

            ESP_LOGI(TAG, "Connection Init Message Received");
            ESP_LOGI(TAG, "GATT_WRITE_EVT, value len %d, value :%s", param->write.len, param->write.value);

            // Setea valor a la característica A
            set_characteristic_value(
                gl_profile_tab[PROFILE_A_APP_ID].char_handle, 
                param->write.value, 
                param->write.len
            );

            // OJO, HAY UN EVENTO ADD CHAR -> ESO ESTÁ RE BUENO, o no?

            // Cuando la escritura es parte de una operación prepared write
            example_write_event_env(gatts_if, &a_prepare_write_env, param);
            break;

        // Evento
        case ESP_GATTS_EXEC_WRITE_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_EXEC_WRITE_EVT");
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK,
                                        NULL);
            example_exec_write_event_env(&a_prepare_write_env, param);
            break;

        // Evento
        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
            break;

        // Evento
        case ESP_GATTS_UNREG_EVT:
            break;

        // Evento
        case ESP_GATTS_CREATE_EVT:
            ESP_LOGI(
                TAG,
                "CREATE_SERVICE_EVT, status %d,  service_handle %d",
                param->create.status, 
                param->create.service_handle);

            gl_profile_tab[PROFILE_A_APP_ID].service_handle = param->create.service_handle;
            gl_profile_tab[PROFILE_A_APP_ID].char_uuid.len = ESP_UUID_LEN_16;
            gl_profile_tab[PROFILE_A_APP_ID].char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_TEST_A;

            esp_ble_gatts_start_service(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle);
            a_property = ESP_GATT_CHAR_PROP_BIT_READ |
                         ESP_GATT_CHAR_PROP_BIT_WRITE |
                         ESP_GATT_CHAR_PROP_BIT_NOTIFY;

            // Agrega una característica
            esp_err_t add_char_ret = esp_ble_gatts_add_char(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &gl_profile_tab[PROFILE_A_APP_ID].char_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 
                a_property,
                &gatts_demo_char1_val, 
                NULL
            );


            if (add_char_ret) {
                ESP_LOGE(TAG, "add char failed, error code =%x",
                         add_char_ret);
            }
            break;

        // Evento
        case ESP_GATTS_ADD_INCL_SRVC_EVT:
            break;

        // Evento
        case ESP_GATTS_ADD_CHAR_EVT: {
            uint16_t length = 0;
            const uint8_t *prf_char;

            ESP_LOGI(TAG,
                     "ADD_CHAR_EVT, status %d,  attr_handle %d, "
                     "service_handle %d",
                     param->add_char.status, param->add_char.attr_handle,
                     param->add_char.service_handle);

            gl_profile_tab[PROFILE_A_APP_ID].char_handle = param->add_char.attr_handle;
            gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
            gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
            esp_err_t get_attr_ret = esp_ble_gatts_get_attr_value(param->add_char.attr_handle, &length, &prf_char);
            
            if (get_attr_ret == ESP_FAIL) {
                ESP_LOGE(TAG, "ILLEGAL HANDLE");
            }

            ESP_LOGI(TAG, "the gatts demo char length = %x", length);
            for (int i = 0; i < length; i++) {
                ESP_LOGI(TAG, "prf_char[%x] = %x", i, prf_char[i]);
            }

            esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &gl_profile_tab[PROFILE_A_APP_ID].descr_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);

            if (add_descr_ret) {
                ESP_LOGE(TAG, "add char descr failed, error code =%x",
                         add_descr_ret);
            }
            break;
        }
        
        // Evento
        case ESP_GATTS_ADD_CHAR_DESCR_EVT:
            gl_profile_tab[PROFILE_A_APP_ID].descr_handle =
                param->add_char_descr.attr_handle;
            ESP_LOGI(TAG,
                     "ADD_DESCR_EVT, status %d, attr_handle %d, "
                     "service_handle %d",
                     param->add_char_descr.status,
                     param->add_char_descr.attr_handle,
                     param->add_char_descr.service_handle);
            break;

        // Evento
        case ESP_GATTS_DELETE_EVT:
            break;

        // Evento
        case ESP_GATTS_START_EVT:
            ESP_LOGI(TAG,
                     "SERVICE_START_EVT, status %d, service_handle %d",
                     param->start.status, param->start.service_handle);
            break;

        // Evento
        case ESP_GATTS_STOP_EVT:
            break;
        
        // Evento
        case ESP_GATTS_CONNECT_EVT: {
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda,
                   sizeof(esp_bd_addr_t));
            /* For the IOS system, please reference the apple official
             * documents about the ble connection parameters restrictions.
             */
            conn_params.latency = 0;
            conn_params.max_int = 0x20;  // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x10;  // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 400;   // timeout = 400*10ms = 4000ms
            ESP_LOGI(TAG,
                     "ESP_GATTS_CONNECT_EVT, conn_id %d, remote "
                     "%02x:%02x:%02x:%02x:%02x:%02x:",
                     param->connect.conn_id, param->connect.remote_bda[0],
                     param->connect.remote_bda[1], param->connect.remote_bda[2],
                     param->connect.remote_bda[3], param->connect.remote_bda[4],
                     param->connect.remote_bda[5]);
            gl_profile_tab[PROFILE_A_APP_ID].conn_id = param->connect.conn_id;
            // start sent the update connection parameters to the peer
            // device.
            esp_ble_gap_update_conn_params(&conn_params);
            break;
        }

        // Evento
        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG,
                     "ESP_GATTS_DISCONNECT_EVT, disconnect reason 0x%x",
                     param->disconnect.reason);
            esp_ble_gap_start_advertising(&adv_params);
            gl_profile_tab[PROFILE_A_APP_ID].conn_id = 0xFF;
            break;

        // Evento
        case ESP_GATTS_CONF_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_CONF_EVT, status %d attr_handle %d",
                     param->conf.status, param->conf.handle);
            if (param->conf.status != ESP_GATT_OK) {
                esp_log_buffer_hex(TAG, param->conf.value,
                                   param->conf.len);
            }
            break;

        // Eventos sin efecto
        case ESP_GATTS_OPEN_EVT:
        case ESP_GATTS_CANCEL_OPEN_EVT:
        case ESP_GATTS_CLOSE_EVT:
        case ESP_GATTS_LISTEN_EVT:
        case ESP_GATTS_CONGEST_EVT:
        default:
            break;
    }
}


/* Envía una notificación en forma de string al cliente conectado. */
esp_err_t send_notify(char *notify_data) {
    // check if connected
    if (gl_profile_tab[PROFILE_A_APP_ID].conn_id == 0xFF) {  
        // assuming 0xFF is default when not connected
        ESP_LOGE(TAG, "Not connected to a client");
        return ESP_FAIL;
    }

    size_t len = strlen(notify_data);
    esp_err_t ret = esp_ble_gatts_send_indicate(
        gl_profile_tab[PROFILE_A_APP_ID].gatts_if,
        gl_profile_tab[PROFILE_A_APP_ID].conn_id,

        // esta es la característica en específico
        gl_profile_tab[PROFILE_A_APP_ID].char_handle, 
        len,
        (uint8_t *)notify_data, 
        false
    );

    if (ret) {
        ESP_LOGE(TAG, "Send indicate error");
    } 
    else {
        ESP_LOGI(TAG, "Send indicate success");
    }
    return ret;
}

// void cont_mode_loop() {
//     while (1) {
//         // get config
//         config_t config;
//         get_nvs_config(&config);
//         int packet_lenght;
//         char *packet = create_packet(config.protocol_id, &packet_lenght,
//                                      config.trans_layer);

//         set_characteristic_value(gl_profile_tab[PROFILE_A_APP_ID].char_handle,
//                                  (uint8_t *)packet, packet_lenght);


//         vTaskDelay(500 / portTICK_PERIOD_MS);
//         send_notify("CHK_DATA");
//         vTaskDelay(7000 / portTICK_PERIOD_MS);
//     }
// }

// void dis_cont_mode_loop() {
//     while (1) {
//         // get config
//         config_t config;
//         get_nvs_config(&config);
//         int packet_lenght;
//         esp_err_t ret;

//         ESP_LOGI(TAG, 
//             "Creating packet UISNG PROTOCOL %d AND TRANS LAYER %c",
//             config.protocol_id, 
//             config.trans_layer);

//         char *packet = create_packet(config.protocol_id, &packet_lenght, config.trans_layer);

//         // Cambia el valor de la característica
//         set_characteristic_value(gl_profile_tab[PROFILE_A_APP_ID].char_handle,
//                                  (uint8_t *)packet, packet_lenght);
        
        

//         // Manda a deep sleep la ESP
//         // if (gl_profile_tab[PROFILE_A_APP_ID].conn_id != 0xFF || ret == ESP_OK) {
//         //     ESP_LOGI(TAG, "Going to sleep");
//         //     esp_sleep_enable_timer_wakeup((long long)(BLE_DISC_TIMEOUT_SEC * 1e+6));
//         //     esp_deep_sleep_start();
//         // }
//     }
// }

void main_ble(void) {
    esp_err_t ret;

    // Libera la memoria reservada del controlador de Bluetooth clásico
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Inicializa el controlador Bluetooth con la configuración especificada.
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s", __func__,
                 esp_err_to_name(ret));
        return;
    }

    // Habilita el controlador Bluetooth en modo BLE
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "%s enable controller failed: %s", __func__,
                 esp_err_to_name(ret));
        return;
    }

    // Inicializa el stack Bluetooth (Bluedroid)
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "%s init bluetooth failed: %s", __func__,
                 esp_err_to_name(ret));
        return;
    }

    // Habilita el stack de Bluetooth (Bluedroid) para comenzar a operar
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "%s enable bluetooth failed: %s", __func__,
                 esp_err_to_name(ret));
        return;
    }

    // Registra event handler GATT.
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "Error al registrar GATT server, código de error = %x", ret);
        return;
    }

    // Registra event handler GAP
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "Error al registrar GAP, código de error = %x", ret);
        return;
    }

    // Registra aplicación 1
    ret = esp_ble_gatts_app_register(PROFILE_A_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "Error al registrar la aplicación GATT, código de error = %x", ret);
        return;
    }

    // Declara un paquete de prueba sencillo en formato uint8_t.
    const uint8_t packet[] = {0x01, 0x02, 0x03, 0x04, 0x05};

    // Establece la longitud del paquete.
    uint16_t packet_length = 6;
 
    // Asigna el valor de una característica en particular, con el valor de packet

    esp_err_t status = set_characteristic_value(gl_profile_tab[PROFILE_A_APP_ID].char_handle, packet, packet_length);

    // Configura el tamaño de la MTU (Unidad Máxima de Transmisión) local a 500 bytes.
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret) {
        ESP_LOGE(TAG, "Error al configurar la MTU local, código de error = %x", local_mtu_ret);
    }

    // Inicializa el ID de conexión a 0xFF, indicando que no hay conexión activa.
    gl_profile_tab[PROFILE_A_APP_ID].conn_id = 0xFF;

    // Esto reemplaza los valores de webaditas
    // set_characteristic_value(gl_profile_tab[PROFILE_A_APP_ID].char_handle, (uint8_t *)packet, packet_length);
    // ret = send_notify("CHK_DATA");
}
