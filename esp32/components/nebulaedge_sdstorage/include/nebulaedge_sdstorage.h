#ifndef NEBULAEDGE_SDSTORAGE
#define NEBULAEDGE_SDSTORAGE

#include <stdio.h>
#include "esp_err.h"

esp_err_t sdstorage_write_packet(uint8_t *data, size_t size);

#endif