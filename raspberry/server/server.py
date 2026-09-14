"""Punto de entrada del server: arranca el descubrimiento BLE.

El resto de la lógica vive repartida por responsabilidad (antes todo esto
estaba junto en un único classes.py):

- discovery.py   escaneo BLE y ciclo de vida de cada dispositivo
- dispatch.py    elige la sesión según protocol_conf y sobrevive sus cambios
- sessions.py    el motor de sesión genérico + las 4 variantes por protocolo
- transport.py   cómo se mueven los bytes en cada protocolo
- repository.py  acceso a Postgres
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
