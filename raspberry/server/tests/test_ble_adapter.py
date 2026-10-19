"""Qué adaptador BLE elige el servidor.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import unittest
from unittest import mock

from system import BLEAdapterResolver


class BleAdapterTests(unittest.TestCase):

    def _resolve(self, env, usb, sysfs):
        listdir = mock.Mock(side_effect=OSError) if sysfs is None else mock.Mock(return_value=sysfs)
        with mock.patch.dict("os.environ", {"BLE_ADAPTER": env}), \
             mock.patch.object(BLEAdapterResolver, "detect_usb_adapter", return_value=usb), \
             mock.patch("system.os.listdir", listdir):
            return BLEAdapterResolver.resolve()

    def test_explicit_name_wins(self):
        self.assertEqual(self._resolve("hci3", "hci1", ["hci0"]), "hci3")

    def test_usb_dongle_is_preferred(self):
        self.assertEqual(self._resolve("auto", "hci1", ["hci0", "hci1"]), "hci1")

    def test_pi5_without_dongle_uses_the_onboard_one(self):
        """Pi 5 sin dongle: solo hci0. Antes caía a hci1 y el servidor no arrancaba."""
        self.assertEqual(self._resolve("auto", "", ["hci0", "hci0:64"]), "hci0")

    def test_nothing_visible_falls_back_to_hci0(self):
        self.assertEqual(self._resolve("auto", "", None), "hci0")


if __name__ == "__main__":
    unittest.main()
