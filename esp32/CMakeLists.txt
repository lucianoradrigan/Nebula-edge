# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# set(IDF_TARGET esp32s3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# NOTA HISTÓRICA: acá vivía un -Wl,--wrap=gpio_set_level.
#
# Venía del proyecto de bringup de la IM-V2, donde el chip select de la microSD
# (IO0 del expansor FXL6408, por I2C) se le entregaba a SDSPI como un pin
# virtual 100 + IO para atenderlo en __wrap_gpio_set_level(). El wrap hacía su
# parte, pero sdspi_host_init_device() también configura el chip select como
# GPIO real con 1ULL << gpio_cs, y correr 100 lugares un valor de 64 bits es
# comportamiento indefinido: acá queda en 100 & 63 = 36, que en esta placa es
# una línea de datos de la flash octal. Reconfigurarla reiniciaba el chip por
# interrupt watchdog. El detalle está en sd_mount(), en nebulaedge_microsd.c.
#
# Ahora SDSPI corre con SDSPI_SLOT_NO_CS y el chip select lo mueve el
# componente, así que el wrap ya no hace falta y nebulaedge_microsd volvió a
# ser autocontenido.

project(nebulaedge)
