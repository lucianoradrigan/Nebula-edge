#ifndef BMM350
#define BMM350

#include "schema.pb-c.h"

#define ODR_1_5625                          15625
#define ODR_3_125                           3125
#define ODR_6_25                            625
#define ODR_12_5                            125
#define ODR_25                              25
#define ODR_50                              50
#define ODR_100                             100
#define ODR_200                             200
#define ODR_400                             400

void readout_data_bmm350(Data1 *data);
void bmm350_init(int odr, int avg);

#endif