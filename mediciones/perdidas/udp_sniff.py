#!/usr/bin/env python3
"""Captura en crudo los datagramas UDP a un puerto en una interfaz (sin tcpdump).
Uso: sudo python3 udp_sniff.py IFACE PUERTO SEGUNDOS SALIDA
Escribe una línea por datagrama: epoch, ip origen, payload en hex."""
import socket, struct, sys, time
iface, port, secs, out = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0800))
s.bind((iface, 0)); s.settimeout(1.0)
end = time.time() + secs
with open(out, "w", encoding="utf-8") as f:
    while time.time() < end:
        try:
            fr = s.recv(65535)
        except socket.timeout:
            continue
        ip = fr[14:]
        if ip[9] != 17:
            continue
        ihl = (ip[0] & 15) * 4
        sport, dport = struct.unpack("!HH", ip[ihl:ihl + 4])
        if dport != port:
            continue
        f.write(f"{time.time():.6f},{socket.inet_ntoa(ip[12:16])},{ip[ihl + 8:].hex()}\n"); f.flush()
