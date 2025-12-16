idf_component_register(SRCS "nebulaedge_mqtt.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_netif mqtt nebulaedge_defs nebulaedge_protobuf)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
