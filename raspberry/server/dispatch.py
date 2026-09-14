"""Despacha la sesión activa según `protocol_conf` y sobrevive a los cambios de protocolo.

Movido desde classes.py sin cambios de lógica. `session_classes` es, junto
con escribir el Transport mismo, el único lugar que hay que tocar para
dar de alta un protocolo nuevo.
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
