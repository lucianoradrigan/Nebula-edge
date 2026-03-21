idf_component_register(SRCS "bmi270.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nebulaedge_defs nebulaedge_microsd nebulaedge_i2c nebulaedge_protobuf
                       )