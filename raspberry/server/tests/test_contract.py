"""Los números del contrato con el firmware, escritos a mano a propósito.

Los dos lados los toman de los enums de schema.proto, así que no pueden
desincronizarse entre sí. Lo que sí puede pasar es que alguien renumere el enum:
compilaría en los dos lados, pero los devices ya flasheados y las filas de la
tabla config seguirían usando los números viejos. Este test es el que se queja.

    cd raspberry/server && python -m unittest discover -s tests
"""
from __future__ import annotations
import unittest

import models
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


if __name__ == "__main__":
    unittest.main()
