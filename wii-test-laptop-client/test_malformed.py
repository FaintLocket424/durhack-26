#!/usr/bin/env python3
"""Robustness checks: the node must survive hostile or broken input.
Usage: test_malformed.py <ip> [port]"""
import socket, struct, sys

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555
MAGIC, HDR = 0x57525431, struct.Struct(">IHHII")

def conn():
    return socket.create_connection((host, port), timeout=5)

def ping_ok():
    with conn() as s:
        s.sendall(HDR.pack(MAGIC, 1, 0, 7, 3) + b"abc")
        assert s.recv(64)[16:] == b"abc"

with conn() as s:                       # not our protocol at all
    s.sendall(b"GET / HTTP/1.1\r\n\r\n")
    t = HDR.unpack(s.recv(64)[:16])[1]
    assert t == 0xFFFF, hex(t)
print("garbage            -> MSG_ERROR, dropped"); ping_ok()

with conn() as s:                       # absurd payload length
    s.sendall(HDR.pack(MAGIC, 1, 0, 1, 0xFFFFFFFF))
    assert HDR.unpack(s.recv(64)[:16])[1] == 0xFFFF
print("oversize length    -> MSG_ERROR, dropped"); ping_ok()

with conn() as s:                       # client vanishes mid-payload
    s.sendall(HDR.pack(MAGIC, 1, 0, 1, 100) + b"short")
print("truncated payload  -> dropped quietly"); ping_ok()

with conn() as s:                       # header sent one byte at a time
    for b in HDR.pack(MAGIC, 1, 0, 9, 2) + b"hi":
        s.sendall(bytes([b]))
    assert s.recv(64)[16:] == b"hi"
print("byte-at-a-time     -> reassembled correctly"); ping_ok()
print("all good")
