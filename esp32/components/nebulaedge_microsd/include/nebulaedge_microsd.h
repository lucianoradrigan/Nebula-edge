#ifndef NEBULAEDGE_MICROSD
#define NEBULAEDGE_MICROSD

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t mount_sd(void);
esp_err_t format_sd(void);
esp_err_t format_sd_if_no_space(void);
esp_err_t unmount_sd(void);
bool is_sd_mounted(void);

#endif