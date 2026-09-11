"""Decodifica paquetes tipados de telemetría y los persiste.

Extrae el bloque que está copiado 6 veces (MQTT/UDP/TCP/BLE) en classes.py,
por ejemplo en UDPDeviceSession.run():

    data, data_type = DataCodec.deserialize_typed_packet(packet)
    if data == None or data_type == -1:
        continue
    if data_type == DataCodec.TYPE_DATA_1:
        print(f"UDP: Paquete Data_1 recibido de {self.device_id}")
        await self.database_repo.insert_data_1_async(data)
    elif data_type == DataCodec.TYPE_DATA_2:
        print(f"UDP: Paquete Data_2 recibido de {self.device_id}")
        await self.database_repo.insert_data_2_async(data)
    elif data_type == DataCodec.TYPE_DEEP_SLEEP:
        continue
    else:
        continue

`PacketRouter.route()` hace la parte que es igual en las 6 sesiones:
decodificar + loggear + insertar (o reconocer deep sleep / descartar
basura). Lo que cada sesión hace DESPUÉS (seguir esperando, comparar
versión de config, cerrar la sesión) se queda fuera: eso es control de
flujo propio de cada transporte y vive en DeviceSession/ConfigResolver.

`_update_last_client_time` tampoco vive acá: es estado de la sesión (cuál
fue el último time_client visto), no algo de "enrutar un paquete". El
caller lee `RoutedPacket.data.time_client` si lo necesita.

A diferencia de un primer borrador de esta idea, `route()` es async: las
inserciones pasan por `insert_data_1_async`/`insert_data_2_async`
(asyncio.to_thread por debajo), para no bloquear el event loop en cada
paquete. `TelemetryRepository` refleja eso en su firma.
"""
from __future__ import annotations
from dataclasses import dataclass
from enum import Enum, auto
from typing import Protocol

from codec import DataCodec
from models import Data_1, Data_2


class TelemetryRepository(Protocol):
    """Contrato mínimo que PacketRouter necesita de un repositorio.

    DatabaseRepository (classes.py) lo cumple por duck typing, sin heredar
    de esto. Sirve para poder testear PacketRouter con un stub en memoria,
    sin tocar Postgres.
    """
    async def insert_data_1_async(self, data_1: Data_1) -> None: ...
    async def insert_data_2_async(self, data_2: Data_2) -> None: ...


class PacketOutcome(Enum):
    TELEMETRY = auto()    # se decodificó e insertó un Data_1/Data_2
    DEEP_SLEEP = auto()   # paquete de aviso de deep sleep (tipo 0x04)
    IGNORED = auto()      # paquete vacío, corrupto o de tipo desconocido


@dataclass(frozen=True)
class RoutedPacket:
    outcome: PacketOutcome
    data: Data_1 | Data_2 | None = None
    data_type: int | None = None


class PacketRouter:
    """Decodifica un paquete tipado y, si es telemetría, la persiste."""

    def __init__(self, database_repo: TelemetryRepository, codec: type[DataCodec] = DataCodec):
        self.database_repo = database_repo
        self.codec = codec

    async def route(self, packet: bytes, device_id: str, source: str = "") -> RoutedPacket:
        """Decodifica `packet` e inserta en BD si corresponde.

        `source` es solo para el prefijo del log (p.ej. "UDP", "BLE"),
        igual que hacía cada sesión antes.
        """
        data, data_type = self.codec.deserialize_typed_packet(packet)
        if data is None or data_type == -1:
            return RoutedPacket(PacketOutcome.IGNORED)

        prefix = f"{source}: " if source else ""

        if data_type == self.codec.TYPE_DEEP_SLEEP:
            return RoutedPacket(PacketOutcome.DEEP_SLEEP, data_type=data_type)

        if data_type == self.codec.TYPE_DATA_1:
            print(f"{prefix}Paquete Data_1 recibido de {device_id}")
            await self.database_repo.insert_data_1_async(data)
        elif data_type == self.codec.TYPE_DATA_2:
            print(f"{prefix}Paquete Data_2 recibido de {device_id}")
            await self.database_repo.insert_data_2_async(data)
        else:
            return RoutedPacket(PacketOutcome.IGNORED)

        return RoutedPacket(PacketOutcome.TELEMETRY, data=data, data_type=data_type)
