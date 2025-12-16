// Basado en https://github.com/espressif/esp-idf/blob/v5.5.1/examples/bluetooth/bluedroid/ble/gatt_server/main/gatts_demo.c

// Tutorial https://github.com/espressif/esp-idf/blob/v5.5.1/examples/bluetooth/bluedroid/ble/gatt_server_service_table/tutorial/Gatt_Server_Service_Table_Example_Walkthrough.md

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_mac.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"

#include "nebulaedge_defs.h"
#include "nebulaedge_ble.h"

#define GATTS_TABLE_TAG             "nebulaedge_ble"
#define PROFILE_NUM                 1                   // Número de perfiles (apps)
#define PROFILE_APP_IDX             0                   // Índice de la única App
#define ESP_APP_ID                  0x55                // ID de la única App
#define SAMPLE_DEVICE_NAME          "ESP_NEBULAEDGE"    // Nombre del dispositivo
#define SVC_INST_ID                 0

/* The max length of characteristic value. When the GATT client performs a write or prepare write operation,
*  the data length must be less than GATTS_DEMO_CHAR_VAL_LEN_MAX. */
#define GATTS_DEMO_CHAR_VAL_LEN_MAX 500
#define PREPARE_BUF_MAX_SIZE        1024
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))

#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

// static EventGroupHandle_t s_ble_event_group = NULL;
extern SemaphoreHandle_t semaphore;

// Este script implementa una Application Profile. El APP Profile ID, que es un número
// asignado por el usuario para identificar cada perfil, se usa para registrar el perfil
// en el stack, en este script es 0x55

enum {
    // Índice de servicio
    IDX_SVC,

    // Índices de característica A
    IDX_CHAR_A,       // Índice
    IDX_CHAR_VAL_A,   // Valor en el índice

    // Índices de característica B
    IDX_CHAR_B,       // Índice
    IDX_CHAR_VAL_B,   // Valor en el índice

    // Índices de característica C
    IDX_CHAR_C,       // Índice
    IDX_CHAR_VAL_C,   // Valor en el índice

    // Número de elementos en la tabla
    HRS_IDX_NB,
};

static uint8_t adv_config_done = 0;

uint16_t ble_handle_table[HRS_IDX_NB];

static uint8_t service_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

/* Length of adv data must be less than 31 bytes */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp    = false,
    .include_name    = true,
    .include_txpower = true,
    .service_uuid_len= sizeof(service_uuid),
    .p_service_uuid  = service_uuid,
    .flag            = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// Scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp    = true,
    .include_name    = true,
    .include_txpower = true,
    .service_uuid_len= sizeof(service_uuid),
    .p_service_uuid  = service_uuid,
    .flag            = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min         = 0x20,
    .adv_int_max         = 0x40,
    .adv_type            = ADV_TYPE_IND,
    .own_addr_type       = BLE_ADDR_TYPE_PUBLIC,
    .channel_map         = ADV_CHNL_ALL,
    .adv_filter_policy   = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event,
					esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

// Aquí se guardan los perfiles de aplicación. En este caso hay solo un 
// perfil (una app). Un solo elemento es guardado en el índice cero como el 
// perfil PROFILE_APP_IDX.
static struct gatts_profile_inst profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_IDX] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

/* Service */
static const uint16_t GATTS_SERVICE_UUID_A      = 0x00FF;
static const uint16_t GATTS_CHAR_UUID_A       = 0xFF01;
static const uint16_t GATTS_CHAR_UUID_B       = 0xFF02;
static const uint16_t GATTS_CHAR_UUID_C       = 0xFF03;

static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;

static const uint8_t char_prop_read                =  ESP_GATT_CHAR_PROP_BIT_READ;
// static const uint8_t char_prop_write               = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t char_prop_read_write   = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t char_value[4]                 = {0x11, 0x22, 0x33, 0x44};

/* Full Database Description - Used to add attributes into the database */
static const esp_gatts_attr_db_t gatt_db[HRS_IDX_NB] = {
    
    /* Service Declaration */
    [IDX_SVC]        =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(GATTS_SERVICE_UUID_A), (uint8_t *)&GATTS_SERVICE_UUID_A}},

    // Char A (RW)
    [IDX_CHAR_A]     =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    [IDX_CHAR_VAL_A] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_A, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // Char B (R)
    [IDX_CHAR_B]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read}},

    [IDX_CHAR_VAL_B]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_B, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},

    // Char C (RW)
    [IDX_CHAR_C]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    [IDX_CHAR_VAL_C]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&GATTS_CHAR_UUID_C, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_DEMO_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {

        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~ADV_CONFIG_FLAG);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;

        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            /* advertising start complete event to indicate advertising start successfully or failed */
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(GATTS_TABLE_TAG, "advertising start failed");
            }else{
                ESP_LOGI(GATTS_TABLE_TAG, "advertising start successfully");
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(GATTS_TABLE_TAG, "Advertising stop failed");
            }
            else {
                ESP_LOGI(GATTS_TABLE_TAG, "Stop adv successfully");
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "update connection params status = %d, conn_int = %d, latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
            break;
        default:
            break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {

    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        } else {
            ESP_LOGE(GATTS_TABLE_TAG, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
            if (gatts_if == ESP_GATT_IF_NONE || gatts_if == profile_tab[idx].gatts_if) {
                if (profile_tab[idx].gatts_cb) {
                    profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

// Handler de la aplicación 1
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:{
            // Set device name
            esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(SAMPLE_DEVICE_NAME);
            if (set_dev_name_ret){
                ESP_LOGE(GATTS_TABLE_TAG, "set device name failed, error code = %x", set_dev_name_ret);
            }

            // Config adv data
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret){
                ESP_LOGE(GATTS_TABLE_TAG, "config adv data failed, error code = %x", ret);
            }

            adv_config_done |= ADV_CONFIG_FLAG;
            // Config scan response data
            ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
            if (ret){
                ESP_LOGE(GATTS_TABLE_TAG, "config scan response data failed, error code = %x", ret);
            }
            adv_config_done |= SCAN_RSP_CONFIG_FLAG;

            esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HRS_IDX_NB, SVC_INST_ID);
            if (create_attr_ret){
                ESP_LOGE(GATTS_TABLE_TAG, "create attr table failed, error code = %x", create_attr_ret);
            }
        }
       	    break;

        // Evento de lectura (se es leída una característica)
        case ESP_GATTS_READ_EVT:

            uint16_t handle = param->read.handle;
            uint16_t offset = param->read.offset;

            // Disparar acción solo en la característica B
            if (handle == ble_handle_table[IDX_CHAR_VAL_B]) {
        
                // Una sola vez por lectura "larga"
                if (offset == 0) { 
                    ESP_LOGI(GATTS_TABLE_TAG, "GATT_READ_EVT, se lee característica B");
                }
            }

            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_READ_EVT");
       	    break;

        // Evento de escritura (se escribe sobre el ESP)
        case ESP_GATTS_WRITE_EVT:

            // Log de evento de escritura
            ESP_LOGI(
                GATTS_TABLE_TAG, 
                "GATT_WRITE_EVT, handle = %d, value len = %d, value :", 
                param->write.handle, 
                param->write.len
            );

            /* send response when param->write.need_rsp is true*/
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }

            // // Disparar acción SOLO cuando se escribe cualquier cosa
            // // en la característica A
            // if (param->write.handle == ble_handle_table[IDX_CHAR_VAL_A]) {
            //     ESP_LOGI(GATTS_TABLE_TAG, "Cliente escribió en característica A, se cede semáforo.");
            //     if (semaphore != NULL) {
            //         xSemaphoreGive(semaphore);
            //     }
            // }

            // Disparar acción SOLO cuando se escribe cualquier cosa en la char C
            if (param->write.handle == ble_handle_table[IDX_CHAR_VAL_C]) {
                ESP_LOGI(GATTS_TABLE_TAG, "Cliente escribió en característica C, se cede semáforo.");
                if (semaphore != NULL) {
                    xSemaphoreGive(semaphore);
                }
            }

      	    break;

        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
            break;
        case ESP_GATTS_CONF_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONF_EVT, status = %d, attr_handle %d", param->conf.status, param->conf.handle);
            break;
        case ESP_GATTS_START_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "SERVICE_START_EVT, status %d, service_handle %d", param->start.status, param->start.service_handle);
            break;



        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_CONNECT_EVT, conn_id = %d", param->connect.conn_id);
            ESP_LOG_BUFFER_HEX(GATTS_TABLE_TAG, param->connect.remote_bda, 6);
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            /* For the iOS system, please refer to Apple official documents about the BLE connection parameters restrictions. */
            conn_params.latency = 0;
            conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
            //start sent the update connection parameters to the peer device.
            esp_ble_gap_update_conn_params(&conn_params);
            break;



        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
            esp_ble_gap_start_advertising(&adv_params);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:{
            if (param->add_attr_tab.status != ESP_GATT_OK){
                ESP_LOGE(GATTS_TABLE_TAG, "create attribute table failed, error code=0x%x", param->add_attr_tab.status);
            }
            else if (param->add_attr_tab.num_handle != HRS_IDX_NB){
                ESP_LOGE(GATTS_TABLE_TAG, "create attribute table abnormally, num_handle (%d) \
                        doesn't equal to HRS_IDX_NB(%d)", param->add_attr_tab.num_handle, HRS_IDX_NB);
            }
            else {
                ESP_LOGI(GATTS_TABLE_TAG, "create attribute table successfully, the number handle = %d",param->add_attr_tab.num_handle);
                memcpy(ble_handle_table, param->add_attr_tab.handles, sizeof(ble_handle_table));
                esp_ble_gatts_start_service(ble_handle_table[IDX_SVC]);
            }
            break;
        }
        case ESP_GATTS_STOP_EVT:
        case ESP_GATTS_OPEN_EVT:
        case ESP_GATTS_CANCEL_OPEN_EVT:
        case ESP_GATTS_CLOSE_EVT:
        case ESP_GATTS_LISTEN_EVT:
        case ESP_GATTS_CONGEST_EVT:
        case ESP_GATTS_UNREG_EVT:
        case ESP_GATTS_DELETE_EVT:
        default:
            break;
    }
}


// Asigna el valor a una característica dada por su handle
esp_err_t set_characteristic_value(uint16_t char_handle, const uint8_t *value, uint16_t length) {
    esp_err_t status = esp_ble_gatts_set_attr_value(char_handle, length, value);

    if (status == ESP_GATT_OK) {
        printf("Characteristic value set successfully.");
    } else {
        printf("Failed to set characteristic value, error: %d", status);
    }

    return status;
}

// Cambia el el valor de la característica B. Retorna ESP_OK
// si es escrito sin problemas.
esp_err_t set_char_b(const uint8_t *value, uint16_t length) {
    esp_err_t status = esp_ble_gatts_set_attr_value(ble_handle_table[IDX_CHAR_VAL_B], length, value);
    if (status == ESP_OK) {
        ESP_LOGI(GATTS_TABLE_TAG, "set_char_b: char B actualizado (%u bytes)", length);
    } 
    else {
        ESP_LOGE(GATTS_TABLE_TAG, "set_char_b: falló: %s", esp_err_to_name(status));
    }
    return status;
}

size_t get_char_a(uint8_t *out, size_t buffer_len) {
    uint16_t len = 0;
    const uint8_t *value = NULL;
    esp_err_t ret = esp_ble_gatts_get_attr_value(ble_handle_table[IDX_CHAR_VAL_A], &len, &value);
    if (ret != ESP_OK || !value) {
        ESP_LOGE(GATTS_TABLE_TAG, "get_char_a, error leyendo char A: %s", esp_err_to_name(ret));
        return 0;
    }
    size_t copy_len = (len > buffer_len) ? buffer_len : len;
    memcpy(out, value, copy_len);
    ESP_LOGI(GATTS_TABLE_TAG, "get_char_a: leídos %u bytes de char A(handle=%u)", (unsigned)copy_len, ble_handle_table[IDX_CHAR_VAL_A]);
    return copy_len;
}

void ble_init(void) {
    
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&cfg);
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TABLE_TAG, "gatts register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TABLE_TAG, "gap register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TABLE_TAG, "gatts app register error, error code = %x", ret);
        return;
    }

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret) {
        ESP_LOGE(GATTS_TABLE_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }
}