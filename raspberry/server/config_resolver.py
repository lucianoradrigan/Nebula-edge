"""Comparación de versiones de configuración (device vs BD vs sesión).

Extrae la lógica que está copiada 4 veces (MQTT/UDP/TCP/BLE) en classes.py,
por ejemplo en UDPDeviceSession.run():

    applied_version = data.config_version_applied
    if applied_version > db_config.config_version:
        ...cerrar sesión para reconfigurar...
    elif applied_version < db_config.config_version:
        if db_config.config_version <= self.config.config_version:
            ...ya se envió, esperar...
        else:
            ...enviar nueva config + esperar ACK...
    else:
        ...al día, seguir recibiendo telemetría...

`ConfigResolver.evaluate()` es una función pura (sin I/O, sin acceso a BD):
toma la versión que el device dice tener aplicada y las dos versiones de
referencia, y devuelve una decisión. Así se puede testear con paquetes/
objetos en memoria, sin Postgres ni hardware de por medio.
"""
from __future__ import annotations
from dataclasses import dataclass
from enum import Enum, auto

from models import ConfigData


class ConfigDecision(Enum):
    # El device ya aplicó una config con versión mayor a la que el server
    # conocía (p.ej. tras un cambio de protocolo): hay que cerrar la sesión
    # actual y servir esa config para que el resto del sistema se entere
    # del nuevo protocolo/parámetros.
    APPLIED_NEWER = auto()

    # El device está al día con la versión vigente en la BD: seguir
    # recibiendo telemetría normalmente.
    UP_TO_DATE = auto()

    # Hay una config más nueva en la BD, pero es la misma que ya se le
    # entregó al device al iniciar esta sesión (cambio de protocolo en
    # curso): hay que esperar a que el device la aplique, sin reenviar.
    ALREADY_SENT = auto()

    # Hay una config más nueva en la BD y todavía no se le ha entregado al
    # device en esta sesión: hay que enviarla y esperar ACK.
    PUSH = auto()


@dataclass(frozen=True)
class ConfigDecisionResult:
    decision: ConfigDecision
    # Poblado en APPLIED_NEWER (la config que el device ya tiene aplicada)
    # y en PUSH (la config que hay que enviarle). None en los demás casos.
    db_config: ConfigData | None = None


class ConfigResolver:
    """Decide qué hacer al comparar `config_version_applied` (reportado por
    el device en cada paquete de datos) contra la versión vigente en BD y
    la versión con la que arrancó la sesión actual."""

    @staticmethod
    def evaluate(
        applied_version: int,
        db_config: ConfigData,
        session_config: ConfigData,
    ) -> ConfigDecisionResult:
        if applied_version > db_config.config_version:
            return ConfigDecisionResult(ConfigDecision.APPLIED_NEWER, db_config)

        if applied_version < db_config.config_version:
            if db_config.config_version <= session_config.config_version:
                return ConfigDecisionResult(ConfigDecision.ALREADY_SENT)
            return ConfigDecisionResult(ConfigDecision.PUSH, db_config)

        return ConfigDecisionResult(ConfigDecision.UP_TO_DATE)
