idf_component_register(SRCS "nebulaedge_tcp.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash esp_event esp_netif nebulaedge_defs)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
