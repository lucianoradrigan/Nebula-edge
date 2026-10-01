idf_component_register(SRCS "nebulaedge_busscan.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_driver_spi fxl6408)
