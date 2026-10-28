"""Tests del cierre de sesión en DeviceDiscovery, sin BLE ni Postgres.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import unittest
from unittest import mock

import discovery
from tests.fakes import FakeBLEDevice, make_config


class SessionCleanupTests(unittest.IsolatedAsyncioTestCase):

    def setUp(self):
        p = mock.patch.object(discovery.BLEAdapterResolver, "resolve", return_value="hci0")
        p.start()
        self.addCleanup(p.stop)
        self.disc = discovery.DeviceDiscovery(db_dsn="postgresql://nadie")
        self.device = FakeBLEDevice()
        self.disc.devices[self.device.address] = discovery.DeviceDiscovery.State.CONNECTED

    async def _end_session(self, insert_log):
        async def session_ends(*args, **kwargs):
            return 0
        with mock.patch.object(discovery, "handle_protocol", session_ends), \
             mock.patch.object(discovery.DatabaseRepository, "insert_log_async", insert_log):
            await self.disc._device_session(self.device, make_config(1))

    async def test_device_is_released_when_the_session_ends(self):
        async def ok(*args, **kwargs):
            return None
        await self._end_session(ok)
        self.assertNotIn(self.device.address, self.disc.devices)

    async def test_device_is_released_even_if_the_database_is_down(self):
        """Con Postgres reiniciándose, registrar la desconexión falla. Antes esa
        excepción cortaba la limpieza y el device quedaba marcado como
        conectado: el descubrimiento lo ignoraba hasta reiniciar el servidor."""
        async def db_down(*args, **kwargs):
            raise OSError("the database system is shutting down (simulado)")
        await self._end_session(db_down)
        self.assertNotIn(self.device.address, self.disc.devices,
                         "el device quedó marcado y no se va a volver a descubrir")


if __name__ == "__main__":
    unittest.main()
