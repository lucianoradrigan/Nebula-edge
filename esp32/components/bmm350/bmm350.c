// Código basado en la API oficial de Bosch
// https://github.com/boschsensortec/BMM350_SensorAPI

#include "nebulaedge_defs.h"
#include "nebulaedge_i2c.h"
#include "schema.pb-c.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_task.h"
#include "math.h"
#include "sdkconfig.h"

/* -------------------- Variables ------------------------ */
/* Frecuencia de toma de datos. Puede tomar los valores: 400, 200, 100,
 * 50, 25, 12.5, 6.25, 3.125 y 1.5625. */
#define ODR                                 ODR_400

/* Promedio entre muestras. Puede ser 0, 2, 4 u 8. OJO: hay combinaciones 
 * de AVG y ODR NO VÁLIDAS (ver datasheet BMM350). */
#define AVG                                 4
#define BMM350_INIT_RETRIES                 3

static const char *TAG = "bmm350";
static esp_err_t ret;
static bool is_bmm350_active = false;

/* Arreglo para almacenar la memoria otp al comienzo de cada boot. */
static uint16_t data_otp[BMM350_OTP_DATA_LENGTH];

/* Arreglos para almacenar datos de compensación magnéticos y temperatura.
 * Data procesada de data_otp. */
static float offset[4];
static float sens[4];
static float tco[3];
static float tcs[3];
static float dut_t0[1];
static float cross[4];
static uint8_t sensor_data_buffer[64];

/* Factores de conversión de datos crudos. Fuente: función update_default_coefiecents() 
 * de script bmm350.c API Bosch */
static const float mag_x_factor = ((float)(1000000.0 / 1048576.0) / (14.55f * 19.46f * (1 / 1.5f) * 0.714607238769531f));
static const float mag_y_factor = ((float)(1000000.0 / 1048576.0) / (14.55f * 19.46f * (1 / 1.5f) * 0.714607238769531f));
static const float mag_z_factor = ((float)(1000000.0 / 1048576.0) / (9.0f * 31.0 * (1 / 1.5f) * 0.714607238769531f));
static const float temp_factor = (1 / (0.00204f * (1 / 1.5f) * 0.714607238769531f * 1048576));

/* Función que lee el Chip ID del BMM350. Sirve para comprobar que funciona bien
 * la comuncicación con el sensor. Realiza reintentos en caso de no devolver el
 * valor esperado: 0x33. 
 * 
 * OBSERVACIÓN 1: Al hacer la lectura en chip_id de 1 solo byte el sensor retornó
 * 0x00 todas las ejecuciones. Es por eso que la lectura se hizo de más de 1 byte,
 * esto solucionaba el problema.
 * OBSERVACIÓN 2: El valor chip_id está en 0x02, no en 0x00. Se ha observado que 
 * este offset solo aplica en la lectura. */
static esp_err_t chipid(void) {
    // Inicio del rango de registros
    uint8_t reg = 0x00;

    // Registro chip_id. Offset +2
    uint8_t reg_chip_id = 0x02;
    
    // Cantidad de registros a leer
    uint8_t data[4];

    for (int attempt = 1; attempt <= BMM350_INIT_RETRIES; ++attempt) {
        ret = device_read(device_bmm350, &reg, data, sizeof(data), TAG);
        
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Intento %d/%d de lectura CHIPID falló: %s", attempt, BMM350_INIT_RETRIES, esp_err_to_name(ret));
            vTaskDelay((attempt * 100) / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Valor de CHIPID: 0x%02X", data[reg_chip_id]);

        if (data[2] == 0x33) {
            ESP_LOGI(TAG, "Chip reconocido exitosamente");
            is_bmm350_active = true;
            return ESP_OK;
        } 
        else {
            ESP_LOGW(TAG, "Intento %d/%d. Chip no reconocido. CHIP ID: 0x%02X", attempt, BMM350_INIT_RETRIES, data[reg_chip_id]);
            vTaskDelay((attempt * 100) / portTICK_PERIOD_MS);
            continue;
        }
    }

    ESP_LOGE(TAG, "Verificación CHIPID agotó reintentos");
    return ESP_ERR_INVALID_RESPONSE;
}

/* Reinicia el BMM350. Realiza varios reintentos en caso de fallar. */
static esp_err_t softreset(void) {
    uint8_t reg_softreset = 0x7E;
    uint8_t val_softreset_1 = 0xB6;
    uint8_t val_softreset_2 = 0x00;

    for (int attempt = 1; attempt <= BMM350_INIT_RETRIES; ++attempt) {
        ret = device_write(device_bmm350, &reg_softreset, &val_softreset_1, 1, TAG);
        vTaskDelay(100 / portTICK_PERIOD_MS);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Intento %d/%d softreset fallido: %s", attempt, BMM350_INIT_RETRIES, esp_err_to_name(ret));
            continue;
        }

        ret = device_write(device_bmm350, &reg_softreset, &val_softreset_2, 1, TAG);
        vTaskDelay(300 / portTICK_PERIOD_MS);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Intento %d/%d softreset fallido: %s", attempt, BMM350_INIT_RETRIES, esp_err_to_name(ret));
            continue;
        }

        // Esperar tras el reset exitoso
        ESP_LOGI(TAG, "Reset exitoso");
        vTaskDelay(pdMS_TO_TICKS(500));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Softreset BMM350 agotó reintentos");
    return ret;
}

/* Se copia la memoria OTP del sensor. Estos datos corresponden a compensación
 * magnética y temperatura. */
static void download_otp(void) {
    uint8_t reg_otp_cmd = 0x50;
    uint8_t val_otp_cmd_read = 0x20;
    uint8_t val_otp_cmd_end = 0x80;
    uint8_t val_otp_cmd;
    uint8_t reg = 0x00; 
    uint8_t data[100];

    ESP_LOGI(TAG, "Copiando memoria OTP... Esto puede demorar.");
    for (uint8_t i = 0; i < BMM350_OTP_DATA_LENGTH; i++) {

        // Data address
        val_otp_cmd = val_otp_cmd_read | (i & 0x1F);

        // Set OTP command
        ret = device_write(device_bmm350, &reg_otp_cmd, &val_otp_cmd, 1, TAG);

        // Delay en específico que ralentiza el programa.
        // En la API Bosch estaba en 300 pero se puede bajar.
        vTaskDelay(24 / portTICK_PERIOD_MS);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error al escribir datos reg_otp_cmd: %s, valor %d", esp_err_to_name(ret), val_otp_cmd);
        }

        // Get OTP data
        device_read(device_bmm350, &reg, data, sizeof(data), TAG);
        vTaskDelay(24 / portTICK_PERIOD_MS);
        
        /* Valor de comprobación. Debería dar 0x33. */
        // printf("valor de comprobación reg 0x00: 0x%02X\n", data[2]);

        // Store data
        data_otp[i] = ((uint16_t)(data[84] << 8) | data[85]) & 0xFFFF;  // Offset +2
    }

    // The boot phase must be terminated by writing 0x80 
    // to OTP_CMD_REG (also done in BMM350_init).
    ret = device_write(device_bmm350, &reg_otp_cmd, &val_otp_cmd_end, 1, TAG);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error en inicializacion: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Algoritmo de inicializacion finalizado.");
}

/* Convierte un unsigned int a signed int. */
static int32_t fix_sign(uint32_t inval, int8_t number_of_bits) {
    int32_t power = 0;
    int32_t retval;

    switch (number_of_bits) {
        case 8:
            power = 128; /* 2^7 */
            break;

        case 12:
            power = 2048; /* 2^11 */
            break;

        case 16:
            power = 32768; /* 2^15 */
            break;

        case 21:
            power = 1048576; /* 2^20 */
            break;

        case 24:
            power = 8388608; /* 2^23 */
            break;

        default:
            power = 0;
            break;
    }

    retval = (int32_t)inval;

    if (retval >= power) {
        retval = retval - (power * 2);
    }

    return retval;
}

/* Realiza cálculos de compensación de datos magnéticos y temperatura
 * con los datos brutos de la memoria OTP. Esto se hace una vez por cada boot del sensor. 
 * Datos de compensación: Offset - Sensibility - TCO - TCS - Cross.
 * 
 * Fuente: función update_mag_off_sens() script bmm350.c API Bosch. */
static void process_otp_data(void) {
    uint16_t off_x_lsb_msb, off_y_lsb_msb, off_z_lsb_msb, t_off;
    uint8_t sens_x, sens_y, sens_z, t_sens;
    uint8_t tco_x, tco_y, tco_z;
    uint8_t tcs_x, tcs_y, tcs_z;
    uint8_t cross_x_y, cross_y_x, cross_z_x, cross_z_y;

    // Offset data magnetometer and temp
    off_x_lsb_msb = data_otp[0x0E] & 0x0FFF;
    off_y_lsb_msb = ((data_otp[0x0E] & 0xF000) >> 4) + (data_otp[0x0F] & 0x00FF);
    off_z_lsb_msb = (data_otp[0x0F] & 0x0F00) + (data_otp[0x10] & 0x00FF);
    t_off = data_otp[0x0D] & 0x00FF;

    offset[0] = fix_sign(off_x_lsb_msb, 12);
    offset[1] = fix_sign(off_y_lsb_msb, 12);
    offset[2] = fix_sign(off_z_lsb_msb, 12);
    offset[3] = fix_sign(t_off, 8) / 5.0f;

    // Sensibility data
    sens_x = (data_otp[0x10] & 0xFF00) >> 8;
    sens_y = (data_otp[0x11] & 0x00FF);
    sens_z = (data_otp[0x11] & 0xFF00) >> 8;
    t_sens = (data_otp[0x0D] & 0xFF00) >> 8;

    sens[0] = fix_sign(sens_x, 8) / 256.0f;
    sens[1] = (fix_sign(sens_y, 8) / 256.0f) + 0.01f;
    sens[2] = fix_sign(sens_z, 8) / 256.0f;
    sens[3] = fix_sign(t_sens, 8) / 512.0f;

    // TCO data
    tco_x = (data_otp[0x12] & 0x00FF);
    tco_y = (data_otp[0x13] & 0x00FF);
    tco_z = (data_otp[0x14] & 0x00FF);

    tco[0] = fix_sign(tco_x, 8) / 32.0f;
    tco[1] = fix_sign(tco_y, 8) / 32.0f;
    tco[2] = fix_sign(tco_z, 8) / 32.0f;

    // TCS data
    tcs_x = (data_otp[0x12] & 0xFF00) >> 8;
    tcs_y = (data_otp[0x13] & 0xFF00) >> 8;
    tcs_z = (data_otp[0x14] & 0xFF00) >> 8;

    tcs[0] = fix_sign(tcs_x, 8) / 16384.0f;
    tcs[1] = fix_sign(tcs_y, 8) / 16384.0f;
    tcs[2] = (fix_sign(tcs_z, 8) / 16384.0f) - 0.0001f;

    // Dut T data
    dut_t0[0] = (fix_sign(data_otp[0x18], 16) / 512.0f) + 23.0f;

    // Cross data
    cross_x_y = (data_otp[0x15] & 0x00FF);
    cross_y_x = (data_otp[0x15] & 0xFF00) >> 8;
    cross_z_x = (data_otp[0x16] & 0x00FF);
    cross_z_y = (data_otp[0x16] & 0xFF00) >> 8;

    cross[0] = fix_sign(cross_x_y, 8) / 800.0f; // XY
    cross[1] = fix_sign(cross_y_x, 8) / 800.0f; // YX
    cross[2] = fix_sign(cross_z_x, 8) / 800.0f; // ZX
    cross[3] = fix_sign(cross_z_y, 8) / 800.0f; // ZY
}

/* Asigna variables ODR y AVG del sensor según lo definido  
 * al inicio de este archivo. */
static void odr_avg_config(int odr_set, int avg_set) {
    uint8_t reg_pmu_cmd = 0x06;
    uint8_t val_pmu_cmd_upd_aoe = 0x02;

    uint8_t reg_pmu_cmd_aggr_set = 0x04;
    uint8_t val_pmu_cmd_aggr_set;
    uint8_t odr, avg; 
    
    switch (odr_set) {
        case ODR_1_5625:
            odr = 0x0A;
            break;
        case ODR_3_125:
            odr = 0x09;
            break;
        case ODR_6_25:
            odr = 0x08;
            break;
        case ODR_12_5:
            odr = 0x07;
            break;
        case ODR_25:
            odr = 0x06;
            break;
        case ODR_50:
            odr = 0x05;
            break;
        case ODR_100:
            odr = 0x04;
            break;
        case ODR_200:
            odr = 0x03;
            break;
        case ODR_400:
            odr = 0x02;
            break;
        default:
            ESP_LOGE(TAG, "FRECUENCIA DE MUESTREO BMM350 INCORRECTO.");
            exit(EXIT_SUCCESS);
    }
    switch (avg_set) {
        case 0:
            avg = 0x00;
            break;
        case 2:
            avg = 0x01;
            break;
        case 4:
            avg = 0x02;
            break;
        case 8:
            avg = 0x03;
            break;
        default:
            ESP_LOGE(TAG, "PROMEDIO DE MUESTRAS BMM350 INCORRECTO.");
            exit(EXIT_SUCCESS);
    }
    val_pmu_cmd_aggr_set = (avg << 4) | odr;

    device_write(device_bmm350, &reg_pmu_cmd, &val_pmu_cmd_upd_aoe, 1, TAG);
    vTaskDelay(24 / portTICK_PERIOD_MS);

    device_write(device_bmm350, &reg_pmu_cmd_aggr_set, &val_pmu_cmd_aggr_set, 1, TAG);
    vTaskDelay(24 / portTICK_PERIOD_MS);   
}

/* Pone en modo normal al BMM350 y activa el data ready interrupt. */
static void bmmpowermode(void) {
    uint8_t reg_pmu_cmd = 0x06;
    uint8_t val_pmu_cmd = 0x01;
    uint8_t reg_int_ctrl = 0x2E;
    uint8_t val_int_ctrl = 0x88;

    // Normal mode
    device_write(device_bmm350, &reg_pmu_cmd, &val_pmu_cmd, 1, TAG);
    vTaskDelay(pdMS_TO_TICKS(24));

    // Data ready on
    device_write(device_bmm350, &reg_int_ctrl, &val_int_ctrl, 1, TAG);
    vTaskDelay(pdMS_TO_TICKS(24));
}

/* Imprime los valores de reg_pmu_cmd_status_0 y reg_pmu_cmd_status_1 
 * Sirve para corroborar si el AVG y ODR se asignaron bien y/o para saber
 * si hay un comando inválido. */
static void internal_status(void) {
    // Registro inicial
    uint8_t reg = 0x00;

    // Registros pmu_cmd_status. Offset +2.
    uint8_t reg_pmu_cmd_status_0 = 0x09; 
    uint8_t reg_pmu_cmd_status_1 = 0x0A; 

    // Datos a leer 
    uint8_t data[12];

    device_read(device_bmm350, &reg, data, sizeof(data), TAG);

    ESP_LOGI(TAG, "PMU CMD Status 0: 0x%02X", (data[reg_pmu_cmd_status_0] & 0b00011111));
    ESP_LOGI(TAG, "PMU CMD Status 1: 0x%02X", (data[reg_pmu_cmd_status_1] & 0b00111111));
}

/* Extrae datos magnéticos y de temperatura del sensor BMM350, los procesa 
 * e imprime en la salida estándar. */           
void readout_data_bmm350(Data1 *data) {

    if (!is_bmm350_active) {
        ESP_LOGW(TAG, "BMM350 no activo, se omite lectura");
        return;
    }

    // Registro inicial
    uint8_t reg = 0x00;
                    
    // Donde empiezan los valores magnéticos. Offset +2
    uint8_t data_reg = 0x33;
    
    // Cantidad de bytes a leer
    uint8_t data_bytes = 12;
    
    // Valores magnéticos brutos
    uint32_t raw_mag_x, raw_mag_y, raw_mag_z, raw_temp;   
    
    // Valores convertidos a enteros con signo
    float out_data[4] = { 0.0f };

    // Lectura en el sensor
    device_read(device_bmm350, &reg, sensor_data_buffer, sizeof(sensor_data_buffer), TAG);

    // Data ready condition
    if ((sensor_data_buffer[50] & 0b00000100) == 4) {

        // Read data
        ret = device_read(device_bmm350, &data_reg, (uint8_t*) sensor_data_buffer, data_bytes, TAG);
        
        raw_mag_x = ((uint32_t) sensor_data_buffer[2] << 16) + ((uint32_t) sensor_data_buffer[1] << 8) + (uint32_t) sensor_data_buffer[0];
        raw_mag_y = ((uint32_t) sensor_data_buffer[5] << 16) + ((uint32_t) sensor_data_buffer[4] << 8) + (uint32_t) sensor_data_buffer[3];
        raw_mag_z = ((uint32_t) sensor_data_buffer[8] << 16) + ((uint32_t) sensor_data_buffer[7] << 8) + (uint32_t) sensor_data_buffer[6];
        raw_temp = ((uint32_t) sensor_data_buffer[11] << 16) + ((uint32_t) sensor_data_buffer[10] << 8) + (uint32_t) sensor_data_buffer[9];

        // Fix sign and apply factor
        out_data[0] = fix_sign(raw_mag_x, 24) * mag_x_factor;
        out_data[1] = fix_sign(raw_mag_y, 24) * mag_y_factor;
        out_data[2] = fix_sign(raw_mag_z, 24) * mag_z_factor;
        out_data[3] = fix_sign(raw_temp, 24) * temp_factor;

        // Fix temp (alineado con API oficial)
        out_data[3] = out_data[3] - 25.49f;

        // Compensación de datos magnéticos
        for (uint8_t i = 0; i < 3; i++) {
            out_data[i] *= 1 + sens[i];
            out_data[i] += offset[i];
            out_data[i] += tco[i] * (out_data[3] - dut_t0[0]);
            out_data[i] /= 1 + tcs[i] * (out_data[3] - dut_t0[0]);
        }

        float cross_denom = 1.0f - (cross[1] * cross[0]);
        float cr_ax_comp_x = (out_data[0] - (cross[0] * out_data[1])) / cross_denom;
        float cr_ax_comp_y = (out_data[1] - (cross[1] * out_data[0])) / cross_denom;
        float cr_ax_comp_z = out_data[2] + ((out_data[0] * ((cross[1] * cross[3]) - cross[2])) -
                      (out_data[1] * (cross[3] - (cross[0] * cross[2])))) / cross_denom;

        out_data[0] = cr_ax_comp_x;
        out_data[1] = cr_ax_comp_y;
        out_data[2] = cr_ax_comp_z;

        // Compensación temperatura
        out_data[3] = (1 + sens[3]) * out_data[3] + offset[3];

        ESP_LOGI(TAG, "mag_x: %.6f uT    mag_y: %.6f uT    mag_z: %.6f uT", out_data[0], out_data[1], out_data[2]);
        ESP_LOGI(TAG, "temp: %.6f °C", out_data[3]);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error lectura: %s", esp_err_to_name(ret));
        }

        if (data != NULL) {
            // Guarda las medidas en protobuf. Se guardan datos
            // magnéticos y no de temperatura.
            data->mag_x = out_data[0];
            data->mag_y = out_data[1];
            data->mag_z = out_data[2];
        }
    }
}

// /* Funcion para debugging: lee un rango de registros. */
// static void read_register_range(void) {
//     /* Inicio del rango de registros */
//     uint8_t reg = 0x00;

//     /* Cantidad de registros a leer */
//     uint8_t data[130];   
//     ret = device_read(device_bmm350, &reg, data, sizeof(data), TAG);

//     if (ret == ESP_OK) {
//         ESP_LOGI(TAG, "Datos leidos desde el sensor:");
//         for (int i = 0; i < sizeof(data); i++) {
//             /* Offset -2 */
//             ESP_LOGI(TAG, "Registro 0x%02X: 0x%02X", reg + i - 2, data[i]);
//         }
//     } else {
//         ESP_LOGE(TAG, "Error leyendo múltiples registros: %s", esp_err_to_name(ret));
//     }
// }

// /* Funcion para debugging: imprime los valores de compensación 
//  * que ya han sido procesados. */
// static void print_otp_data(void) {
//     ESP_LOGI(TAG, "OTP stored data");
//     for (uint8_t i=0; i<BMM350_OTP_DATA_LENGTH; i++) {
//         ESP_LOGI(TAG, "%d", data_otp[i]);
//     }

//     ESP_LOGI(TAG, "Offset stored data");
//     for (uint8_t i=0; i<4; i++) {
//         ESP_LOGI(TAG, "%f", offset[i]);
//     }

//     ESP_LOGI(TAG, "Sens stored data");
//     for (uint8_t i=0; i<4; i++) {
//         ESP_LOGI(TAG, "%f", sens[i]);
//     }

//     ESP_LOGI(TAG, "TCO stored data");
//     for (uint8_t i=0; i<3; i++) {
//         ESP_LOGI(TAG, "%f", tco[i]);
//     }

//     ESP_LOGI(TAG, "TCS stored data");
//     for (uint8_t i=0; i<3; i++) {
//         ESP_LOGI(TAG, "%f", tcs[i]);
//     }

//     ESP_LOGI(TAG, "Cross stored data");
//     for (uint8_t i=0; i<4; i++) {
//         ESP_LOGI(TAG, "%f", cross[i]);
//     }
// }

/* Función para ser llamada desde el script main. Contiene llamados a todas las
 * funciones que se encargan de inicializar el sensor. */
void bmm350_init(int odr, int avg) {
    is_bmm350_active = false;

    ret = softreset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init abortada: softreset falló (%s)", esp_err_to_name(ret));
        return;
    }

    ret = chipid();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init abortada: CHIPID inválido (%s)", esp_err_to_name(ret));
        return;
    }

    download_otp();
    process_otp_data();
    odr_avg_config(odr, avg);
    bmmpowermode();    
    internal_status();
}

// Main para ocupar este archivo por separado.
// void app_main(void) {
//     ESP_ERROR_CHECK(i2c_master_init(&bus_handle));
//     ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bmm350, BMM350_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));
//     softreset();
//     chipid();
//     download_otp();
//     process_otp_data();
//     odr_avg_config(ODR, AVG);
//     bmmpowermode();    
//     internal_status();
//     printf("Comienza lectura\n\n");
//     readout_data_bmm350(NULL, true, true);
// }