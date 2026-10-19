Para crear el esquema de datos el único archivo a modificar es el archivo 
schema.proto y luego compilar con protobuf-c. Los demás archivos (.c y .h) son resultado de la compilación.
 
Se debe instalar la librería protoc.

Para compilar el archivo .proto se debe correr $protoc --c_out=. schema.proto
Esto creará los archivos .h y .c. El header es el que incluye en el archivo a trabajar.

Si se quiere modificar .proto solo hay que compilar otra vez (comando de arriba).


Obs: protobuf es incluido como componente porque schema.c incluye el header proto-c.h.

**El servidor usa el mismo contrato.** `schema.proto` está duplicado a propósito
en `raspberry/server/schema.proto`: después de editarlo, copiarlo allá y
regenerar también el código Python, desde `raspberry/server`:

    protoc --python_out=. schema.proto

Los dos `schema.proto` tienen que quedar idénticos (`diff` sin salida).
