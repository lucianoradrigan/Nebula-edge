idf_component_register(SRCS "nebulaedge_model_store.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_partition nvs_flash mbedtls)
