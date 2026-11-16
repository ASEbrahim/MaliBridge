#!/usr/bin/env python3
# Dynamic lightmaps the way GoldSrc (Half-Life) draws a lit world with the flashlight on, through Zink:
# 128x128 RGBA lightmap pages shared by many surfaces, each surface drawn with two textures (its
# base texture on unit 0, its page on unit 1, GL_MODULATE). A surface the light touches gets its
# lightmap block rebuilt and the page's changed rows written with glTexSubImage2D just before the
# surface is drawn (GLQuake's multitexture path, R_DrawSequentialPoly), so most uploads go into a
# page the frame has already drawn with. a player log (Tab S9 FE, Mali-G68): fps falls
# from 50-60 to 9-26 while ~6,000 such uploads happen in 10 s.
#   gllightmap.py <mode> <lit> <frames> [out.bin]
#     mode  mtex     the uploads between the draws (GoldSrc)
#           batched  the same uploads, all before the frame's first draw (GLQuake's texsort order)
#     lit   surfaces the light touches each frame (0: none)
# Each frame ends with glFlush and a fence; the CPU waits for the fence two frames back (a swap's
# throttle). Prints "RESULT ok frames=F uploads=U crc=C" (crc of the last frame's pixels).
# Compatibility GL over EGL surfaceless, into an FBO with colour and depth.
import ctypes, ctypes.util, sys, time, zlib

mode, lit, frames = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
egl = ctypes.CDLL(ctypes.util.find_library("EGL") or "libEGL.so.1")
gl = ctypes.CDLL(ctypes.util.find_library("GL") or "libGL.so.1")
P = ctypes.c_void_p
egl.eglGetProcAddress.restype = P
egl.eglGetProcAddress.argtypes = [ctypes.c_char_p]
gpd = ctypes.CFUNCTYPE(P, ctypes.c_int, P, P)(egl.eglGetProcAddress(b"eglGetPlatformDisplayEXT"))
for f, a in (("eglInitialize", [P, P, P]), ("eglChooseConfig", [P, P, P, ctypes.c_int, P]), ("eglCreateContext", [P, P, P, P]),
             ("eglMakeCurrent", [P, P, P, P])):
    getattr(egl, f).argtypes = a
egl.eglCreateContext.restype = P
d = gpd(0x31DD, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
assert egl.eglInitialize(d, None, None), "eglInitialize"
assert egl.eglBindAPI(0x30A2), "eglBindAPI(OpenGL)"
attrs = (ctypes.c_int * 5)(0x3033, 0, 0x3040, 0x8, 0x3038)
cfg = P(); n = ctypes.c_int()
assert egl.eglChooseConfig(d, attrs, ctypes.byref(cfg), 1, ctypes.byref(n)) and n.value, "config"
cattrs = (ctypes.c_int * 7)(0x3098, 2, 0x30FB, 1, 0x30FD, 2, 0x3038)  # 2.1 compatibility
ctx = egl.eglCreateContext(d, cfg, None, cattrs)
assert ctx and egl.eglMakeCurrent(d, None, None, ctx), "no GL 2.1 context"
gl.glGetString.restype = ctypes.c_char_p
print("renderer:", gl.glGetString(0x1F01).decode(), "|", gl.glGetString(0x1F02).decode())
gl.glClearColor.argtypes = [ctypes.c_float] * 4
gl.glVertex3f.argtypes = [ctypes.c_float] * 3
gl.glOrtho.argtypes = [ctypes.c_double] * 6
gl.glTexSubImage2D.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, P]
gl.glTexImage2D.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, P]
gpa = egl.eglGetProcAddress
mtc = ctypes.CFUNCTYPE(None, ctypes.c_uint, ctypes.c_float, ctypes.c_float)(gpa(b"glMultiTexCoord2f"))
active = ctypes.CFUNCTYPE(None, ctypes.c_uint)(gpa(b"glActiveTexture"))
fence_sync = ctypes.CFUNCTYPE(P, ctypes.c_uint, ctypes.c_uint)(gpa(b"glFenceSync"))
client_wait = ctypes.CFUNCTYPE(ctypes.c_uint, P, ctypes.c_uint, ctypes.c_uint64)(gpa(b"glClientWaitSync"))
delete_sync = ctypes.CFUNCTYPE(None, P)(gpa(b"glDeleteSync"))

W, H = 1024, 768
fbo = ctypes.c_uint(); rb = (ctypes.c_uint * 2)()
gl.glGenFramebuffers(1, ctypes.byref(fbo)); gl.glBindFramebuffer(0x8D40, fbo)
gl.glGenRenderbuffers(2, rb)
gl.glBindRenderbuffer(0x8D41, rb[0]); gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)  # RGBA8
gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb[0])
gl.glBindRenderbuffer(0x8D41, rb[1]); gl.glRenderbufferStorage(0x8D41, 0x88F0, W, H)  # DEPTH24_STENCIL8
gl.glFramebufferRenderbuffer(0x8D40, 0x821A, 0x8D41, rb[1])  # DEPTH_STENCIL_ATTACHMENT
assert gl.glCheckFramebufferStatus(0x8D40) == 0x8CD5, "fbo"
gl.glViewport(0, 0, W, H)
gl.glMatrixMode(0x1701); gl.glLoadIdentity(); gl.glOrtho(0, W, H, 0, -1, 1)
gl.glMatrixMode(0x1700); gl.glLoadIdentity()
gl.glEnable(0x0B71)  # DEPTH_TEST
gl.glPixelStorei(0x0CF5, 1)

PAGES, PAGE, BLOCK, BASES, SURF = 32, 128, 16, 16, 512
def tex(w, h, data):
    t = ctypes.c_uint(); gl.glGenTextures(1, ctypes.byref(t)); gl.glBindTexture(0x0DE1, t)
    gl.glTexParameteri(0x0DE1, 0x2801, 0x2601); gl.glTexParameteri(0x0DE1, 0x2800, 0x2601)  # LINEAR
    gl.glTexParameteri(0x0DE1, 0x2802, 0x812F); gl.glTexParameteri(0x0DE1, 0x2803, 0x812F)
    gl.glTexImage2D(0x0DE1, 0, 0x8058, w, h, 0, 0x1908, 0x1401, data)
    return t.value
bases = [tex(64, 64, bytes(((x * 4 + b * 37) & 255, (y * 4) & 255, (b * 53) & 255, 255)[c] for y in range(64) for x in range(64) for c in range(4)))
         for b in range(BASES)]
pagedata = [bytearray(bytes((90, 90, 90, 255)) * PAGE * PAGE) for _ in range(PAGES)]
pages = [tex(PAGE, PAGE, bytes(p)) for p in pagedata]
# surface i: page i % PAGES, block (i // PAGES) of the 64 a page holds; a 32x24 quad on screen
surf = []
for i in range(SURF):
    p, b = i % PAGES, i // PAGES
    surf.append((p, (b % 8) * BLOCK, (b // 8) * BLOCK, (i % 32) * 32, (i // 32) * 24 * 3 % (H - 24), bases[i % BASES]))
lit_set = set(range(0, SURF, max(1, SURF // lit))) if lit else set()
lit_set = set(sorted(lit_set)[:lit])

def rebuild(i, frame):
    p, bx, by = surf[i][0], surf[i][1], surf[i][2]
    row = bytearray()
    v = (frame * 7 + i * 13) & 127
    for y in range(BLOCK):
        for x in range(BLOCK):
            row += bytes((120 + v, 110 + ((x * 8) & 63), 100 + ((y * 8) & 63), 255))
    pd = pagedata[p]
    for y in range(BLOCK):
        o = ((by + y) * PAGE + bx) * 4
        pd[o:o + BLOCK * 4] = row[y * BLOCK * 4:(y + 1) * BLOCK * 4]

uploads = 0
def upload(i):
    global uploads
    p, by = surf[i][0], surf[i][2]
    active(0x84C1); gl.glBindTexture(0x0DE1, pages[p])
    rows = (ctypes.c_char * (PAGE * BLOCK * 4)).from_buffer(pagedata[p], by * PAGE * 4)
    gl.glTexSubImage2D(0x0DE1, 0, 0, by, PAGE, BLOCK, 0x1908, 0x1401, rows)  # the block's rows, full width (GLQuake)
    uploads += 1

def draw(i):
    p, bx, by, sx, sy, base = surf[i]
    active(0x84C0); gl.glBindTexture(0x0DE1, base)
    active(0x84C1); gl.glBindTexture(0x0DE1, pages[p])
    u0, v0, u1, v1 = (bx + 0.5) / PAGE, (by + 0.5) / PAGE, (bx + BLOCK - 0.5) / PAGE, (by + BLOCK - 0.5) / PAGE
    gl.glBegin(0x0007)
    for (a, b, lu, lv) in ((0, 0, u0, v0), (1, 0, u1, v0), (1, 1, u1, v1), (0, 1, u0, v1)):
        mtc(0x84C0, a, b); mtc(0x84C1, lu, lv); gl.glVertex3f(sx + a * 32, sy + b * 24, 0.0)
    gl.glEnd()

for u in (0, 1):
    active(0x84C0 + u); gl.glEnable(0x0DE1); gl.glTexEnvi(0x2300, 0x2200, 0x2100)  # MODULATE
syncs = []
t0 = time.monotonic()
for frame in range(frames):
    gl.glClearColor(0.1, 0.1, 0.12, 1); gl.glClear(0x4000 | 0x0100 | 0x0400)
    for i in lit_set:
        rebuild(i, frame)
    if mode == "batched":
        for i in sorted(lit_set):
            upload(i)
    for i in range(SURF):
        if mode == "mtex" and i in lit_set:
            upload(i)
        draw(i)
    gl.glFlush()
    syncs.append(fence_sync(0x9117, 0))  # SYNC_GPU_COMMANDS_COMPLETE
    if len(syncs) > 2:
        s = syncs.pop(0)
        client_wait(s, 1, 10 * 10**9)
        delete_sync(s)
gl.glFinish()
dt = time.monotonic() - t0
out = ctypes.create_string_buffer(W * H * 4)
gl.glReadPixels(0, 0, W, H, 0x1908, 0x1401, out)
err = gl.glGetError()
if len(sys.argv) > 4:
    open(sys.argv[4], "wb").write(out.raw)
print("RESULT %s mode=%s lit=%d frames=%d uploads=%d ms_per_frame=%.1f crc=%08x glerror=0x%x" %
      ("ok" if err == 0 else "WRONG", mode, lit, frames, uploads, dt * 1000 / frames, zlib.crc32(out.raw), err))
