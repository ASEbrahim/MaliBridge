#!/usr/bin/env python3
"""Two tiny Wayland compositors for host/zerocopy_nested_test.sh (the wire protocol by hand, no libraries).

  "gamescope" (--nested SOCKET): wl_compositor, wl_surface, zwp_linux_dmabuf_v1 version 3, no
      banner_ahb_v1 - what a program under DroidDeck's nested gamescope sees. Every dma-buf wl_buffer
      it is given is recorded (the dma-buf's inode), as gamescope would pass it on to the app's
      compositor; each commit releases the previous buffer and answers the frame callback.
  "the app's compositor" (--host SOCKET): banner_ahb_v1 at --host-version (default 3) with mode
      --host-mode (default 1). Every offer is recorded: the id, the size, and the inode of the dma-buf
      read from the offered socket (the host stand-in's "handle", tests/... VKBRIDGE_TEST_SCANOUT=1).

Pacing (host/present_path_test.sh), off by default:
  --release-delay-ms D: the previous buffer's wl_buffer.release goes D ms after the commit that replaced it
      (a compositor still reading it), instead of at once.
  --vblank-ms V: gamescope's FIFO (its WSI layer gives the driver MAILBOX and paces in gamescope): commits queue
      up; every V ms the oldest queued buffer is latched (logged as {"ev": "latch", "t": seconds, "buf", "px"}),
      its frame callbacks answered, and the buffer it replaced released --release-ms R later (default 1).
  --pixels: each dma-buf's first pixel (4 bytes at its plane offset, read with pread: the test's memfd stand-ins)
      logged as "px" when it goes on screen and again just before its release ({"ev": "release", "buf", "px"}):
      a frame's number the test drew, so the order of frames and an image drawn into while the compositor held
      it (tearing) show.
  --drop-after N: the connection closed after N commits (a compositor gone): the client's swapchain is lost.

The order of the first events (host/wlbind_test.sh), off by default:
  --shm: "gamescope" also offers wl_shm (formats ARGB8888, XRGB8888, ABGR8888, XBGR8888 on bind); each wl_shm buffer
      is logged as {"ev": "shm_buffer", "id", "width", "height", "stride", "format"}.
  --late-registry-done: the wl_display.sync that follows a get_registry is answered only after the client's next
      requests (its binds) have been answered, so the bound globals' first events (dma-buf formats and modifiers,
      wl_shm formats) reach the client before that sync's done, in the same roundtrip. A client sees the same
      order when another of its threads reads the connection during that roundtrip (an old swapchain's presenting
      thread during a recreate). A client that binds nothing gets the done after 2 s.

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
import time

DISPLAY, REGISTRY, CALLBACK, COMPOSITOR, SURFACE, BUFFER, DMABUF, PARAMS, AHB, SHM, POOL = (
    "wl_display", "wl_registry", "wl_callback", "wl_compositor", "wl_surface", "wl_buffer",
    "zwp_linux_dmabuf_v1", "zwp_linux_buffer_params_v1", "banner_ahb_v1", "wl_shm", "wl_shm_pool")
FOURCC = lambda s: s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24  # noqa: E731
FORMATS = [FOURCC(b"AR24"), FOURCC(b"XR24"), FOURCC(b"AB24"), FOURCC(b"XB24")]
SHM_FORMATS = [0, 1, FOURCC(b"AB24"), FOURCC(b"XB24")]  # wl_shm's own codes for ARGB8888 and XRGB8888


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
        self.planes = {}  # buffer id -> (fd, offset) (--pixels)
        self.dropped = False
        self.registry_fresh = False  # --late-registry-done: a get_registry whose sync is not answered yet
        self.held = None  # ... that sync's callback, held
        self.after_held = False  # ... and a request came after it

    def send(self, obj, opcode, payload=b""):
        try:
            self.sock.sendall(struct.pack("<II", obj, (8 + len(payload)) << 16 | opcode) + payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the client went (device_select closes right after a sync's done): its EOF ends the connection

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
        if self.held and self.after_held:
            self.release_held()
        return True

    def release_held(self):
        cb, self.held, self.after_held = self.held, None, False
        if cb and not self.dropped:
            try:
                self.send(cb, 0, struct.pack("<I", 0))
                self.send(1, 1, struct.pack("<I", cb))
            except OSError:
                pass

    def pixel(self, buf):
        p = self.planes.get(buf)
        if not p:
            return None
        try:
            b = os.pread(p[0], 4, p[1])
        except OSError:
            return None
        return struct.unpack("<I", b)[0] if len(b) == 4 else None

    def release_later(self, buf, delay):
        def go():
            if self.dropped or self.objs.get(buf) != BUFFER:
                return
            if self.server.pixels:
                self.server.log(ev="release", buf=buf, px=self.pixel(buf))
            try:
                self.send(buf, 0)  # wl_buffer.release
            except OSError:
                pass
        if delay <= 0:
            go()
        else:
            self.server.timers.append((time.monotonic() + delay, go))

    def answer_frames(self, cbs):
        for cb in cbs:
            self.send(cb, 0, struct.pack("<I", 0))
            self.send(1, 1, struct.pack("<I", cb))
            self.objs.pop(cb, None)

    def show(self, surf, buf):
        """buf goes on screen: the one it replaced is released."""
        srv = self.server
        old = self.attached.get(surf)
        if old and old != buf:
            self.release_later(old, srv.release_delay if not srv.vblank else srv.release_after_latch)
        self.attached[surf] = buf
        srv.commits += 1
        if srv.vblank or srv.pixels:
            srv.log(ev="latch", t=round(time.monotonic(), 6), buf=buf, px=self.pixel(buf) if srv.pixels else None)

    def u(self, body, n):
        return struct.unpack_from("<%dI" % n, body)

    def string(self, body, off):
        (ln,) = struct.unpack_from("<I", body, off)
        s = body[off + 4: off + 4 + ln - 1].decode()
        return s, off + 4 + ((ln + 3) & ~3)

    def request(self, obj, op, body):
        kind = self.objs.get(obj)
        srv = self.server
        if self.held:
            self.after_held = True
        if kind == DISPLAY:
            (new,) = self.u(body, 1)
            if op == 0 and self.registry_fresh:  # sync after get_registry, --late-registry-done
                self.registry_fresh = False
                self.held, self.after_held = new, False
                srv.timers.append((time.monotonic() + 2.0, lambda: self.held == new and self.release_held()))
            elif op == 0:  # sync
                self.send(new, 0, struct.pack("<I", 0))
                self.send(1, 1, struct.pack("<I", new))
            elif op == 1:  # get_registry
                self.objs[new] = REGISTRY
                self.registry_fresh = srv.late_registry_done
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
            elif iface == SHM:
                for f in SHM_FORMATS:
                    self.send(new, 0, struct.pack("<I", f))
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
                srv.commits_in += 1
                if srv.vblank:
                    srv.fifo.setdefault((self, obj), []).append((buf, self.data.pop(("frames", obj), [])))
                else:
                    if buf:
                        self.show(obj, buf)
                    self.answer_frames(self.data.pop(("frames", obj), []))
                if srv.drop_after and srv.commits_in >= srv.drop_after and not self.dropped:
                    srv.log(ev="drop", commits=srv.commits_in)
                    self.dropped = True
        elif kind == BUFFER and op == 0:
            self.objs.pop(obj, None)
            p = self.planes.pop(obj, None)
            if p:
                os.close(p[0])
            self.send(1, 1, struct.pack("<I", obj))
        elif kind == SHM and op == 0:  # create_pool(new id, fd, size)
            (new,) = self.u(body, 1)
            self.objs[new] = POOL
            os.close(self.fds.pop(0))
        elif kind == POOL:
            if op == 0:  # create_buffer(new id, offset, width, height, stride, format)
                new, off, w, h, stride, fmt = struct.unpack_from("<IiiiiI", body)
                self.objs[new] = BUFFER
                srv.log(ev="shm_buffer", id=new, width=w, height=h, stride=stride, format=fmt)
            elif op == 1:  # destroy
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
                    srv.log(ev="buffer", id=new, ino=ino_of(fd), width=w, height=h, format=fmt, plane=plane, offset=offset,
                            stride=stride, modifier=mod)
                    if srv.pixels and plane == 0 and new not in self.planes:
                        self.planes[new] = (fd, offset)
                    else:
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
        self.commits_in = 0
        self.release_delay = 0.0
        self.vblank = 0.0
        self.release_after_latch = 0.001
        self.pixels = False
        self.drop_after = 0
        self.late_registry_done = False
        self.fifo = {}  # (conn, surface) -> [(buffer, frame callbacks)] (vblank mode)
        self.timers = []  # (monotonic due, fn)
        if os.path.exists(path):
            os.unlink(path)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(path)
        self.listener.listen(8)

    def latch(self):
        """One vblank: the oldest queued commit of each surface goes on screen (vblank mode)."""
        for (conn, surf), q in list(self.fifo.items()):
            if not q or conn.dropped:
                continue
            buf, cbs = q.pop(0)
            try:
                if buf:
                    conn.show(surf, buf)
                conn.answer_frames(cbs)
            except OSError:
                self.fifo.pop((conn, surf), None)

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
    ap.add_argument("--release-delay-ms", type=float, default=0)
    ap.add_argument("--vblank-ms", type=float, default=0)
    ap.add_argument("--release-ms", type=float, default=1)
    ap.add_argument("--pixels", action="store_true")
    ap.add_argument("--drop-after", type=int, default=0)
    ap.add_argument("--shm", action="store_true")
    ap.add_argument("--late-registry-done", action="store_true")
    a = ap.parse_args()
    logf = open(a.log, "a")
    nested = Server("nested", a.nested, [(COMPOSITOR, 4), (DMABUF, 3)] + ([(SHM, 1)] if a.shm else []), 0, logf)
    host_globals = [(COMPOSITOR, 4)] + ([] if a.no_host_ahb else [(AHB, a.host_version)])
    host = Server("host", a.host, host_globals, a.host_mode, logf)
    sel = selectors.DefaultSelector()
    for srv in (nested, host):
        sel.register(srv.listener, selectors.EVENT_READ, ("listen", srv))
    nested.release_delay = a.release_delay_ms / 1000
    nested.vblank = a.vblank_ms / 1000
    nested.release_after_latch = a.release_ms / 1000
    nested.pixels = a.pixels
    nested.drop_after = a.drop_after
    nested.late_registry_done = a.late_registry_done
    next_vb = time.monotonic() + nested.vblank
    print("ready", flush=True)
    while True:
        now = time.monotonic()
        due = [t for t, _ in nested.timers] + ([next_vb] if nested.vblank else [])
        timeout = max(0.0, min(due) - now) if due else None
        for key, _ in sel.select(timeout):
            what, x = key.data
            if what == "listen":
                c, _ = x.listener.accept()
                sel.register(c, selectors.EVENT_READ, ("conn", Conn(x, c)))
                x.log(ev="connect")
            else:
                if not x.read() or x.dropped:
                    sel.unregister(x.sock)
                    x.sock.close()
                    x.dropped = True
                    x.server.log(ev="disconnect")
                    for k in [k for k in x.server.fifo if k[0] is x]:
                        x.server.fifo.pop(k, None)
                    for fd, _ in x.planes.values():
                        os.close(fd)
                    x.planes = {}
        now = time.monotonic()
        if nested.vblank and now >= next_vb:
            nested.latch()
            next_vb += nested.vblank
            if next_vb < now:  # fell behind (a stall): the next vblank from now
                next_vb = now + nested.vblank
        if nested.timers:
            ready = [x for x in nested.timers if x[0] <= now]
            nested.timers = [x for x in nested.timers if x[0] > now]
            for _, fn in ready:
                fn()


if __name__ == "__main__":
    sys.exit(main())
