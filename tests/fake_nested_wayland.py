#!/usr/bin/env python3
"""Two tiny Wayland compositors for host/zerocopy_nested_test.sh (the wire protocol by hand, no libraries).

  "gamescope" (--nested SOCKET): wl_compositor, wl_surface, zwp_linux_dmabuf_v1 version 3, no
      banner_ahb_v1 - what a program under DroidDeck's nested gamescope sees. Every dma-buf wl_buffer
      it is given is recorded (the dma-buf's inode), as gamescope would pass it on to the app's
      compositor; each commit releases the previous buffer and answers the frame callback.
  "the app's compositor" (--host SOCKET): banner_ahb_v1 at --host-version (default 3) with mode
      --host-mode (default 1). Every offer is recorded: the id, the size, and the inode of the dma-buf
      read from the offered socket (the host stand-in's "handle", tests/... VKBRIDGE_TEST_SCANOUT=1).

Events go to --log as JSON lines: {"who": "nested"|"host", "ev": ..., ...}. Runs until killed.
"""
import argparse
import array
import json
import os
import selectors
import socket
import struct
import sys

DISPLAY, REGISTRY, CALLBACK, COMPOSITOR, SURFACE, BUFFER, DMABUF, PARAMS, AHB = (
    "wl_display", "wl_registry", "wl_callback", "wl_compositor", "wl_surface", "wl_buffer",
    "zwp_linux_dmabuf_v1", "zwp_linux_buffer_params_v1", "banner_ahb_v1")
FOURCC = lambda s: s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24  # noqa: E731
FORMATS = [FOURCC(b"AR24"), FOURCC(b"XR24"), FOURCC(b"AB24"), FOURCC(b"XB24")]


def ino_of(fd):
    st = os.fstat(fd)
    return st.st_ino


class Conn:
    def __init__(self, server, sock):
        self.server = server
        self.sock = sock
        self.buf = b""
        self.fds = []
        self.objs = {1: DISPLAY}
        self.data = {}
        self.attached = {}  # surface id -> buffer id

    def send(self, obj, opcode, payload=b""):
        self.sock.sendall(struct.pack("<II", obj, (8 + len(payload)) << 16 | opcode) + payload)

    def read(self):
        try:
            msg, anc, _, _ = self.sock.recvmsg(65536, socket.CMSG_SPACE(28 * 4))
        except (ConnectionResetError, BlockingIOError):
            msg, anc = b"", []
        for level, kind, data in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                a = array.array("i")
                a.frombytes(data[: len(data) - len(data) % 4])
                self.fds.extend(a)
        if not msg:
            return False
        self.buf += msg
        while len(self.buf) >= 8:
            obj, word = struct.unpack_from("<II", self.buf)
            size, op = word >> 16, word & 0xFFFF
            if size < 8 or len(self.buf) < size:
                break
            body, self.buf = self.buf[8:size], self.buf[size:]
            self.request(obj, op, body)
        return True

    def u(self, body, n):
        return struct.unpack_from("<%dI" % n, body)

    def string(self, body, off):
        (ln,) = struct.unpack_from("<I", body, off)
        s = body[off + 4: off + 4 + ln - 1].decode()
        return s, off + 4 + ((ln + 3) & ~3)

    def request(self, obj, op, body):
        kind = self.objs.get(obj)
        srv = self.server
        if kind == DISPLAY:
            (new,) = self.u(body, 1)
            if op == 0:  # sync
                self.send(new, 0, struct.pack("<I", 0))
                self.send(1, 1, struct.pack("<I", new))
            elif op == 1:  # get_registry
                self.objs[new] = REGISTRY
                for name, (iface, ver) in enumerate(srv.globals, 1):
                    enc = iface.encode() + b"\0"
                    pad = enc + b"\0" * ((4 - len(enc) % 4) % 4)
                    self.send(new, 0, struct.pack("<II", name, len(enc)) + pad + struct.pack("<I", ver))
        elif kind == REGISTRY and op == 0:  # bind
            (name,) = self.u(body, 1)
            iface, off = self.string(body, 4)
            ver, new = struct.unpack_from("<II", body, off)
            self.objs[new] = iface
            if iface == DMABUF:
                for f in FORMATS:
                    self.send(new, 0, struct.pack("<I", f))
                    if ver >= 3:
                        self.send(new, 1, struct.pack("<III", f, 0, 0))
            elif iface == AHB:
                srv.log(ev="bind", version=ver)
                if ver >= 2:
                    self.send(new, 0, struct.pack("<I", srv.mode))
        elif kind == COMPOSITOR and op == 0:
            (new,) = self.u(body, 1)
            self.objs[new] = SURFACE
        elif kind == SURFACE:
            if op == 1:  # attach
                (buf,) = self.u(body, 1)
                self.data[("pending", obj)] = buf
            elif op == 3:  # frame
                (new,) = self.u(body, 1)
                self.objs[new] = CALLBACK
                self.data.setdefault(("frames", obj), []).append(new)
            elif op == 6:  # commit
                buf = self.data.pop(("pending", obj), None)
                if buf:
                    old = self.attached.get(obj)
                    if old and old != buf and self.objs.get(old) == BUFFER:
                        self.send(old, 0)  # wl_buffer.release
                    self.attached[obj] = buf
                    srv.commits += 1
                for cb in self.data.pop(("frames", obj), []):
                    self.send(cb, 0, struct.pack("<I", 0))
                    self.send(1, 1, struct.pack("<I", cb))
                    self.objs.pop(cb, None)
        elif kind == BUFFER and op == 0:
            self.objs.pop(obj, None)
            self.send(1, 1, struct.pack("<I", obj))
        elif kind == DMABUF and op == 1:
            (new,) = self.u(body, 1)
            self.objs[new] = PARAMS
            self.data[("planes", new)] = []
        elif kind == PARAMS:
            if op == 1:  # add
                fd = self.fds.pop(0)
                plane, offset, stride, hi, lo = self.u(body, 5)
                self.data[("planes", obj)].append((fd, plane, offset, stride, hi << 32 | lo))
            elif op == 3:  # create_immed
                new, w, h, fmt, flags = struct.unpack_from("<IiiII", body)
                planes = self.data.pop(("planes", obj), [])
                self.objs[new] = BUFFER
                for fd, plane, offset, stride, mod in planes:
                    srv.log(ev="buffer", ino=ino_of(fd), width=w, height=h, format=fmt, plane=plane, offset=offset,
                            stride=stride, modifier=mod)
                    os.close(fd)
            elif op == 0:
                for fd, *_ in self.data.pop(("planes", obj), []):
                    os.close(fd)
                self.objs.pop(obj, None)
                self.send(1, 1, struct.pack("<I", obj))
        elif kind == AHB:
            if op == 2:  # offer(fd socket, id, width, height, stride)
                sock_fd = self.fds.pop(0)
                oid, w, h, stride = self.u(body, 4)
                s = socket.socket(fileno=sock_fd)
                s.settimeout(2)
                try:
                    _, anc, _, _ = s.recvmsg(16, socket.CMSG_SPACE(4 * 4))
                except OSError:
                    anc = []
                s.close()
                inos = []
                for level, k, data in anc:
                    if level == socket.SOL_SOCKET and k == socket.SCM_RIGHTS:
                        a = array.array("i")
                        a.frombytes(data[: len(data) - len(data) % 4])
                        for fd in a:
                            inos.append(ino_of(fd))
                            os.close(fd)
                srv.log(ev="offer", id=oid, width=w, height=h, stride=stride, inos=inos)
            elif op == 3:
                (oid,) = self.u(body, 1)
                srv.log(ev="withdraw", id=oid)
            elif op == 1:  # attach: not expected under gamescope
                self.fds.pop(0)
                srv.log(ev="attach")
            elif op == 0:
                self.objs.pop(obj, None)
                self.send(1, 1, struct.pack("<I", obj))


class Server:
    def __init__(self, who, path, globals_, mode, logf):
        self.who, self.globals, self.mode, self.logf = who, globals_, mode, logf
        self.commits = 0
        if os.path.exists(path):
            os.unlink(path)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(8)

    def log(self, **kw):
        kw["who"] = self.who
        self.logf.write(json.dumps(kw) + "\n")
        self.logf.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nested", required=True)
    ap.add_argument("--host", required=True)
    ap.add_argument("--host-version", type=int, default=3)
    ap.add_argument("--host-mode", type=int, default=1)
    ap.add_argument("--no-host-ahb", action="store_true")
    ap.add_argument("--log", required=True)
    a = ap.parse_args()
    logf = open(a.log, "a")
    nested = Server("nested", a.nested, [(COMPOSITOR, 4), (DMABUF, 3)], 0, logf)
    host_globals = [(COMPOSITOR, 4)] + ([] if a.no_host_ahb else [(AHB, a.host_version)])
    host = Server("host", a.host, host_globals, a.host_mode, logf)
    sel = selectors.DefaultSelector()
    for srv in (nested, host):
        sel.register(srv.listener, selectors.EVENT_READ, ("listen", srv))
    print("ready", flush=True)
    while True:
        for key, _ in sel.select():
            what, x = key.data
            if what == "listen":
                c, _ = x.listener.accept()
                sel.register(c, selectors.EVENT_READ, ("conn", Conn(x, c)))
                x.log(ev="connect")
            else:
                if not x.read():
                    sel.unregister(x.sock)
                    x.sock.close()
                    x.server.log(ev="disconnect")


if __name__ == "__main__":
    sys.exit(main())
