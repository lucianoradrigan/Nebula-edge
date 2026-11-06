"""Tests del cierre de sesión en DeviceDiscovery, sin BLE ni Postgres.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import asyncio
import unittest
from unittest import mock

from bleak.exc import BleakError

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


class ScannerErrorTests(unittest.IsolatedAsyncioTestCase):
    """Un error del adaptador BLE al pausar o reanudar el escaneo no puede
    matar a connection_worker: es la única task que entrega la config inicial,
    y sin ella el servidor sigue escaneando pero no atiende a ningún device."""

    class AdapterGoneScanner:
        """Como bleak con el dongle desconectado: BleakError, no BleakDBusError."""
        async def start(self):
            raise BleakError("adapter 'hci1' not found")

        async def stop(self):
            raise BleakError("adapter 'hci1' not found")

    class UnreachableClient:
        def __init__(self, *args, **kwargs):
            pass

        async def connect(self):
            raise OSError("no conecta (simulado)")

        async def disconnect(self):
            pass

    async def test_worker_survives_an_adapter_error(self):
        with mock.patch.object(discovery.BLEAdapterResolver, "resolve", return_value="hci1"):
            disc = discovery.DeviceDiscovery(db_dsn="postgresql://nadie")
        disc.scanner = self.AdapterGoneScanner()
        disc.scanner_running = True     # para que _scanner_stop() también lo intente

        asked = []

        async def get_config(_repo, addr):
            asked.append(addr)
            return make_config(1)

        with mock.patch.object(discovery.DatabaseRepository, "get_config_async", get_config), \
             mock.patch.object(discovery, "BleakClient", self.UnreachableClient):
            worker = asyncio.create_task(disc.connection_worker())
            self.addCleanup(worker.cancel)

            disc.shared_queue.put_nowait(FakeBLEDevice("AA:BB:CC:DD:EE:01"))
            await asyncio.sleep(0.1)
            self.assertFalse(worker.done(), "connection_worker murió con el error del adaptador")

            disc.shared_queue.put_nowait(FakeBLEDevice("AA:BB:CC:DD:EE:02"))
            await asyncio.sleep(0.1)
            self.assertEqual(asked, ["AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"],
                             "el segundo device no se atendió")


if __name__ == "__main__":
    unittest.main()
