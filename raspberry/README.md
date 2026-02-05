# Raspberry server set up

Requerimientos:
    - Permisos sudo en la Raspberry
    - Tener Docker instalado

Primero se debe crear el contenedor Docker donde correrá el server:

```
sudo docker compose build
```


La base de datos tiene que estar completamente inicializada para que el server funcione. En las primeras iteraciones el server fallará por esto mismo.