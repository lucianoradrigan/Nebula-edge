"""Punto de entrada del servidor NebulaEdge.

UTILIDAD PRINCIPAL
    Levantar el descubrimiento BLE y quedarse corriendo. Es el proceso que
    ejecuta el contenedor: `raspberry/entrypoint.sh` -> `python3 -u server.py`.

No tiene lógica propia: construye `DeviceDiscovery` (discovery.py) y le cede
el control al event loop de asyncio, que a partir de ahí atiende en paralelo
el escaneo BLE y una sesión por cada dispositivo conectado.

MAPA DEL SERVIDOR
    Los módulos, en el orden en que se encadenan durante una sesión:

    server.py           arranca el proceso
    discovery.py        escanea BLE, entrega la config inicial, abre una sesión
    protocol_dispatch.py  elige qué sesión corre según protocol_conf
    sessions.py         el bucle de sesión, idéntico para los 4 protocolos
    transport.py        lo único que cambia entre protocolos: mover los bytes
    packet_router.py    clasifica el paquete entrante y persiste la telemetría
    config_resolver.py  decide si hay que empujar una config nueva al device
    repository.py       acceso a Postgres

    De apoyo, sin orden particular:

    codec.py            (de)serialización protobuf
    models.py           dataclasses del dominio
    system.py           lo que viene del host: reloj UTC, DSN, adaptador BLE, WiFi
    mqtt_client.py      cliente MQTT compartido del proceso
    gatt_uuids.py       UUIDs del servicio GATT

CONFIGURACIÓN
    Todo lo que depende del entorno se lee en system.py, no acá ni en
    discovery.py:

        PG_HOST / PG_DB / PG_USER / PG_PASSWORD   conexión a Postgres
        BLE_ADAPTER                               adaptador BLE a usar
        HOST_IP / WIFI_SSID / WIFI_PASSWD         credenciales que van al device

    Las cuatro primeras las pone docker-compose.yml a partir de un .env
    opcional (plantilla en raspberry/.env.example); sin él caen a los valores
    de desarrollo. El nombre BLE objetivo sigue siendo un parámetro por defecto
    de `DeviceDiscovery.__init__`.
"""
from __future__ import annotations
import asyncio

from discovery import DeviceDiscovery

if __name__ == "__main__":
    master = DeviceDiscovery()
    try:
        asyncio.run(master.run())
    except KeyboardInterrupt:
        print("\nCerrando programa...")
