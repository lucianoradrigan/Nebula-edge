"""Prueba de deriva del reloj: deja un device en UDP a 1 s, continuo o con deep sleep (sube la versión)."""
import os, sys, time
import psycopg2
sys.path.insert(0, os.path.expanduser("~/Nebula-edge/raspberry/server"))
from system import database_dsn

mac, sleep_s, window = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
con = psycopg2.connect(database_dsn()); con.autocommit = True
cur = con.cursor()
cur.execute("SELECT max(config_version) FROM nebulaedge_schema.config")
v = cur.fetchone()[0] + 1
cur.execute("""UPDATE nebulaedge_schema.config SET config_version=%s, protocol_conf=1, send_interval_s=1,
               sleep_time_s=%s, sleep_window_size=%s WHERE id_device=%s""", (v, sleep_s, window, mac))
print(f"{time.time():.3f} v{v} sleep_time_s={sleep_s} window={window}", flush=True)
