#ifndef NEBULAEDGE_MICROSD
#define NEBULAEDGE_MICROSD

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t sd_mount(void);
esp_err_t sd_format(void);
esp_err_t sd_format_if_no_space(void);
esp_err_t sd_unmount(void);
bool sd_is_mounted(void);

#endif