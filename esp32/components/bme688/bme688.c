#include "nebulaedge_defs.h"
#include "nebulaedge_microsd.h"
#include "schema.pb-c.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "nebulaedge_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bme688.h"

static int32_t t_fine;
static int temp_ovs_global, press_ovs_global, hum_ovs_global;

static const char *TAG = "bme688";
static esp_err_t ret;
static bool is_bme688_active = false;
#define BME688_INIT_RETRIES 3

static uint8_t calc_gas_wait(uint16_t dur) {
    // Fuente: BME688 API
    // https://github.com/boschsensortec/BME68x_SensorAPI/blob/master/bme68x.c#L1176
    uint8_t factor = 0;
    uint8_t durval;

    if (dur >= 0xfc0) {
        durval = 0xff; /* Max duration*/
    } 
    else {
        while (dur > 0x3F) {
            dur = dur >> 2;
            factor += 1;
        }

        durval = (uint8_t)(dur + (factor * 64));
    }

    return durval;
}

static uint8_t calc_res_heat(uint16_t temp) {
    // Fuente: BME688 API
    // https://github.com/boschsensortec/BME68x_SensorAPI/blob/master/bme68x.c#L1145
    uint8_t heatr_res;
    int8_t amb_temp = 25;

    uint8_t reg_par_g1 = 0xED;
    uint8_t par_g1_u8;
    int8_t par_g1;
    device_read(device_bme688, &reg_par_g1, &par_g1_u8, 1, TAG);
    par_g1 = (int8_t)par_g1_u8;

    uint8_t reg_par_g2_lsb = 0xEB;
    uint8_t par_g2_lsb;
    device_read(device_bme688, &reg_par_g2_lsb, &par_g2_lsb, 1, TAG);

    uint8_t reg_par_g2_msb = 0xEC;
    uint8_t par_g2_msb;
    device_read(device_bme688, &reg_par_g2_msb, &par_g2_msb, 1, TAG);

    int16_t par_g2 = (int16_t)(CONCAT_BYTES(par_g2_msb, par_g2_lsb));
    uint8_t reg_par_g3 = 0xEE;
    uint8_t par_g3_u8;
    int8_t par_g3;
    device_read(device_bme688, &reg_par_g3, &par_g3_u8, 1, TAG);
    par_g3 = (int8_t)par_g3_u8;

    uint8_t reg_res_heat_range = 0x02;
    uint8_t res_heat_range;
    uint8_t mask_res_heat_range = (0x3 << 4);
    uint8_t tmp_res_heat_range;

    uint8_t reg_res_heat_val = 0x00;
    uint8_t res_heat_val_u8;
    int8_t res_heat_val;

    int32_t var1;
    int32_t var2;
    int32_t var3;
    int32_t var4;
    int32_t var5;
    int32_t heatr_res_x100;

    if (temp > 400) {
        temp = 400;
    }

    device_read(device_bme688, &reg_res_heat_range, &tmp_res_heat_range, 1, TAG);
    device_read(device_bme688, &reg_res_heat_val, &res_heat_val_u8, 1, TAG);
    res_heat_val = (int8_t)res_heat_val_u8;
    res_heat_range = (mask_res_heat_range & tmp_res_heat_range) >> 4;

    var1 = (((int32_t)amb_temp * par_g3) / 1000) * 256;
    var2 = (par_g1 + 784) * (((((par_g2 + 154009) * temp * 5) / 100) + 3276800) / 10);
    var3 = var1 + (var2 / 2);
    var4 = (var3 / (res_heat_range + 4));
    var5 = (131 * res_heat_val) + 65536;
    heatr_res_x100 = (int32_t)(((var4 / var5) - 250) * 34);
    heatr_res = (uint8_t)((heatr_res_x100 + 50) / 100);

    return heatr_res;
}

/* Comprueba comunicación con sensor. */
static esp_err_t chipid(void) {
    uint8_t reg_id = 0xd0;
    uint8_t tmp;

    for (int attempt = 1; attempt <= BME688_INIT_RETRIES; ++attempt) {
        ret = device_read(device_bme688, &reg_id, &tmp, 1, TAG);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Intento %d/%d de lectura CHIPID falló: %s", attempt, BME688_INIT_RETRIES, esp_err_to_name(ret));
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "Valor de CHIPID: 0x%02X", tmp);

        if (tmp == 0x61) {
            ESP_LOGI(TAG, "Chip reconocido exitosamente");
            is_bme688_active = true;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Intento %d/%d. Chip BME688 no reconocido. CHIP ID: 0x%02X", attempt, BME688_INIT_RETRIES, tmp);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    ESP_LOGE(TAG, "Verificación CHIPID BME688 agotó reintentos");
    return ESP_ERR_INVALID_RESPONSE;
}

/* Reinicia el BME688. */
static esp_err_t softreset(void) {
    uint8_t reg_softreset = 0xE0; 
    uint8_t val_softreset = 0xB6;

    for (int attempt = 1; attempt <= BME688_INIT_RETRIES; ++attempt) {
        ret = device_write(device_bme688, &reg_softreset, &val_softreset, 1, TAG);
        vTaskDelay(100 / portTICK_PERIOD_MS);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Intento %d/%d softreset fallido: %s", attempt, BME688_INIT_RETRIES, esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "Softreset BME688: OK");
        vTaskDelay(pdMS_TO_TICKS(200));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Softreset BME688 agotó reintentos");
    return ret;
}

/* Se setea el valor de oversampling de temperatura, presión y humedad. 
 * Además, pone al sensor en modo forzado. */
static void set_oversampling_tph(void) {
    int osrs_t_set = temp_ovs_global;
    int osrs_p_set = press_ovs_global;
    int osrs_h_set = hum_ovs_global;
    // Humidity sensor oversampling control
    uint8_t ctrl_hum_reg = 0x72;

    // Temperature and pressure oversampling control
    uint8_t ctrl_meas_reg = 0x74;

    // IIR filter control
    uint8_t config_reg = 0x75;

    // Set heater profile of the sensor
    uint8_t run_gas_reg = 0x71;

    // Target heater resistance
    uint8_t res_heat_0_reg = 0x5A;

    // Time between the beginning of the heat phase
    // and the start of gas sensor resistance measurment
    uint8_t gas_wait_0_reg = 0x64;

    uint8_t osrs_h, osrs_t, osrs_p, osrs_t_p, iir_filter, config_val, run_gas_val, gas_duration, heater_step, ctrl_meas_val;
    uint8_t tmp;

    // Se elige el valor que debe escribirse para oversampling de humedad
    switch (osrs_h_set) {
        case 1:
            osrs_h = 0x01;
            break;
        case 2:
            osrs_h = 0x02;
            break;
        case 4:
            osrs_h = 0x03;
            break;
        case 8:
            osrs_h = 0x04;
            break;
        case 16:
            osrs_h = 0x05;
            break;               
        default:
            ESP_LOGE(TAG, "OVERSAMPLING HUMEDAD BME688 INCORRECTO.");
            exit(EXIT_SUCCESS);
    }

    // Se elige el valor que debe escribirse para oversampling de temperatura
    switch (osrs_t_set) {
        case 1:
            osrs_t = 0x01;
            break;
        case 2:
            osrs_t = 0x02;
            break;
        case 4:
            osrs_t = 0x03;
            break;
        case 8:
            osrs_t = 0x04;
            break;
        case 16:
            osrs_t = 0x05;
            break;               
        default:
            ESP_LOGE(TAG, "OVERSAMPLING TEMPERATURA BME688 INCORRECTO.");
            exit(EXIT_SUCCESS);
    }

    // Se elige el valor que debe escribirse para oversampling de temperatura
    switch (osrs_p_set) {
        case 1:
            osrs_p = 0x01;
            break;
        case 2:
            osrs_p = 0x02;
            break;
        case 4:
            osrs_p = 0x03;
            break;
        case 8:
            osrs_p = 0x04;
            break;
        case 16:
            osrs_p = 0x05;
            break;               
        default:
            ESP_LOGE(TAG, "OVERSAMPLING PRESION BME688 INCORRECTO.");
            exit(EXIT_SUCCESS);
    }

    // Set oversampling humedad. Se lee 0x72 para obtener valor spi_3w_int_en.
    device_read(device_bme688, &ctrl_hum_reg, &tmp, 1, TAG);
    osrs_h = (0xF8 & tmp) | osrs_h;
    device_write(device_bme688, &ctrl_hum_reg, &osrs_h, 1, TAG);

    // Set oversampling temperatura y presión
    osrs_t_p = (osrs_t << 5) | (osrs_p << 2);
    device_write(device_bme688, &ctrl_meas_reg, &osrs_t_p, 1, TAG);

    // Set IIR filter para temperatura. Se lee 0x75 para obtener valor spi_3w_en.
    device_read(device_bme688, &config_reg, &tmp, 1, TAG);
    iir_filter = 0x01;
    config_val = (iir_filter << 3) | (0x03 & tmp);
    device_write(device_bme688, &config_reg, &config_val, 1, TAG);

    // Set gas conversion
    run_gas_val = 0x20;
    device_write(device_bme688, &run_gas_reg, &run_gas_val, 1, TAG);
    
    // Seteamos gas_wait_0_reg a 100ms
    gas_duration = calc_gas_wait(100);
    device_write(device_bme688, &gas_wait_0_reg, &gas_duration, 1, TAG);

    // Seteamos res_heat_0_reg a 300C
    heater_step = calc_res_heat(300);
    device_write(device_bme688, &res_heat_0_reg, &heater_step, 1, TAG);

    // Seteamos el sensor en forced mode
    ctrl_meas_val = osrs_t_p | 0x01;
    device_write(device_bme688, &ctrl_meas_reg, &ctrl_meas_val, 1, TAG);

    // Seteamos el sensor en forced mode
    ctrl_meas_val = osrs_t_p | 0x01;
    device_write(device_bme688, &ctrl_meas_reg, &ctrl_meas_val, 1, TAG);
}

/* This internal API is used to calculate the temperature value. */
static int16_t temp_celsius(uint32_t temp_adc) {
    // Datasheet[23]
    // https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme688-ds000.pdf#page=23

    // Se obtienen los parametros de calibracion
    uint8_t addr_par_t1_lsb = 0xE9, addr_par_t1_msb = 0xEA;
    uint8_t addr_par_t2_lsb = 0x8A, addr_par_t2_msb = 0x8B;
    uint8_t addr_par_t3_lsb = 0x8C;
    uint16_t par_t1;
    int16_t par_t2;
    int8_t par_t3;

    uint8_t par[5];
    device_read(device_bme688, &addr_par_t1_lsb, par, 1, TAG);
    device_read(device_bme688, &addr_par_t1_msb, par + 1, 1, TAG);
    device_read(device_bme688, &addr_par_t2_lsb, par + 2, 1, TAG);
    device_read(device_bme688, &addr_par_t2_msb, par + 3, 1, TAG);
    device_read(device_bme688, &addr_par_t3_lsb, par + 4, 1, TAG);

    par_t1 = (par[1] << 8) | par[0];
    par_t2 = (par[3] << 8) | par[2];
    par_t3 = par[4];

    int64_t var1;
    int64_t var2;
    int64_t var3;
    int16_t calc_temp;

    var1 = ((int32_t)temp_adc >> 3) - ((int32_t)par_t1 << 1);
    var2 = (var1 * (int32_t)par_t2) >> 11;
    var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
    var3 = ((var3) * ((int32_t)par_t3 << 4)) >> 14;
    t_fine = (int32_t)(var2 + var3);
    calc_temp = (int16_t)(((t_fine * 5) + 128) >> 8);
    return calc_temp;
}

/* This internal API is used to calculate the pressure value. */
static uint32_t press_pascal(uint32_t press_adc) {
    // Se obtienen los parametros de calibracion de la presion
    uint8_t addr_par_p1_lsb = 0x8E, addr_par_p1_msb = 0x8F;
    uint8_t addr_par_p2_lsb = 0x90, addr_par_p2_msb = 0x91;
    uint8_t addr_par_p3_lsb = 0x92;
    uint8_t addr_par_p4_lsb = 0x94, addr_par_p4_msb = 0x95;
    uint8_t addr_par_p5_lsb = 0x96, addr_par_p5_msb = 0x97;
    uint8_t addr_par_p6_lsb = 0x99;
    uint8_t addr_par_p7_lsb = 0x98;
    uint8_t addr_par_p8_lsb = 0x9C, addr_par_p8_msb = 0x9D;
    uint8_t addr_par_p9_lsb = 0x9E, addr_par_p9_msb = 0x9F;
    uint8_t addr_par_p10_lsb = 0xA0;

    uint16_t par_p1;
    int16_t par_p2;
    int8_t par_p3;
    int16_t par_p4;
    int16_t par_p5;
    int8_t par_p6;
    int8_t par_p7;
    int16_t par_p8;
    int16_t par_p9;
    uint8_t par_p10;

    uint8_t par_p[16];

    device_read(device_bme688, &addr_par_p1_lsb, par_p, 1, TAG);
    device_read(device_bme688, &addr_par_p1_msb, par_p + 1, 1, TAG);
    device_read(device_bme688, &addr_par_p2_lsb, par_p + 2, 1, TAG);
    device_read(device_bme688, &addr_par_p2_msb, par_p + 3, 1, TAG);
    device_read(device_bme688, &addr_par_p3_lsb, par_p + 4, 1, TAG);
    device_read(device_bme688, &addr_par_p4_lsb, par_p + 5, 1, TAG);
    device_read(device_bme688, &addr_par_p4_msb, par_p + 6, 1, TAG);
    device_read(device_bme688, &addr_par_p5_lsb, par_p + 7, 1, TAG);
    device_read(device_bme688, &addr_par_p5_msb, par_p + 8, 1, TAG);
    device_read(device_bme688, &addr_par_p6_lsb, par_p + 9, 1, TAG);
    device_read(device_bme688, &addr_par_p7_lsb, par_p + 10, 1, TAG);
    device_read(device_bme688, &addr_par_p8_lsb, par_p + 11, 1, TAG);
    device_read(device_bme688, &addr_par_p8_msb, par_p + 12, 1, TAG);
    device_read(device_bme688, &addr_par_p9_lsb, par_p + 13, 1, TAG);
    device_read(device_bme688, &addr_par_p9_msb, par_p + 14, 1, TAG);
    device_read(device_bme688, &addr_par_p10_lsb, par_p + 15, 1, TAG);

    par_p1 = (par_p[1] << 8) | par_p[0];
    par_p2 = (int16_t)((par_p[3] << 8) | par_p[2]);
    par_p3 = (int8_t)par_p[4];
    par_p4 = (int16_t)((par_p[6] << 8) | par_p[5]);
    par_p5 = (int16_t)((par_p[8] << 8) | par_p[7]);
    par_p6 = (int8_t)par_p[9];
    par_p7 = (int8_t)par_p[10];
    par_p8 = (int16_t)((par_p[12] << 8) | par_p[11]);
    par_p9 = (int16_t)((par_p[14] << 8) | par_p[13]);
    par_p10 = (uint8_t)par_p[15];

    // Calculo de la presion
    int64_t var1_p, var2_p, var3_p;
    int32_t calc_press;

    var1_p = ((int32_t)t_fine >> 1) - 64000;
    var2_p = ((((var1_p >> 2) * (var1_p >> 2)) >> 11) * (int32_t)par_p6) >> 2;
    var2_p = var2_p + ((var1_p * (int32_t)par_p5) << 1);
    var2_p = (var2_p >> 2) + ((int32_t)par_p4 << 16);
    var1_p = (((((var1_p >> 2) * (var1_p >> 2)) >> 13) * ((int32_t)par_p3 << 5)) >> 3) + (((int32_t)par_p2 * var1_p) >> 1);

    var1_p = var1_p >> 18;
    var1_p = ((32768 + var1_p) * (int32_t)par_p1) >> 15;
    calc_press = 1048576 - press_adc;
    calc_press = (uint32_t)((calc_press - (var2_p >> 12)) * ((uint32_t)3125));
    if (calc_press >= (1 << 30)) {
        calc_press = ((calc_press / (uint32_t)var1_p) << 1);
    }
    else {
        calc_press = ((calc_press << 1) / (uint32_t)var1_p);
    }
    var1_p = ((int32_t)par_p9 * (int32_t)(((calc_press >> 3) * (calc_press >> 3)) >> 13)) >> 12;
    var2_p = ((int32_t)(calc_press >> 2) * (int32_t)par_p8) >> 13;
    var3_p = ((int32_t)(calc_press >> 8) * (int32_t)(calc_press >> 8) * (int32_t)(calc_press >> 8) * (int32_t)par_p10) >> 17;
    calc_press = (int32_t)(calc_press) + ((var1_p + var2_p + var3_p + ((int32_t)par_p7 << 7)) >> 4);

    return (uint32_t)calc_press;
}

/* This internal API is used to calculate the humidiyy value. */
static uint32_t hum_percent(uint16_t hum_adc) {
    uint8_t addr_par_h1_lsb = 0xE2, addr_par_h1_msb = 0xE3;
    uint8_t addr_par_h2_lsb = 0xE2, addr_par_h2_msb = 0xE1;
    uint8_t addr_par_h3_lsb = 0xE4;
    uint8_t addr_par_h4_lsb = 0xE5;
    uint8_t addr_par_h5_lsb = 0xE6;
    uint8_t addr_par_h6_lsb = 0xE7;
    uint8_t addr_par_h7_lsb = 0xE8;

    uint16_t par_h1, par_h2;
    int8_t par_h3, par_h4, par_h5, par_h7;
    uint8_t par_h6;
    uint8_t par_h[9];

    device_read(device_bme688, &addr_par_h1_lsb, par_h, 1, TAG);
    device_read(device_bme688, &addr_par_h1_msb, par_h + 1, 1, TAG);
    device_read(device_bme688, &addr_par_h2_lsb, par_h + 2, 1, TAG);
    device_read(device_bme688, &addr_par_h2_msb, par_h + 3, 1, TAG);
    device_read(device_bme688, &addr_par_h3_lsb, par_h + 4, 1, TAG);
    device_read(device_bme688, &addr_par_h4_lsb, par_h + 5, 1, TAG);
    device_read(device_bme688, &addr_par_h5_lsb, par_h + 6, 1, TAG);
    device_read(device_bme688, &addr_par_h6_lsb, par_h + 7, 1, TAG);
    device_read(device_bme688, &addr_par_h7_lsb, par_h + 8, 1, TAG);

    // 11110000 = 0xf0
    // 1111 = 0x0f

    par_h1 = (par_h[0] & 0x0F) | (uint16_t)(par_h[1] << 4);
    par_h2 = ((par_h[2] & 0xF0) >> 4) | (uint16_t)(par_h[3] << 4);
    par_h3 = (int8_t)par_h[4];
    par_h4 = (int8_t)par_h[5];
    par_h5 = (int8_t)par_h[6];
    par_h6 = (uint8_t)par_h[7];
    par_h7 = (int8_t)par_h[8];

    int32_t var1, var2, var3, var4, var5, var6;
    int32_t temp_scaled;
    int32_t calc_hum;

    temp_scaled = (((int32_t)t_fine * 5) + 128) >> 8;
    var1 = (int32_t)(hum_adc - ((int32_t)((int32_t)par_h1 * 16))) -
           (((temp_scaled * (int32_t)par_h3) / ((int32_t)100)) >> 1);
    var2 =
        ((int32_t)par_h2 *
         (((temp_scaled * (int32_t)par_h4) / ((int32_t)100)) +
          (((temp_scaled * ((temp_scaled * (int32_t)par_h5) / ((int32_t)100))) >> 6) / ((int32_t)100)) +
          (int32_t)(1 << 14))) >>
        10;
    var3 = var1 * var2;
    var4 = (int32_t)par_h6 << 7;
    var4 = ((var4) + ((temp_scaled * (int32_t)par_h7) / ((int32_t)100))) >> 4;
    var5 = ((var3 >> 14) * (var3 >> 14)) >> 10;
    var6 = (var4 * var5) >> 1;
    calc_hum = (((var3 + var6) >> 10) * ((int32_t)1000)) >> 12;
    if (calc_hum > 100000) /* Cap at 100%rH */ {
        calc_hum = 100000;
    }
    else if (calc_hum < 0) {
        calc_hum = 0;
    }
    return (uint32_t)calc_hum;
}

/* This internal API is used to calculate the gas resistance */
static uint32_t gas_resistance_ohms(uint16_t gas_res_adc, uint8_t gas_range) {
    uint32_t calc_gas_res;
    uint32_t var1 = UINT32_C(262144) >> gas_range;
    int32_t var2 = (int32_t)gas_res_adc - INT32_C(512);

    var2 *= INT32_C(3);
    var2 = INT32_C(4096) + var2;

    /* multiplying 10000 then dividing then multiplying by 100 instead of multiplying by 1000000 to prevent overflow */
    calc_gas_res = (UINT32_C(10000) * var1) / (uint32_t)var2;
    calc_gas_res = calc_gas_res * 100;

    return calc_gas_res;
}

/* Imprime en consola el modo actual del sensor BME688
 * sleep mode - normal mode - forced mode. */
static void get_mode(void) {
    uint8_t reg_mode = 0x74;
    uint8_t tmp;

    ret = device_read(device_bme688, &reg_mode, &tmp, 1, TAG);

    tmp = tmp & 0x3;

    ESP_LOGI(TAG, "Valor de BME MODE: 0x%02X", tmp);
}


/* Extrae datos de temperatura, presión, humedad y resistencia de gas
 * del sensor BME688, los procesa e imprime en la salida estándar. */     
void readout_data_bme688(Environmental *data) {

    if (!is_bme688_active) {
        ESP_LOGW(TAG, "BME688 no activo, se omite lectura");
        return;
    }

    // Datasheet[23:41]
    // https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme688-ds000.pdf#page=23

    uint8_t tmp; 

    // Se obtienen los datos de temperatura
    uint8_t forced_temp_reg[] = {0x22, 0x23, 0x24};
    uint8_t forced_press_reg[] = {0x1F, 0x20, 0x21};
    uint8_t forced_hum_reg[] = {0x25, 0x26};

    uint8_t forced_gas_reg[] = {0x2C, 0x2D};
    uint8_t forced_gas_range_reg[] = {0x2D};

    uint32_t temp_adc = 0;
    uint32_t press_adc = 0;
    uint16_t hum_adc = 0;
    uint32_t gas_adc = 0;
    uint8_t gas_range = 0;

    // Datasheet[41]
    // https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme688-ds000.pdf#page=41
    set_oversampling_tph();

    // Temperature read
    device_read(device_bme688, &forced_temp_reg[0], &tmp, 1, TAG);
    temp_adc = temp_adc | tmp << 12;
    device_read(device_bme688, &forced_temp_reg[1], &tmp, 1, TAG);
    temp_adc = temp_adc | tmp << 4;
    device_read(device_bme688, &forced_temp_reg[2], &tmp, 1, TAG);
    temp_adc = temp_adc | (tmp & 0xF0) >> 4;

    // Pressure read
    device_read(device_bme688, &forced_press_reg[0], &tmp, 1, TAG);
    press_adc = press_adc | tmp << 12;
    device_read(device_bme688, &forced_press_reg[1], &tmp, 1, TAG);
    press_adc = press_adc | tmp << 4;
    device_read(device_bme688, &forced_press_reg[2], &tmp, 1, TAG);
    press_adc = press_adc | (tmp & 0xF0) >> 4;

    // Humidity read
    device_read(device_bme688, &forced_hum_reg[0], &tmp, 1, TAG);
    hum_adc = hum_adc | tmp << 8;
    device_read(device_bme688, &forced_hum_reg[1], &tmp, 1, TAG);
    hum_adc = hum_adc | tmp;

    // Gas read
    device_read(device_bme688, &forced_gas_reg[0], &tmp, 1, TAG);
    gas_adc = gas_adc | tmp << 2;
    device_read(device_bme688, &forced_gas_reg[1], &tmp, 1, TAG);
    gas_adc = gas_adc | tmp >> 6;

    // Gas range read
    device_read(device_bme688, &forced_gas_range_reg[0], &tmp, 1, TAG);
    gas_range = tmp & 0x0F;

    int16_t temp = temp_celsius(temp_adc);
    uint32_t press = press_pascal(press_adc);
    float hum = hum_percent(hum_adc);
    uint32_t gas = gas_resistance_ohms(gas_adc, gas_range);
    
    ESP_LOGI(TAG, "temp_bme688: %f C", (float)temp / 100);
    ESP_LOGI(TAG, "press: %f hPa", (float)press / 100);
    ESP_LOGI(TAG, "hum: %f percent", hum / 1000);
    ESP_LOGI(TAG, "gas: %f Ohms", (float)gas);

    if (data != NULL) {
        // Guarda las medidas en protobuf.
        data->temperature = (float)temp/100;
        data->press = (int32_t)(press / 100);
        data->hum = (int32_t)(hum / 1000);
        data->co = (float)gas;
    }
}

/* Función para ser llamada desde el script main. Contiene llamados a todas las
 * funciones que se encargan de inicializar el sensor. */
void bme688_init(int temp_ovs, int press_ovs, int hum_ovs) {
    temp_ovs_global = temp_ovs;
    press_ovs_global = press_ovs;
    hum_ovs_global = hum_ovs;
    is_bme688_active = false;

    ret = chipid();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init abortada: CHIPID inválido (%s)", esp_err_to_name(ret));
        return;
    }

    ret = softreset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init abortada: softreset falló (%s)", esp_err_to_name(ret));
        return;
    }

    get_mode();
    set_oversampling_tph();
}

// void app_main(void) {
//     ESP_ERROR_CHECK(i2c_master_init(&bus_handle));
//     ESP_ERROR_CHECK(i2c_slave_init(&bus_handle, &device_bme688, BME688_SLAVE_ADDR, I2C_MASTER_FREQ_HZ));
//     chipid();
//     softreset();
//     get_mode();
//     set_oversampling_tph(2,16,1);
//     printf("Comienza lectura\n\n");
//     readout_data_bme688(NULL, true, true, true, true, true);
// }