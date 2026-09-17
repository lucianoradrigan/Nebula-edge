#ifndef BME688
#define BME688

#include "schema.pb-c.h"

void readout_data_bme688(Environmental *data);
void bme688_init(int temp_ovs, int press_ovs, int hum_ovs);

#endif