# SRCS: archivos a compilar.
# INCLUDE_DIRS: directorios donde se buscarán headers. 
# El scope de estos headers será global. 

idf_component_register(SRCS "main.c" 
                       INCLUDE_DIRS ".")