"""Tests de DatabaseRepository._write, sin Postgres.

El SQL real no se prueba acá (ver tests/__init__.py): solo que un fallo se
reporta con False en vez de propagar, y que un bug nuestro no se disfraza de
fallo de base.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import unittest
from contextlib import contextmanager
from datetime import datetime
from unittest import mock

import repository
from models import Data_1, Log


# 2027-01-15 08:00:00.250 UTC: con fracción de segundo a propósito.
RECEIVED_AT = 1_800_000_000.25


def data_1() -> Data_1:
    return Data_1(id_device="AA:BB:CC:DD:EE:01", temperature=21.5, press=101325,
                  hum=40, co=1.0, config_version_applied=1, time_client=1_800_000_000)


class FakeCursor:
    def __init__(self, fail: Exception | None):
        self.fail = fail
        self.executed: list[tuple] = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def execute(self, sql, params):
        if self.fail:
            raise self.fail
        self.executed.append((sql, params))


class FakeConnection:
    def __init__(self, fail: Exception | None = None):
        self.cur = FakeCursor(fail)
        self.commits = 0

    def cursor(self):
        return self.cur

    def commit(self):
        self.commits += 1


class WriteTests(unittest.TestCase):

    def _repo(self, conn: FakeConnection):
        repo = repository.DatabaseRepository("postgresql://nadie")

        @contextmanager
        def connection():
            yield conn
        repo._connection = connection
        return repo

    def test_insert_is_committed(self):
        conn = FakeConnection()
        self.assertTrue(self._repo(conn).insert_data_1(data_1(), RECEIVED_AT))
        self.assertEqual(conn.commits, 1)
        sql, params = conn.cur.executed[0]
        self.assertIn("nebulaedge_schema.data_1", sql)
        self.assertEqual(params[0], "AA:BB:CC:DD:EE:01")

    def test_time_server_keeps_the_fraction_of_a_second(self):
        """time_client viene del device en segundos enteros; time_server es
        del servidor y tiene que conservar los milisegundos, o no sirve para
        medir tiempos entre paquetes."""
        conn = FakeConnection()
        self._repo(conn).insert_data_1(data_1(), RECEIVED_AT)
        sql, params = conn.cur.executed[0]
        self.assertIn("time_server", sql)
        self.assertEqual(params[-1], datetime(2027, 1, 15, 8, 0, 0, 250000))

    def test_database_failure_returns_false(self):
        conn = FakeConnection(fail=OSError("server closed the connection (simulado)"))
        with mock.patch.object(repository, "log") as log:
            self.assertFalse(self._repo(conn).insert_data_1(data_1(), RECEIVED_AT))
        self.assertIn("ERROR de base insertando data_1", log.call_args[0][0])
        self.assertEqual(conn.commits, 0)

    def test_missing_field_is_reported_as_a_bug(self):
        conn = FakeConnection()
        with mock.patch.object(repository, "log") as log:
            self.assertFalse(self._repo(conn).insert_data_1(object(), RECEIVED_AT))
        self.assertTrue(log.call_args[0][0].startswith("BUG: insertando data_1"))
        self.assertEqual(conn.cur.executed, [], "no debía llegar a la base")

    def test_insert_log_reports_its_own_failure(self):
        """insert_log recibe un parámetro llamado `log`, que tapa la función
        log() del módulo. Cuando el reporte vivía dentro de insert_log, un
        fallo terminaba llamando al Log como si fuera la función."""
        conn = FakeConnection(fail=OSError("the database system is shutting down (simulado)"))
        event = Log(id_device="AA:BB:CC:DD:EE:01", status_report=1, protocol_report=1,
                    batt_level=None, time_client=1_800_000_000, time_server=1_800_000_000)
        with mock.patch.object(repository, "log") as log:
            self.assertFalse(self._repo(conn).insert_log(event))
        self.assertIn("ERROR de base insertando log", log.call_args[0][0])


if __name__ == "__main__":
    unittest.main()
