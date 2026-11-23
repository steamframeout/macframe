import socket, struct, sys, threading
PORT = int(sys.argv[1])
def dev(busid, busnum, devnum, speed, vid, pid, nif):
    path = ("/sys/devices/pci0000:00/usb%d/%s" % (busnum, busid)).encode().ljust(256, b"\0")
    return path + busid.encode().ljust(32, b"\0") + struct.pack(">IIIHHHBBBBBB", busnum, devnum, speed, vid, pid, 0x100, 0, 0, 0, 1, 1, nif)
devs = [(dev("1-1", 1, 2, 3, 0x046d, 0xc52b, 2), 2), (dev("2-1", 2, 3, 5, 0x28de, 0x2432, 1), 1)]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", PORT)); s.listen(5)
held = []
while True:
    c, _ = s.accept()
    hdr = c.recv(8, socket.MSG_WAITALL)
    ver, code, st = struct.unpack(">HHI", hdr)
    if code == 0x8005:
        out = struct.pack(">HHII", 0x111, 5, 0, len(devs))
        for d, nif in devs: out += d + b"\xff\x00\x00\x00" * nif
        c.sendall(out); c.close(); print("devlist", flush=True)
    elif code == 0x8003:
        busid = c.recv(32, socket.MSG_WAITALL).rstrip(b"\0").decode()
        print("import", busid, flush=True)
        d = [d for d, _ in devs if d[256:288].rstrip(b"\0").decode() == busid]
        if d: c.sendall(struct.pack(">HHI", 0x111, 3, 0) + d[0]); held.append(c)
        else: c.sendall(struct.pack(">HHI", 0x111, 3, 1)); c.close()
