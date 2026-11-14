#!/usr/bin/env python3
"""Coordinator-side test client. Usage: client.py <wii-ip> [port] [count]"""
import socket, struct, sys, time

MAGIC = 0x57525431
HDR = struct.Struct(">IHHII")           # magic, type, flags, request_id, payload_len
MSG_PING, MSG_PONG, MSG_ERROR = 0x0001, 0x0002, 0xFFFF

def recv_all(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return bytes(buf)

def request(s, mtype, payload, rid):
    s.sendall(HDR.pack(MAGIC, mtype, 0, rid, len(payload)) + payload)
    magic, rtype, _flags, rrid, plen = HDR.unpack(recv_all(s, HDR.size))
    assert magic == MAGIC, "bad magic in response"
    assert rrid == rid, f"request_id mismatch {rrid} != {rid}"
    return rtype, recv_all(s, plen)

def main():
    host  = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port  = int(sys.argv[2]) if len(sys.argv) > 2 else 5555
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 20

    with socket.create_connection((host, port), timeout=5) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        rtts = []
        for i in range(count):
            payload = f"hello {i}".encode()
            t0 = time.perf_counter()
            rtype, body = request(s, MSG_PING, payload, i)
            rtts.append((time.perf_counter() - t0) * 1000)
            assert rtype == MSG_PONG and body == payload, (rtype, body)
        print(f"{count} pings ok, rtt min/avg/max = "
              f"{min(rtts):.2f}/{sum(rtts)/len(rtts):.2f}/{max(rtts):.2f} ms")

        big = bytes(range(256)) * 1024                    # 256 KiB: exercises the send/recv loops
        rtype, body = request(s, MSG_PING, big, 1000)
        assert rtype == MSG_PONG and body == big
        print("256 KiB echo ok")

        rtype, body = request(s, 0x1234, b"", 2000)       # unknown type -> MSG_ERROR, connection survives
        assert rtype == MSG_ERROR
        print("unknown type -> MSG_ERROR code", struct.unpack(">I", body)[0])
        rtype, body = request(s, MSG_PING, b"still alive", 2001)
        assert rtype == MSG_PONG and body == b"still alive"
        print("connection survived the error")

if __name__ == "__main__":
    main()
