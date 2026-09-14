"""Punto de entrada del servidor NebulaEdge.

UTILIDAD PRINCIPAL
    Levantar el descubrimiento BLE y quedarse corriendo. Es el proceso que
    ejecuta el contenedor: `raspberry/entrypoint.sh` -> `python3 -u server.py`.

No tiene lógica propia: construye `MasterConnection` (discovery.py) y le cede
el control al event loop de asyncio, que a partir de ahí atiende en paralelo
el escaneo BLE y una sesión por cada dispositivo conectado.

MAPA DEL SERVIDOR
    Los módulos, en el orden en que se encadenan durante una sesión:

    server.py           arranca el proceso
    discovery.py        escanea BLE, entrega la config inicial, abre una sesión
    dispatch.py         elige qué sesión corre según protocol_conf
    sessions.py         el bucle de sesión, idéntico para los 4 protocolos
    transport.py        lo único que cambia entre protocolos: mover los bytes
    router.py           clasifica el paquete entrante y persiste la telemetría
    config_resolver.py  decide si hay que empujar una config nueva al device
    repository.py       acceso a Postgres

    De apoyo, sin orden particular:

    codec.py            (de)serialización protobuf
    models.py           dataclasses del dominio
    system.py           reloj UTC, adaptador BLE, credenciales WiFi del host
    mqtt.py             cliente MQTT compartido del proceso
    ble.py              UUIDs del servicio GATT

CONFIGURACIÓN
    El DSN de Postgres y el nombre BLE objetivo tienen valores por defecto en
    `MasterConnection.__init__`. El adaptador BLE se puede forzar con la
    variable de entorno BLE_ADAPTER (ver system.py).
"""
from __future__ import annotations
import asyncio

from discovery import MasterConnection

if __name__ == "__main__":
    master = MasterConnection()
    try:
        asyncio.run(master.run())
    except KeyboardInterrupt:
        print("\nCerrando programa...")
