#ifndef BME688
#define BME688

#include "schema.pb-c.h"

void readout_data_bme688(Data1 *data, bool loop, bool act_temp, bool act_press, bool act_hum, bool act_gas, int temp_ovs, int press_ovs, int hum_ovs);
void bme688_init(void);

#endif