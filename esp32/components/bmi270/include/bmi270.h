#ifndef BMI270
#define BMI270

#include <stdbool.h>
#include "schema.pb-c.h"

void readout_data_bmi270(Inertial *data);
void bmi270_init(int acc_odr, int acc_avg, int acc_range, int gyr_odr, int gyr_range);

#endif