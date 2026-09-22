#include "nebulaedge_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <time.h>

const uint8_t DEEP_SLEEP_FLAG[DEEP_SLEEP_FLAG_LEN] = {0x04, 'd', 's'};

SemaphoreHandle_t semaphore = NULL;
