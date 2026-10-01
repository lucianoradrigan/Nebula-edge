"""Decisión sobre la configuración de un device: qué hacer con las versiones que no calzan.

UTILIDAD PRINCIPAL
    `ConfigResolver.evaluate()` compara tres versiones de configuración -la que
    el device dice tener aplicada, la que hay en la BD, y la que esta sesión ya
    le envió- y devuelve una de tres decisiones:

        APPLIED_NEWER   el device ya aplicó algo más nuevo de lo que la sesión
                        creía: hay que cerrar y volver a decidir el protocolo
        ALREADY_SENT    la config nueva ya se le mandó; falta que la aplique,
                        así que por ahora solo hay que esperar
        PUSH            hay una versión nueva en la BD que todavía no se envía,
                        o una que se envió y el device nunca aplicó

    Las tres versiones se comparan porque ninguna sola alcanza: la del device
    dice qué está corriendo, la de la BD qué debería correr, y la de la sesión
    evita reenviar en loop una config que ya se empujó y aún no se confirma.

    `apply_wait_exhausted` es el cuarto dato, y existe porque las versiones no
    alcanzan para todo: dicen si la config se envió, pero no si llegó. Ver la
    nota en evaluate().

ES UNA FUNCIÓN PURA
    No toca la base de datos ni la red: recibe los objetos ya cargados y
    devuelve una decisión. Por eso se puede testear con dataclasses en memoria,
    sin Postgres ni hardware de por medio.
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
        apply_wait_exhausted: bool = False,
    ) -> ConfigDecisionResult:
        """`apply_wait_exhausted`: la sesión ya esperó bastante a que el device
        aplique la config que se le entregó al abrirla.

        POR QUÉ HACE FALTA
            ALREADY_SENT da por sentado que, como la config se envió, alcanza
            con esperar. La premisa es cierta -el server la escribió- pero la
            conclusión no: un envío puede perderse sin que el server se entere.
            Le pasaba a la escritura por BLE del redescubrimiento, que el
            firmware descartaba cuando su cola estaba llena (arreglado en
            19ea6b4), y le puede pasar a cualquier escritura que no llegue.

            Y la espera no tenía tope: mientras siguiera llegando telemetría la
            sesión no reenviaba nunca ni expiraba nunca, porque el timeout de
            datos se renueva con cada paquete. El device se quedaba con la
            config vieja indefinidamente.

            Que la telemetría siga llegando es justamente la prueba de que el
            transporte activo funciona, así que al agotarse la paciencia
            reenviar por ahí es estrictamente mejor que seguir esperando.

        QUIÉN MIDE EL TIEMPO
            La sesión, no esta función: acá no hay reloj ni estado, y la idea es
            que siga sin tenerlos. Ver ProtocolSession._session_loop().
        """
        if applied_version > db_config.config_version:
            return ConfigDecisionResult(ConfigDecision.APPLIED_NEWER, db_config)

        if applied_version < db_config.config_version:
            if (db_config.config_version <= session_config.config_version
                    and not apply_wait_exhausted):
                return ConfigDecisionResult(ConfigDecision.ALREADY_SENT)
            return ConfigDecisionResult(ConfigDecision.PUSH, db_config)

        return ConfigDecisionResult(ConfigDecision.UP_TO_DATE)
