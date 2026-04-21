idf_component_register(SRCS "nebulaedge_sdstorage.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nebulaedge_microsd nebulaedge_proto_schema json)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
