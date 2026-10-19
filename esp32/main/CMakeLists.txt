# SRCS: archivos a compilar.
# INCLUDE_DIRS: directorios donde se buscarán headers. 
# El scope de estos headers será global. 

idf_component_register(SRCS "main.c" "infer.c" "model_runner.cc"
                       INCLUDE_DIRS ".")