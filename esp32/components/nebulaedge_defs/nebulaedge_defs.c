#include "nebulaedge_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <time.h>

SemaphoreHandle_t semaphore = NULL;