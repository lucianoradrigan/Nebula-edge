"""Tests de ConfigResolver.evaluate(), la decisión sobre qué hacer con una
configuración cuyas versiones no calzan.

Es una función pura sobre tres versiones: no toca la base ni la red, así que
acá no hay device simulado, ni sockets, ni esperas. Es el lugar barato para
fijar el comportamiento antes de mirar un log de banco.

Correr desde raspberry/server/:

    python -m unittest discover -s tests
"""
from __future__ import annotations
import unittest

from config_resolver import ConfigDecision, ConfigResolver
from tests.fakes import make_config


def decide(applied: int, db: int, session: int, **kwargs):
    """Atajo: las tres versiones son lo único que mira el resolver."""
    return ConfigResolver.evaluate(
        applied, make_config(db), make_config(session), **kwargs
    )


class ConfigResolverTests(unittest.TestCase):

    def test_device_applied_something_newer_than_the_database(self):
        """El device reporta una versión mayor a la de la BD.

        Pasa tras un cambio de protocolo: el device ya aplicó la config nueva y
        el server todavía razona con la vieja. Hay que cerrar la sesión y volver
        a decidir el protocolo, y para eso se devuelve la config.
        """
        result = decide(applied=5, db=3, session=3)
        self.assertEqual(result.decision, ConfigDecision.APPLIED_NEWER)
        self.assertIsNotNone(result.db_config, "APPLIED_NEWER tiene que traer la config")

    def test_device_is_up_to_date(self):
        result = decide(applied=3, db=3, session=3)
        self.assertEqual(result.decision, ConfigDecision.UP_TO_DATE)
        self.assertIsNone(result.db_config)

    def test_new_version_in_the_database_is_pushed(self):
        """La BD tiene algo más nuevo que lo que esta sesión entregó: se manda."""
        result = decide(applied=3, db=4, session=3)
        self.assertEqual(result.decision, ConfigDecision.PUSH)
        self.assertEqual(result.db_config.config_version, 4)

    def test_config_already_sent_is_not_resent_right_away(self):
        """La sesión arrancó con esa misma versión: ya se le entregó al device.

        La primera respuesta correcta es esperar, no reenviar: el device puede
        estar justo en medio de aplicarla, y reenviar en loop lo único que hace
        es pisar el handshake.
        """
        result = decide(applied=3, db=4, session=4)
        self.assertEqual(result.decision, ConfigDecision.ALREADY_SENT)
        self.assertIsNone(result.db_config)

    def test_waiting_for_an_apply_that_never_comes_ends_in_a_push(self):
        """EL CALLEJÓN SIN SALIDA.

        `ALREADY_SENT` da por sentado que, como la config se envió, alcanza con
        esperar. La premisa de que se envió es cierta -el server la escribió-,
        pero la conclusión no: un envío puede perderse sin que el server se
        entere. Le pasaba a la escritura por BLE, que el firmware descartaba
        cuando su cola estaba llena (arreglado en 19ea6b4), y le puede pasar a
        cualquier escritura que no llegue.

        Y la espera no tenía tope: mientras siguiera llegando telemetría, la
        sesión no reenviaba nunca ni expiraba nunca, porque el timeout de datos
        se renueva con cada paquete. El device se quedaba con la config vieja
        indefinidamente.

        Que siga llegando telemetría es justamente la prueba de que el
        transporte activo funciona, así que cuando se agota la paciencia
        reenviar por ahí es estrictamente mejor que seguir esperando.
        """
        # Mientras haya paciencia, se espera: eso no cambia.
        self.assertEqual(
            decide(applied=3, db=4, session=4).decision,
            ConfigDecision.ALREADY_SENT,
        )

        # Agotada la paciencia, se reenvía en vez de esperar para siempre.
        result = decide(applied=3, db=4, session=4, apply_wait_exhausted=True)
        self.assertEqual(result.decision, ConfigDecision.PUSH)
        self.assertEqual(result.db_config.config_version, 4,
                         "el reenvío tiene que traer la config que falta aplicar")

    def test_exhausted_patience_does_not_change_the_other_decisions(self):
        """La paciencia agotada solo abre el camino del reenvío.

        Si el device está al día o adelantado, no hay nada que reenviar y la
        decisión tiene que ser la misma con y sin paciencia.
        """
        self.assertEqual(
            decide(applied=3, db=3, session=3, apply_wait_exhausted=True).decision,
            ConfigDecision.UP_TO_DATE,
        )
        self.assertEqual(
            decide(applied=5, db=3, session=3, apply_wait_exhausted=True).decision,
            ConfigDecision.APPLIED_NEWER,
        )


if __name__ == "__main__":
    unittest.main()
