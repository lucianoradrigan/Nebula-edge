idf_component_register(SRCS "schema.pb-c.c"
                    INCLUDE_DIRS "."
                    REQUIRES protobuf-c)

if(CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE)
   # some variables are only used by asserts
   target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable)
endif()
