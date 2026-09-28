# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# set(IDF_TARGET esp32s3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# En la IM-V2 el chip select de la microSD no es un GPIO: cuelga del IO0 del
# expansor FXL6408, por I2C. El driver SDSPI de ESP-IDF mueve ese CS con
# gpio_set_level() y no ofrece ningún punto de enganche, así que la única forma
# de interceptarlo es este wrap del linker: nebulaedge_microsd le pasa a SDSPI
# un pin virtual (100 + IO) y atiende esos números en __wrap_gpio_set_level().
#
# POR QUÉ ESTÁ ACÁ Y NO EN EL COMPONENTE
#     No se puede acotar: idf_build_set_property aplica al enlace de la imagen
#     entera, ESP-IDF incluido. Hoy redirige cuatro objetos enlazados
#     (sdspi_host.c, sdmmc_host.c, nebulaedge_i2c.c, i2c_common.c); los otros
#     tres pasan derecho a __real_ porque sus GPIO son <= 99.
#
# CONSECUENCIA
#     nebulaedge_microsd deja de ser autocontenido: necesita esta línea. Si se
#     copia a otro proyecto sin ella, el enlace falla con "undefined reference
#     to __real_gpio_set_level", que al menos es un fallo ruidoso.
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=gpio_set_level" "-Wl,-u,__wrap_gpio_set_level" APPEND)

project(nebulaedge)
