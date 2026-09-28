idf_component_register(SRCS "nebulaedge_microsd.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_sdspi esp_driver_spi esp_driver_gpio fatfs fxl6408)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
