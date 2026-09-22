"""Acceso a Postgres: el único módulo del servidor que sabe que la BD existe.

UTILIDAD PRINCIPAL
    Leer la configuración de cada dispositivo y escribir su telemetría y sus
    logs, contra las cuatro tablas de `nebulaedge_schema`:

        config         una fila por device; es la fuente de verdad de qué
                       protocolo y qué parámetros de sensor le tocan
        environmental  telemetría del BME688, al ritmo de env_interval_s
        inertial       BMI270 + BMM350, al ritmo de send_interval_s
        log            eventos de operación: conexión, heartbeat, desconexión

USAR SIEMPRE LOS MÉTODOS *_async DESDE CORUTINAS
    psycopg2 es bloqueante. Llamar `get_config()` directo desde una corutina
    congela el event loop entero -y con él, las sesiones de todos los demás
    devices- mientras dura la consulta. Los envoltorios `*_async` delegan en
    `asyncio.to_thread` justamente para evitar eso.

POOL DE CONEXIONES
    Las conexiones salen de un pool compartido por DSN (`_connection()`), no de
    un `psycopg2.connect()` por llamada. Con dos inserts por intervalo de envío
    por device, abrir una conexión por insert agota el `max_connections` de
    Postgres en minutos. El pool es de clase, no de instancia, porque esta
    clase se instancia "al pasar" en varios puntos del código.

ACOPLAMIENTO
    `PacketRouter` (packet_router.py) usa esta clase por duck typing, vía el Protocol
    `TelemetryRepository`: nunca la importa. Por eso los tests le pueden pasar
    un repositorio en memoria sin levantar Postgres.
"""
from __future__ import annotations
from contextlib import contextmanager
from datetime import datetime
from psycopg2.pool import ThreadedConnectionPool
import asyncio
import threading

from models import ConfigData, Environmental, Inertial, Log
from system import utc_epoch_now, LocalWifiConfig


class DatabaseRepository:
    """Repositorio único de acceso a la base de datos.

    Se instancia libremente donde se necesite -incluso "al pasar", como
    `DatabaseRepository(dsn).insert_log_async(...)`- porque no guarda estado
    propio: solo el DSN. Las conexiones salen del pool compartido de clase.

    Cada operación existe en dos versiones con la misma firma: la sincrónica
    (`get_config`) y la que hay que usar desde una corutina (`get_config_async`).
    """

    # Un pool por DSN, compartido por TODAS las instancias de esta clase: como
    # se construyen instancias efímeras en varios puntos, un pool por instancia
    # significaría abrir un pool nuevo cada vez.
    # Es ThreadedConnectionPool y no SimpleConnectionPool porque los métodos
    # *_async corren en hilos distintos del pool de asyncio.to_thread.
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
        """Convierte un epoch Unix (segundos, UTC) al datetime naive que
        esperan las columnas TIMESTAMP (sin huso horario) de Postgres.

        Asume que `value` es un epoch UTC real, no uno con el offset horario
        local ya sumado. Todo el sistema trabaja en UTC justamente para que
        columnas como `time_client` (que viene del device) y `time_server`
        (calculado en el servidor con `utc_epoch_now()`, system.py) queden en
        la misma escala dentro de una misma fila.
        """
        return datetime.utcfromtimestamp(int(value))

    def get_config(self, device_id: str) -> ConfigData | None:
        """Obtiene la configuración de un dispositivo específico."""
        with self._connection() as db:
            with db.cursor() as cursor:
                cursor.execute("""
                    SELECT id_device, config_version, protocol_conf, acc_sampling, gyro_sensibility,
                        bme688_sampling, send_interval_s, env_interval_s, sleep_time_s,
                        sleep_window_size, tcp_port, udp_port, mqtt_broker
                    FROM nebulaedge_schema.config
                    WHERE id_device = %s;
                """, (device_id,))

                row = cursor.fetchone()

                if row:
                    # Con el TTL por defecto: sin esto, cada get_config lanzaba
                    # cuatro subprocesos nmcli. El SSID y la IP del host no
                    # cambian entre dos lecturas seguidas de la misma tabla.
                    host_ip_addr, ssid, passwd = LocalWifiConfig.get()
                    time_cli = utc_epoch_now()
                    return ConfigData(
                        id_device=row[0],
                        config_version=row[1],
                        protocol_conf=row[2],
                        acc_sampling=row[3],
                        gyro_sensibility=row[4],
                        bme688_sampling=row[5],
                        send_interval_s=row[6],
                        env_interval_s=row[7] or 0,
                        sleep_time_s=row[8],
                        sleep_window_size=row[9],
                        tcp_port=row[10],
                        udp_port=row[11],
                        host_ip_addr=host_ip_addr,
                        ssid=ssid,
                        passwd=passwd,
                        mqtt_broker=row[12],
                        time_client=time_cli        # Timestamp en segundos
                    )
                else:
                    return None

    def insert_environmental(self, environmental: Environmental) -> bool:
        """Inserta telemetría ambiental (BME688). True si la fila quedó guardada.

        No propaga: un fallo de base no debe cortar la sesión con un device que
        sigue vivo y mandando. Pero sí lo REPORTA, que es lo que faltaba: antes
        se tragaba cualquier excepción y PacketRouter informaba TELEMETRY -que
        significa "insertado"- igual.
        """
        _ctx = "environmental"
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.environmental (
                            id_device, temperature, press, hum, co,
                            config_version_applied, time_client
                        ) VALUES (%s, %s, %s, %s, %s, %s, %s)
                    """, (
                        environmental.id_device,
                        environmental.temperature,
                        environmental.press,
                        environmental.hum,
                        environmental.co,
                        environmental.config_version_applied,
                        self._int_to_db_datetime(environmental.time_client)
                    ))
                    db.commit()
            return True
        except AttributeError as e:
            # Un campo que no existe en la dataclass es un bug nuestro, no un
            # fallo de base. Antes se hacía `return` en silencio y el error
            # quedaba invisible para siempre.
            print(f"BUG: {_ctx} tiene un campo que el insert no encontró: {e}")
            return False
        except Exception as e:
            print(f"ERROR de base insertando {_ctx}: {e}")
            return False

    def insert_inertial(self, inertial: "Inertial") -> bool:
        """Inserta telemetría inercial. True si la fila quedó guardada. Ver
        insert_environmental para por qué no propaga pero sí reporta.
        """
        _ctx = "inertial"
        try:
            with self._connection() as db:
                with db.cursor() as cursor:
                    cursor.execute("""
                        INSERT INTO nebulaedge_schema.inertial (
                            id_device, acc_x, acc_y, acc_z,
                            gyr_x, gyr_y, gyr_z,
                            mag_x, mag_y, mag_z,
                            config_version_applied, time_client
                        ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                    """, (
                        inertial.id_device,
                        inertial.acc_x,
                        inertial.acc_y,
                        inertial.acc_z,
                        inertial.gyr_x,
                        inertial.gyr_y,
                        inertial.gyr_z,
                        inertial.mag_x,
                        inertial.mag_y,
                        inertial.mag_z,
                        inertial.config_version_applied,
                        self._int_to_db_datetime(inertial.time_client),
                    ))
                    db.commit()
            return True
        except AttributeError as e:
            # Un campo que no existe en la dataclass es un bug nuestro, no un
            # fallo de base. Antes se hacía `return` en silencio y el error
            # quedaba invisible para siempre.
            print(f"BUG: {_ctx} tiene un campo que el insert no encontró: {e}")
            return False
        except Exception as e:
            print(f"ERROR de base insertando {_ctx}: {e}")
            return False

    def insert_log(self, log: "Log") -> bool:
        """Inserta un evento de log. True si la fila quedó guardada."""
        _ctx = "log"
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
            return True
        except AttributeError as e:
            # Un campo que no existe en la dataclass es un bug nuestro, no un
            # fallo de base. Antes se hacía `return` en silencio y el error
            # quedaba invisible para siempre.
            print(f"BUG: {_ctx} tiene un campo que el insert no encontró: {e}")
            return False
        except Exception as e:
            print(f"ERROR de base insertando {_ctx}: {e}")
            return False

    # psycopg2 es sincrónico/bloqueante: cada método de arriba abre su propia
    # conexión y espera la red. Llamado directo desde una corutina, congela
    # el event loop completo (todas las demás sesiones de devices se detienen).
    # Estos envoltorios delegan al thread pool de asyncio.
    async def get_config_async(self, device_id: str) -> ConfigData | None:
        return await asyncio.to_thread(self.get_config, device_id)

    async def insert_environmental_async(self, environmental: "Environmental") -> bool:
        return await asyncio.to_thread(self.insert_environmental, environmental)

    async def insert_inertial_async(self, inertial: "Inertial") -> bool:
        return await asyncio.to_thread(self.insert_inertial, inertial)

    async def insert_log_async(self, log: "Log") -> bool:
        return await asyncio.to_thread(self.insert_log, log)
