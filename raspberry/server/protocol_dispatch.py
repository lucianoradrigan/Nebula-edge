"""Elección de la sesión de protocolo, y reelección cuando el protocolo cambia.

UTILIDAD PRINCIPAL
    Traducir el número `protocol_conf` de la configuración a la clase de sesión
    que le corresponde, correrla, y -cuando esa sesión termina devolviendo una
    configuración nueva- repetir con la que ahora toque. Ese bucle es lo que
    permite cambiar de protocolo en caliente sin reiniciar el servidor ni el
    device.

        protocol_conf:   0 MQTT    1 UDP    2 TCP    3 BLE

DAR DE ALTA UN PROTOCOLO NUEVO
    `session_classes` es, junto con escribir el `Transport` (transport.py) y su
    subclase de sesión (sessions.py), lo único que hay que tocar.

CUÁNDO TERMINA DE VERDAD
    El bucle corta cuando `session.run()` devuelve None: el device dejó de
    responder, se agotaron los reintentos de ACK, o el enlace se cortó sin
    posibilidad de reabrirlo. También corta si llega un protocol_conf que no
    está en el dict.

    Además de la sesión, se corre en paralelo un heartbeat que escribe en la
    tabla `log` mientras el protocolo esté activo; se cancela al terminar.
"""
from __future__ import annotations
import asyncio


from dataclasses import replace

from models import BleContext, ConfigData, Timeouts
from repository import DatabaseRepository
from sessions import MQTTDeviceSession, UDPDeviceSession, TCPDeviceSession, BLEDeviceSession
from system import log

# Valores de `protocol_conf` en la tabla config. Son parte del contrato con el
# firmware (ver el switch de app_main en esp32/main/main.c): no se reordenan.
PROTOCOL_MQTT = 0
PROTOCOL_UDP = 1
PROTOCOL_TCP = 2
PROTOCOL_BLE = 3


async def handle_protocol(
    device_id: str,
    db_dsn: str,
    initial_config: ConfigData,
    timeouts: Timeouts | None = None,
    ble: BleContext | None = None,
):
    """Despacha la sesión según el protocolo configurado en `ConfigData`."""
    config = initial_config
    idx = None
    timeouts = Timeouts() if timeouts is None else timeouts
    database_repo = DatabaseRepository(db_dsn)
    session_classes = {
        PROTOCOL_MQTT: MQTTDeviceSession,
        PROTOCOL_UDP: UDPDeviceSession,
        PROTOCOL_TCP: TCPDeviceSession,
        PROTOCOL_BLE: BLEDeviceSession,
    }

    while True:
        idx = config.protocol_conf
        session_cls = session_classes.get(idx)
        if session_cls is None:
            log(f"Protocol_conf inválido ({idx}) para {device_id}. Cerrando sesión.")
            idx = -1
            break

        session = session_cls(device_id, config, database_repo, timeouts, ble)

        # Solo la PRIMERA sesión puede aprovechar la conexión que dejó abierta
        # el descubrimiento; para cuando se cambie de protocolo ya estará
        # cerrada. La primera recibe el contexto tal cual (con el cliente); de
        # ahí en adelante se pasa una copia sin él. Es una copia y no una
        # mutación para no vaciarle el cliente a la sesión que ya lo tiene.
        if ble is not None and ble.client is not None:
            ble = replace(ble, client=None)

        # Heartbeat / loggeo rutinario
        heartbeat_task = asyncio.create_task(session._protocol_heartbeat_loop())
        try:
            config = await session.run()
        finally:
            heartbeat_task.cancel()
            try:
                await heartbeat_task
            except asyncio.CancelledError:
                pass

        if config is None:
            log(f"Sesión finalizada para {device_id}")
            break

    # Se retorna último protocolo, con propósito de loggeo
    return idx
