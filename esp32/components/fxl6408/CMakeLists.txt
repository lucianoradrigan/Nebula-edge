idf_component_register(SRCS "fxl6408.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_i2c)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
