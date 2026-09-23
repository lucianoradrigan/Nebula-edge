"""Clasificación de los paquetes que llegan del device, y persistencia de la telemetría.

UTILIDAD PRINCIPAL
    `PacketRouter.route()` recibe los bytes crudos de un paquete y responde qué
    eran, en forma de `PacketOutcome`:

        TELEMETRY    era Environmental o Inertial; ya quedó insertado en la BD
        DEEP_SLEEP   el device avisa que se va a dormir
        IGNORED      no se pudo decodificar, o no es un tipo conocido

DÓNDE TERMINA SU RESPONSABILIDAD
    Decodificar, loggear e insertar. Lo que la sesión haga DESPUÉS con esa
    respuesta -seguir escuchando, reabrir el enlace, comparar versiones de
    config, cerrar la sesión- queda deliberadamente afuera: eso es control de
    flujo y vive en `ProtocolSession` (sessions.py).

    Tampoco lleva la cuenta del último `time_client` visto: eso es estado de la
    sesión, no del enrutado. El caller lo lee de `RoutedPacket.data` si lo
    necesita.

POR QUÉ ES ASYNC
    Las inserciones pasan por los envoltorios `*_async` del repositorio
    (asyncio.to_thread por debajo) para no bloquear el event loop en cada
    paquete recibido. `TelemetryRepository` refleja eso en su firma.

DESACOPLADO DE POSTGRES
    `TelemetryRepository` es el contrato mínimo que necesita: cualquier objeto
    con esos dos métodos sirve. `DatabaseRepository` lo cumple sin heredar de
    él, y los tests le pasan un doble en memoria.
"""
from __future__ import annotations
from dataclasses import dataclass
from enum import Enum, auto
from typing import Protocol

from codec import DataCodec
from models import Environmental, Inertial
from system import log


class TelemetryRepository(Protocol):
    """Contrato mínimo que PacketRouter necesita de un repositorio.

    DatabaseRepository (repository.py) lo cumple por duck typing, sin heredar
    de esto. Sirve para poder testear PacketRouter con un stub en memoria,
    sin tocar Postgres.
    """
    async def insert_environmental_async(self, environmental: Environmental) -> bool: ...
    async def insert_inertial_async(self, inertial: Inertial) -> bool: ...


class PacketOutcome(Enum):
    TELEMETRY = auto()    # se decodificó un Environmental/Inertial válido
    DEEP_SLEEP = auto()   # paquete de aviso de deep sleep (tipo 0x04)
    IGNORED = auto()      # paquete vacío, corrupto o de tipo desconocido


@dataclass(frozen=True)
class RoutedPacket:
    """Qué resultó de un paquete entrante.

    `outcome` habla de lo que LLEGÓ; `persisted`, de si se pudo guardar. Antes
    eran lo mismo: TELEMETRY significaba "se decodificó e insertó", pero los
    insert se tragaban cualquier error de base, así que decía "insertado" sin
    que nadie lo hubiera verificado. Separarlos deja a la sesión saber que el
    device sigue vivo aunque la base esté caída.
    """
    outcome: PacketOutcome
    data: Environmental | Inertial | None = None
    data_type: int | None = None
    persisted: bool = True


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

        if data_type == self.codec.TYPE_ENVIRONMENTAL:
            log(f"{prefix}Paquete Environmental recibido de {device_id}")
            persisted = await self.database_repo.insert_environmental_async(data)
        elif data_type == self.codec.TYPE_INERTIAL:
            log(f"{prefix}Paquete Inertial recibido de {device_id}")
            persisted = await self.database_repo.insert_inertial_async(data)
        else:
            return RoutedPacket(PacketOutcome.IGNORED)

        if not persisted:
            log(f"{prefix}AVISO: la telemetría de {device_id} NO se guardó en la base")

        return RoutedPacket(PacketOutcome.TELEMETRY, data=data,
                            data_type=data_type, persisted=persisted)
