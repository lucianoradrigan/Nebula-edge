"""Tests de LocalWifiConfig: la red de los devices sale del entorno, no de nmcli.

    cd raspberry/server && python -m unittest discover -s tests
"""
import unittest
from unittest import mock

import system
from system import LocalWifiConfig


class LocalWifiConfigTests(unittest.TestCase):

    def setUp(self):
        LocalWifiConfig._warned_missing = False
        self.logs = []
        p = mock.patch.object(system, "log", lambda m, *a, **k: self.logs.append(m))
        p.start()
        self.addCleanup(p.stop)

    def _env(self, **kv):
        base = {"WIFI_SSID": "", "WIFI_PASSWD": "", "HOST_IP": "", "WIFI_DEVICE": "wlan0"}
        base.update(kv)
        p = mock.patch.dict("os.environ", base)
        p.start()
        self.addCleanup(p.stop)

    def test_credentials_come_from_env(self):
        self._env(WIFI_SSID="nebulaedge_ap", WIFI_PASSWD="clave1234", HOST_IP="10.42.0.1")
        self.assertEqual(LocalWifiConfig.get(), ("10.42.0.1", "nebulaedge_ap", "clave1234"))

    def test_ip_from_interface_when_host_ip_empty(self):
        self._env(WIFI_SSID="nebulaedge_ap", WIFI_PASSWD="clave1234")
        with mock.patch.object(LocalWifiConfig, "interface_ip", return_value="10.42.0.1") as ip:
            self.assertEqual(LocalWifiConfig.get()[0], "10.42.0.1")
            ip.assert_called_once_with("wlan0")

    def test_ip_falls_back_to_default_route(self):
        self._env(WIFI_SSID="x", WIFI_PASSWD="y")
        with mock.patch.object(LocalWifiConfig, "interface_ip", return_value=""), \
             mock.patch.object(LocalWifiConfig, "_local_ip_fallback", return_value="192.168.8.111"):
            self.assertEqual(LocalWifiConfig.get()[0], "192.168.8.111")

    def test_missing_ssid_warns_once(self):
        self._env(HOST_IP="10.42.0.1")
        LocalWifiConfig.get()
        LocalWifiConfig.get()
        self.assertEqual(sum("WIFI_SSID" in m for m in self.logs), 1)

    def test_never_runs_nmcli(self):
        self._env(WIFI_SSID="x", WIFI_PASSWD="y")
        with mock.patch("subprocess.run", side_effect=AssertionError("no debe correr procesos")):
            LocalWifiConfig.get()


if __name__ == "__main__":
    unittest.main()
