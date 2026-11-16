#!/usr/bin/env python3
# Fixed-function GL states GoldSrc (Half-Life) uses for its HUD, console, menus and world, one state set per
# run, so Zink's fixed-function shaders for each can be dumped (ZINK_DEBUG=spirv) and paired with what the
# bridge's split_varying pass says about them (MK-47). Each case draws textured GL_QUADS (colour per
# vertex) into an FBO, reads it back and writes it (out.bin) so two drivers can be compared.
#   glstates.py list                     the case names
#   glstates.py <case> <out.bin> [ref]   one case; with ref: compares (exact up to 2 per channel)
# Compatibility GL 2.1 over EGL surfaceless. Last line: "RESULT ok ..." or "RESULT WRONG ...".
import ctypes, ctypes.util, sys

CASES = ["modulate", "replace", "decal", "blend", "add", "alphatest", "alphatest_mod", "fog_linear", "fog_exp",
         "fog_alphatest", "flat", "tc4", "texmat", "two_units", "two_units_combine", "combine_scale2", "rect",
         "texgen", "clipplane", "colorsum", "lines", "fog_coord", "notex_fog", "two_units_fog", "trifan_alpha"]
if len(sys.argv) > 1 and sys.argv[1] == "list":
    print(" ".join(CASES)); sys.exit(0)
case, outp = sys.argv[1], sys.argv[2]
assert case in CASES, case

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
F = ctypes.c_float
for f in ("glColor4f", "glClearColor", "glTexCoord4f", "glMultiTexCoord4f"):
    getattr(gl, f).argtypes = ([ctypes.c_uint] if f == "glMultiTexCoord4f" else []) + [F] * 4
gl.glVertex2f.argtypes = [F] * 2
gl.glTexCoord2f.argtypes = [F] * 2
gl.glMultiTexCoord2f.argtypes = [ctypes.c_uint, F, F]
gl.glFogf.argtypes = [ctypes.c_uint, F]
gl.glFogCoordf.argtypes = [F]
gl.glAlphaFunc.argtypes = [ctypes.c_uint, F]
gl.glScalef.argtypes = [F] * 3
gl.glOrtho.argtypes = [ctypes.c_double] * 6
gl.glTexImage2D.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, P]
gl.glTexEnvf.argtypes = [ctypes.c_uint, ctypes.c_uint, F]

W = H = 256
fbo = ctypes.c_uint(); rb = ctypes.c_uint(); db = ctypes.c_uint()
gl.glGenFramebuffers(1, ctypes.byref(fbo)); gl.glBindFramebuffer(0x8D40, fbo)
gl.glGenRenderbuffers(1, ctypes.byref(rb)); gl.glBindRenderbuffer(0x8D41, rb)
gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)  # RGBA8
gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
gl.glGenRenderbuffers(1, ctypes.byref(db)); gl.glBindRenderbuffer(0x8D41, db)
gl.glRenderbufferStorage(0x8D41, 0x81A6, W, H)  # DEPTH_COMPONENT24
gl.glFramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, db)
assert gl.glCheckFramebufferStatus(0x8D40) == 0x8CD5, "fbo"
gl.glViewport(0, 0, W, H)
gl.glMatrixMode(0x1701); gl.glLoadIdentity(); gl.glOrtho(0, W, H, 0, -1, 1)
gl.glMatrixMode(0x1700); gl.glLoadIdentity()

TS = 64
def make_tex(target=0x0DE1, seed=0):
    t = ctypes.c_uint(); gl.glGenTextures(1, ctypes.byref(t)); gl.glBindTexture(target, t)
    gl.glTexParameteri(target, 0x2801, 0x2600); gl.glTexParameteri(target, 0x2800, 0x2600)  # NEAREST
    gl.glTexParameteri(target, 0x2802, 0x812F); gl.glTexParameteri(target, 0x2803, 0x812F)  # CLAMP_TO_EDGE
    px = bytearray()
    for y in range(TS):
        for x in range(TS):
            px += bytes(((x * 4 + seed) & 255, (y * 4) & 255, ((x ^ y) * 4) & 255, 255 if (x // 8 + y // 8 + seed) % 2 else 96))
    buf = ctypes.create_string_buffer(bytes(px))
    gl.glTexImage2D(target, 0, 0x8058, TS, TS, 0, 0x1908, 0x1401, buf)
    return t

def env(mode):
    gl.glTexEnvi(0x2300, 0x2200, mode)  # TEXTURE_ENV_MODE

def quads(tc=None, units=1, fogc=False, rect=False):
    s = TS if rect else 1.0
    gl.glBegin(0x0007)  # QUADS
    for i in range(12):
        x, y = 8 + (i % 4) * 60, 8 + (i // 4) * 80
        gl.glColor4f(1.0, 0.5 + 0.04 * i, 0.3 + 0.05 * (i % 4), 0.8)
        for (u, v, px, py) in ((0, 0, x, y), (s, 0, x + 50, y), (s, s, x + 50, y + 70), (0, s, x, y + 70)):
            if fogc: gl.glFogCoordf(0.1 * i + u)
            if tc == "4": gl.glTexCoord4f(u * 2, v * 2, 0.0, 2.0)
            elif units == 2:
                gl.glMultiTexCoord2f(0x84C0, u, v); gl.glMultiTexCoord2f(0x84C1, v, u)
            else: gl.glTexCoord2f(u, v)
            gl.glVertex2f(px, py)
    gl.glEnd()

gl.glClearColor(0.1, 0.2, 0.15, 1); gl.glClear(0x4100)
gl.glEnable(0x0BE2); gl.glBlendFunc(0x0302, 0x0303)  # BLEND SRC_ALPHA, ONE_MINUS_SRC_ALPHA
tex = make_tex()
gl.glEnable(0x0DE1)
env(0x2100)  # MODULATE
def fog(mode):
    gl.glEnable(0x0B60); gl.glFogi(0x0B65, mode)  # FOG, FOG_MODE
    gl.glFogf(0x0B63, 0.0); gl.glFogf(0x0B64, 1.5); gl.glFogf(0x0B62, 0.7)  # START, END, DENSITY
    col = (F * 4)(0.5, 0.4, 0.3, 1.0); gl.glFogfv(0x0B66, col)
args = {}
if case == "replace": env(0x1E01)
elif case == "decal": env(0x2101)
elif case == "blend": env(0x0BE2)
elif case == "add": env(0x0104)
elif case in ("alphatest", "alphatest_mod", "fog_alphatest", "trifan_alpha"):
    gl.glEnable(0x0BC0); gl.glAlphaFunc(0x0204, 0.666)  # ALPHA_TEST GREATER (GoldSrc's console font)
    if case == "alphatest": env(0x1E01)
    if case == "fog_alphatest": fog(0x2601)
elif case == "fog_linear": fog(0x2601)
elif case == "fog_exp": fog(0x0800)
elif case == "flat": gl.glShadeModel(0x1D00)
elif case == "tc4": args["tc"] = "4"
elif case == "texmat":
    gl.glMatrixMode(0x1702); gl.glLoadIdentity(); gl.glScalef(0.5, 0.5, 1); gl.glMatrixMode(0x1700)
elif case in ("two_units", "two_units_combine", "two_units_fog"):
    gl.glActiveTexture(0x84C1); make_tex(seed=7); gl.glEnable(0x0DE1)
    if case == "two_units_combine":
        env(0x8570); gl.glTexEnvi(0x2300, 0x8571, 0x2100); gl.glTexEnvf(0x2300, 0x8573, 2.0)  # COMBINE, COMBINE_RGB MODULATE, RGB_SCALE 2
    else: env(0x2100)
    gl.glActiveTexture(0x84C0); args["units"] = 2
    if case == "two_units_fog": fog(0x2601)
elif case == "combine_scale2":
    env(0x8570); gl.glTexEnvi(0x2300, 0x8571, 0x2100); gl.glTexEnvf(0x2300, 0x8573, 2.0)
elif case == "rect":
    gl.glDisable(0x0DE1); make_tex(0x84F5); gl.glEnable(0x84F5); args["rect"] = True
elif case == "texgen":
    pl = (F * 4)(1.0 / 256, 0, 0, 0); gl.glTexGenfv(0x2000, 0x2501, pl); gl.glTexGeni(0x2000, 0x2500, 0x2401)
    gl.glEnable(0x0C60)  # S: OBJECT_LINEAR
elif case == "clipplane":
    pl = (ctypes.c_double * 4)(1, 0, 0, -20); gl.glClipPlane(0x3000, pl); gl.glEnable(0x3000)
elif case == "colorsum":
    gl.glEnable(0x8458); gl.glSecondaryColor3f.argtypes = [F] * 3; gl.glSecondaryColor3f(0.2, 0.1, 0.0)
elif case == "fog_coord":
    fog(0x2601); gl.glFogi(0x8450, 0x8451); args["fogc"] = True  # FOG_COORD_SRC = FOG_COORD
elif case == "notex_fog":
    gl.glDisable(0x0DE1); fog(0x2601)
if case == "lines":
    gl.glBegin(0x0001)
    for i in range(40):
        gl.glColor4f(1, i / 40, 0.5, 1); gl.glTexCoord2f(i / 40, 0.5); gl.glVertex2f(4 + i * 6, 10)
        gl.glTexCoord2f(i / 40, 0.9); gl.glVertex2f(4 + i * 6, 240)
    gl.glEnd()
elif case == "trifan_alpha":
    gl.glBegin(0x0006)
    for (u, v, px, py) in ((0.5, 0.5, 128, 128), (0, 0, 10, 10), (1, 0, 246, 10), (1, 1, 246, 246), (0, 1, 10, 246), (0, 0, 10, 10)):
        gl.glColor4f(1, u, v, 1); gl.glTexCoord2f(u, v); gl.glVertex2f(px, py)
    gl.glEnd()
else:
    quads(**args)
gl.glFinish()
out = ctypes.create_string_buffer(W * H * 4)
gl.glReadPixels(0, 0, W, H, 0x1908, 0x1401, out)
err = gl.glGetError()
open(outp, "wb").write(out.raw)
if len(sys.argv) > 3:
    ref = open(sys.argv[3], "rb").read()
    a = out.raw
    diff = sum(1 for p in range(0, W * H * 4, 4) if max(abs(a[p + c] - ref[p + c]) for c in range(4)) > 2)
    print(f"RESULT {'ok' if not diff else 'WRONG'} {case}: {diff} of {W * H} pixels differ (glError {err:#x})")
    sys.exit(1 if diff else 0)
print(f"RESULT ok {case} written (glError {err:#x})")
