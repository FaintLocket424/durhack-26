#!/usr/bin/env python3
"""Test coordinator: splits an image into tiles, farms them out to several Wii
nodes in parallel (one connection + thread per node), accumulates the float
sums, writes a PPM.

  python3 coordinator.py 192.168.1.50 192.168.1.51 --width 320 --height 240 \
      --tile 16 --spp 4 --passes 16 -o out.ppm
"""
import argparse, queue, socket, struct, threading, time
import numpy as np

MAGIC = 0x57525431
HDR = struct.Struct('>IHHII')
MSG_RENDER_TILE, MSG_TILE_RESULT, MSG_ERROR = 0x0010, 0x0011, 0xFFFF


def recv_all(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("node closed the connection")
        buf += chunk
    return bytes(buf)


def request(s, rid, mtype, payload):
    s.sendall(HDR.pack(MAGIC, mtype, 0, rid, len(payload)) + payload)
    magic, t, _, rid_back, n = HDR.unpack(recv_all(s, HDR.size))
    body = recv_all(s, n)
    if magic != MAGIC or rid_back != rid:
        raise ConnectionError("bad reply frame")
    if t == MSG_ERROR:
        raise RuntimeError("node error %d" % struct.unpack('>I', body)[0])
    return t, body


def worker(addr, jobs, img, lock, args, done):
    host, _, port = addr.partition(':')
    try:
        s = socket.create_connection((host, int(port or 5555)), timeout=600)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError as e:
        print(f"[{addr}] cannot connect: {e}")
        return
    rid = 0
    while True:
        try:
            job = jobs.get_nowait()
        except queue.Empty:
            return
        x0, y0, w, h, seed = job
        rid += 1
        payload = struct.pack('>9I', x0, y0, w, h, args.width, args.height,
                              args.spp, seed, args.depth)
        try:
            t, body = request(s, rid, MSG_RENDER_TILE, payload)
            rx, ry, rw, rh, _ = struct.unpack('>5I', body[:20])
            px = np.frombuffer(body, dtype='>f4', offset=20).reshape(rh, rw, 3)
        except Exception as e:
            print(f"[{addr}] failed ({e}); requeueing tile, dropping node")
            jobs.put(job)
            return
        with lock:
            img[ry:ry + rh, rx:rx + rw] += px
            done[0] += 1
            n = done[0]
        if n % 25 == 0:
            print(f"{n}/{done[1]} jobs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nodes', nargs='+', help="host or host:port")
    ap.add_argument('--width', type=int, default=320)
    ap.add_argument('--height', type=int, default=240)
    ap.add_argument('--tile', type=int, default=16)
    ap.add_argument('--spp', type=int, default=4, help="samples per request")
    ap.add_argument('--passes', type=int, default=16, help="requests per tile")
    ap.add_argument('--depth', type=int, default=6)
    ap.add_argument('-o', '--out', default='out.ppm')
    args = ap.parse_args()

    img = np.zeros((args.height, args.width, 3), dtype=np.float64)
    jobs = queue.Queue()
    jid = 0
    for p in range(args.passes):
        for y in range(0, args.height, args.tile):
            for x in range(0, args.width, args.tile):
                w = min(args.tile, args.width - x)
                h = min(args.tile, args.height - y)
                jid += 1
                seed = (jid * 2654435761) & 0xFFFFFFFF      # unique per request
                jobs.put((x, y, w, h, seed))

    done = [0, jid]
    lock = threading.Lock()
    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(a, jobs, img, lock, args, done))
               for a in args.nodes]
    for t in threads: t.start()
    for t in threads: t.join()

    if not jobs.empty():
        print(f"WARNING: {jobs.qsize()} jobs unfinished (all nodes dropped)")
    total = args.spp * args.passes
    out = np.clip(np.sqrt(img / total), 0, 1)               # gamma
    out = (out * 255 + 0.5).astype(np.uint8)
    with open(args.out, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (args.width, args.height))
        f.write(out.tobytes())
    print(f"{done[0]} jobs in {time.time() - t0:.1f}s -> {args.out}")


if __name__ == '__main__':
    main()
