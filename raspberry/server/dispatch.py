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

from bleak.backends.device import BLEDevice

from models import ConfigData, Timeouts
from repository import DatabaseRepository
from sessions import MQTTDeviceSession, UDPDeviceSession, TCPDeviceSession, BLEDeviceSession


async def handle_protocol(
    device: BLEDevice,
    db_dsn: str,
    initial_config: ConfigData,
    scanner_lock: asyncio.Lock | None = None,
    scanner_stop = None,
    scanner_start = None,
    ble_adapter: str | None = None,
    timeouts: Timeouts | None = None,
):
    """Despacha la sesión según el protocolo configurado en `ConfigData`."""
    config = initial_config
    idx = None
    timeouts = timeouts or Timeouts()
    database_repo = DatabaseRepository(db_dsn)
    session_classes = {
        0: MQTTDeviceSession,
        1: UDPDeviceSession,
        2: TCPDeviceSession,
        3: BLEDeviceSession,
    }

    device_id = device.address
    while True:
        idx = config.protocol_conf
        session_cls = session_classes.get(idx)
        if session_cls is None:
            print(f"Protocol_conf inválido ({idx}) para {device_id}. Cerrando sesión.")
            idx = -1
            break

        session = session_cls(
            device,
            config,
            database_repo,
            scanner_lock,
            scanner_stop,
            scanner_start,
            ble_adapter,
            timeouts,
        )

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
            print(f"Sesión finalizada para {device_id}")
            break

    # Se retorna último protocolo, con propósito de loggeo
    return idx
