"""Acceso a Postgres: pool de conexiones + las cuatro tablas de nebulaedge_schema.

Movido desde classes.py sin cambios de lógica. DatabaseRepository es el
único punto del server que sabe que existe Postgres; PacketRouter
(router.py) lo consume por duck typing vía el Protocol
`TelemetryRepository`, sin importar esta clase.
"""
from __future__ import annotations
from contextlib import contextmanager
from datetime import datetime
from psycopg2.pool import ThreadedConnectionPool
import asyncio
import threading

from models import ConfigData, Data_1, Data_2, Log
from system import utc_epoch_now, LocalWifiConfig


class DatabaseRepository:
    """Repositorio general para interactuar con la BD.

    OJO: antes cada método (get_config/insert_data_1/insert_data_2/insert_log)
    hacía `with psycopg2.connect(self.db_dsn) as db:` por llamada. Ese "with"
    de psycopg2 solo hace commit/rollback al salir, NO cierra la conexión: quedaba
    una conexión abierta sin cerrar por cada paquete de telemetría insertado.
    Con 2 inserts por send_interval_s por device (Data_1 + Data_2), eso agota
    max_connections de Postgres en minutos. Ahora se pide/devuelve una conexión
    de un pool compartido (ver _connection()), del mismo modo para todos los
    métodos, sin cambiar ninguna de sus firmas ni la forma en que se instancia
    DatabaseRepository en el resto del archivo.
    """

    # Un pool por DSN, compartido por todas las instancias de DatabaseRepository
    # (el código de más abajo instancia esta clase "al pasar" en varios sitios,
    # p.ej. `DatabaseRepository(self.db_dsn).insert_log_async(...)`; si el pool
    # fuera de instancia, cada una de esas instancias efímeras abriría el suyo).
    # ThreadedConnectionPool porque insert_*_async/get_config_async corren en
    # threads del pool de asyncio.to_thread, no todos en el mismo hilo.
    _pools: dict[str, ThreadedConnectionPool] = {}
    _pools_lock = threading.Lock()

    def __init__(self, db_dsn: str):
        """Guarda DSN para pedir conexiones al pool compartido."""
        self.db_dsn = db_dsn

    def _get_pool(self) -> ThreadedConnectionPool:
        db_pool = self._pools.get(self.db_dsn)
        if db_pool is not None:
            return db_pool
        with self._pools_lock:
            db_pool = self._pools.get(self.db_dsn)
            if db_pool is None:
                # minconn=1: no abre conexiones de más si nunca se usa este DSN.
                # maxconn=20: generoso para la cantidad de devices esperada y
                # cómodo bajo el max_connections=100 por defecto de Postgres.
                db_pool = ThreadedConnectionPool(1, 20, self.db_dsn)
                self._pools[self.db_dsn] = db_pool
        return db_pool

    @contextmanager
    def _connection(self):
        """Pide una conexión del pool y siempre la devuelve al salir.

        Si algo falla dentro del `with`, hace rollback antes de devolverla:
        sin esto, una conexión reciclada del pool quedaría con una
        transacción abierta y la siguiente consulta que la reciba fallaría
        con "current transaction is aborted".
        """
        db_pool = self._get_pool()
        conn = db_pool.getconn()
        try:
            yield conn
        except Exception:
            conn.rollback()
            raise
        finally:
            db_pool.putconn(conn)

    @staticmethod
    def _int_to_db_datetime(value: int) -> datetime:
        """Convierte un epoch Unix (segundos, UTC real) a datetime naive UTC
        para guardar en columnas TIMESTAMP (sin huso horario) de Postgres.

        Asume que `value` es un epoch UTC de verdad. Antes no lo era
        siempre: `time_client` (del device) sí, pero `time_server`
        (calculado acá con la vieja `local_epoch_now()`) traía sumado el
        offset horario local, así que dos columnas de la misma fila de
        `log` quedaban en escalas de tiempo distintas. Ver utc_epoch_now()
        en system.py.
        """
        return datetime.utcfromtimestamp(int(value))

    def get_config(self, device_id: str) -> ConfigData | None:
        """Obtiene la configuración de un dispositivo específico."""
        with self._connection() as db:
            with db.cursor() as cursor:
                cursor.execute("""
                    SELECT id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
                        bme688_sampling, send_interval_s, sleep_time_s, sleep_window_size,
                        tcp_port, udp_port, mqtt_broker
                    FROM nebulaedge_schema.config
                    WHERE id_device = %s;
                """, (device_id,))

                row = cursor.fetchone()

                if row:
                    host_ip_addr, ssid, passwd = LocalWifiConfig.get(cache_ttl_sec=0)
                    time_cli = utc_epoch_now()
                    return ConfigData(
                        id_device=row[0],
                        config_version=row[1],
                        protocol_conf=row[2],
                        acc_sampling=row[3],
                        gyro_sensibility=row[4],
                        bme688_sampling=row[5],
                        send_interval_s=row[6],
                        sleep_time_s=row[7],
                        sleep_window_size=row[8],
                        tcp_port=row[9],
                        udp_port=row[10],
                        host_ip_addr=host_ip_addr,
                        ssid=ssid,
                        passwd=passwd,
                        mqtt_broker=row[11],
                        time_client=time_cli        # Timestamp en segundos
                    )
                else:
                    return None

    def insert_data_1(self, data_1: Data_1):
        """Inserta datos de sensor en la BD."""
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.data_1 (
                            id_device, temperature, press, hum, co, rms,
                            amp_x, freq_x, amp_y, freq_y, amp_z, freq_z,
                            mag_x, mag_y, mag_z, config_version_applied, time_client
                        ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                    """, (
                        data_1.id_device,
                        data_1.temperature,
                        data_1.press,
                        data_1.hum,
                        data_1.co,
                        data_1.rms,
                        data_1.amp_x,
                        data_1.freq_x,
                        data_1.amp_y,
                        data_1.freq_y,
                        data_1.amp_z,
                        data_1.freq_z,
                        data_1.mag_x,
                        data_1.mag_y,
                        data_1.mag_z,
                        data_1.config_version_applied,
                        self._int_to_db_datetime(data_1.time_client)
                    ))
                    db.commit()
        except AttributeError:
            return
        except Exception as e:
            print(e)

    def insert_data_2(self, data_2: "Data_2"):
        """Inserta datos Data_2 en la BD."""
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.data_2 (
                            id_device, acc_x, acc_y, acc_z,
                            gyr_x, gyr_y, gyr_z,
                            config_version_applied, time_client
                        ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
                    """, (
                        data_2.id_device,
                        data_2.acc_x,
                        data_2.acc_y,
                        data_2.acc_z,
                        data_2.gyr_x,
                        data_2.gyr_y,
                        data_2.gyr_z,
                        data_2.config_version_applied,
                        self._int_to_db_datetime(data_2.time_client),
                    ))
                    db.commit()
        except AttributeError:
            return
        except Exception as e:
            print(e)

    def insert_log(self, log: "Log"):
        """Inserta un evento de log en la BD."""
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.log (
                            id_device, status_report, protocol_report, batt_level,
                            time_client, time_server
                        ) VALUES (%s, %s, %s, %s, %s, %s)
                    """, (
                        log.id_device,
                        log.status_report,
                        log.protocol_report,
                        log.batt_level,
                        self._int_to_db_datetime(log.time_client),
                        self._int_to_db_datetime(log.time_server),
                    ))
                    db.commit()
        except AttributeError:
            return
        except Exception as e:
            print(e)

    # psycopg2 es sincrónico/bloqueante: cada método de arriba abre su propia
    # conexión y espera la red. Llamado directo desde una corutina, congela
    # el event loop completo (todas las demás sesiones de devices se detienen).
    # Estos envoltorios delegan al thread pool de asyncio.
    async def get_config_async(self, device_id: str) -> ConfigData | None:
        return await asyncio.to_thread(self.get_config, device_id)

    async def insert_data_1_async(self, data_1: "Data_1") -> None:
        await asyncio.to_thread(self.insert_data_1, data_1)

    async def insert_data_2_async(self, data_2: "Data_2") -> None:
        await asyncio.to_thread(self.insert_data_2, data_2)

    async def insert_log_async(self, log: "Log") -> None:
        await asyncio.to_thread(self.insert_log, log)
