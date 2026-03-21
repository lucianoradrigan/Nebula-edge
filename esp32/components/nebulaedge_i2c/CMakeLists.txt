idf_component_register(SRCS "nebulaedge_i2c.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_i2c nebulaedge_defs)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
