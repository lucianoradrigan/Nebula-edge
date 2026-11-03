"""Los números del contrato con el firmware, escritos a mano a propósito.

Los dos lados los toman de los enums de schema.proto, así que no pueden
desincronizarse entre sí. Lo que sí puede pasar es que alguien renumere el enum:
compilaría en los dos lados, pero los devices ya flasheados y las filas de la
tabla config seguirían usando los números viejos. Este test es el que se queja.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import time
import unittest
from pathlib import Path

import models
import schema_pb2
from codec import DataCodec


class ContractTests(unittest.TestCase):

    def test_packet_type_bytes(self):
        self.assertEqual(
            (DataCodec.TYPE_DATA_1, DataCodec.TYPE_DATA_2, DataCodec.TYPE_DEEP_SLEEP,
             DataCodec.TYPE_MODEL_STATUS, DataCodec.TYPE_MODEL_CHUNK_REQUEST,
             DataCodec.TYPE_INFER_RESULT),
            (0x01, 0x02, 0x04, 0x05, 0x06, 0x07),
        )

    def test_protocol_numbers(self):
        self.assertEqual(
            (models.PROTOCOL_MQTT, models.PROTOCOL_UDP, models.PROTOCOL_TCP,
             models.PROTOCOL_BLE, models.PROTOCOL_INFER),
            (0, 1, 2, 3, 4),
        )
        self.assertEqual(models.VALID_PROTOCOLS, frozenset(range(5)))

    def test_time_sent_us_is_the_last_fixed64_field(self):
        """El device escribe time_sent_us sobre el paquete ya serializado,
        en los últimos 9 bytes (stamp_time_sent() en data_path.c). Eso solo
        funciona mientras sea fixed64 y el campo de número más alto del
        mensaje: si alguien agrega un campo con un número mayor, la marca de
        envío pisaría otra cosa."""
        for msg in (schema_pb2.Data_1, schema_pb2.Data_2):
            fields = msg.DESCRIPTOR.fields
            last = max(fields, key=lambda f: f.number)
            self.assertEqual(last.name, "time_sent_us", msg.DESCRIPTOR.name)
            self.assertEqual(last.type, last.TYPE_FIXED64, msg.DESCRIPTOR.name)

    def test_time_sent_us_occupies_the_last_9_bytes(self):
        """Lo mismo, visto desde los bytes: tag de un byte y 8 de valor
        little-endian al final, con el valor de relleno que pone el device."""
        for msg, tag in ((schema_pb2.Data_1(), 0x49), (schema_pb2.Data_2(), 0x71)):
            msg.id_device = "AA:BB:CC:DD:EE:01"
            msg.seq = 7
            msg.time_sent_us = 1
            raw = msg.SerializeToString()
            self.assertEqual(raw[-9], tag, msg.DESCRIPTOR.name)
            self.assertEqual(int.from_bytes(raw[-8:], "little"), 1)

    def test_config_carries_the_server_time_in_us_from_serialization(self):
        """La hora en µs se pone al serializar, no al leer la base: una config
        armada hace rato tiene que salir igual con la hora de ahora."""
        from tests.fakes import make_config
        cfg = make_config(1)
        before = time.time_ns() // 1000
        pb = schema_pb2.Config()
        pb.ParseFromString(DataCodec.serialize_config(cfg))
        self.assertTrue(before <= pb.time_server_us <= time.time_ns() // 1000)

    def test_schema_proto_is_the_same_on_both_sides(self):
        """schema.proto está duplicado a propósito entre firmware y servidor
        (ver nebulaedge_proto_schema/README.md). Si alguien edita una copia y
        no la otra, cada lado compila contra un contrato distinto."""
        server_dir = Path(__file__).resolve().parent.parent
        server_copy = server_dir / "schema.proto"
        firmware_copy = (server_dir.parent.parent / "esp32" / "components"
                         / "nebulaedge_proto_schema" / "schema.proto")
        if not firmware_copy.exists():
            self.skipTest("sin el firmware al lado (p.ej. dentro del contenedor)")
        self.assertEqual(server_copy.read_text(), firmware_copy.read_text(),
                         "las dos copias de schema.proto difieren")


if __name__ == "__main__":
    unittest.main()
