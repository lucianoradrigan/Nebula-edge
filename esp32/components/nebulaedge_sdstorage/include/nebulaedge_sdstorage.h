#ifndef NEBULAEDGE_SDSTORAGE
#define NEBULAEDGE_SDSTORAGE

#include <stdio.h>
#include "esp_err.h"

esp_err_t data_to_sd(uint8_t *data, size_t size);

#endif