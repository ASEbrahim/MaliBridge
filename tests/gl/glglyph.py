#!/usr/bin/env python3
# Text the way GoldSrc (Half-Life) draws it, through Zink: font pages of 256x256 RGBA, glyphs written
# into them one by one with glTexSubImage2D (small rects, odd widths) between frames that draw the
# glyphs already there as GL_QUADS in immediate mode (fixed function, GL_MODULATE, alpha blending).
# Several frames run without a read-back (as a game does: uploads into a page the GPU still reads),
# then the frame is read back. Variants: RGBA, GL_ALPHA, GL_LUMINANCE_ALPHA pages; a pixel unpack
# buffer; GL_UNPACK_ROW_LENGTH / SKIP into one big glyph sheet.
#   glglyph.py <out.bin>              writes what it read (reference run)
#   glglyph.py <out.bin> <ref.bin>    and compares with a reference run, case by case (exact up to 2)
# Compatibility GL over EGL surfaceless, into an FBO. Last line: "RESULT ok ..." or "RESULT WRONG ...".
import ctypes, ctypes.util, struct, sys

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
for f in ("glColor4f", "glClearColor"):
    getattr(gl, f).argtypes = [ctypes.c_float] * 4
gl.glVertex2f.argtypes = [ctypes.c_float] * 2
gl.glTexCoord2f.argtypes = [ctypes.c_float] * 2
gl.glOrtho.argtypes = [ctypes.c_double] * 6
gl.glBufferData.argtypes = [ctypes.c_uint, ctypes.c_ssize_t, P, ctypes.c_uint]
gl.glTexSubImage2D.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, P]
gl.glTexImage2D.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, P]

W = H = 512
fbo = ctypes.c_uint(); rb = ctypes.c_uint()
gl.glGenFramebuffers(1, ctypes.byref(fbo)); gl.glBindFramebuffer(0x8D40, fbo)
gl.glGenRenderbuffers(1, ctypes.byref(rb)); gl.glBindRenderbuffer(0x8D41, rb)
gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)  # RGBA8
gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
assert gl.glCheckFramebufferStatus(0x8D40) == 0x8CD5, "fbo"
gl.glViewport(0, 0, W, H)
gl.glMatrixMode(0x1701); gl.glLoadIdentity(); gl.glOrtho(0, W, H, 0, -1, 1)
gl.glMatrixMode(0x1700); gl.glLoadIdentity()
gl.glEnable(0x0DE1)  # TEXTURE_2D
gl.glEnable(0x0BE2); gl.glBlendFunc(0x0302, 0x0303)  # SRC_ALPHA, ONE_MINUS_SRC_ALPHA
gl.glTexEnvi(0x2300, 0x2200, 0x2100)  # MODULATE

PAGE = 256

def glyph(ch, w, h):
    """a glyph's coverage: a ring and a bar that depend on the character, anti-aliased edges"""
    out = []
    for y in range(h):
        for x in range(w):
            cx, cy = x - w / 2 + 0.5, y - h / 2 + 0.5
            r = (cx * cx + cy * cy) ** 0.5
            v = 255 if abs(r - min(w, h) / 3) < 1.2 else 0
            if (x + ch) % 5 == 0 and y > h // 3: v = 255
            if (x * 7 + y * 3 + ch) % 11 == 0: v = max(v, 120)
            out.append(v)
    return out

def page_bytes(kind, cov):
    if kind == "rgba":
        return b"".join(struct.pack("4B", 255, 255, 255, a) for a in cov), 0x1908, 0x8058, 4  # RGBA, RGBA8
    if kind == "alpha":
        return bytes(cov), 0x1906, 0x803C, 1  # ALPHA, ALPHA8
    return b"".join(struct.pack("2B", 255, a) for a in cov), 0x190A, 0x8045, 2  # LUMINANCE_ALPHA, LUMINANCE8_ALPHA8

def run_case(kind, mode):
    tex = ctypes.c_uint(); gl.glGenTextures(1, ctypes.byref(tex)); gl.glBindTexture(0x0DE1, tex)
    gl.glTexParameteri(0x0DE1, 0x2801, 0x2600); gl.glTexParameteri(0x0DE1, 0x2800, 0x2600)  # NEAREST
    gl.glTexParameteri(0x0DE1, 0x2802, 0x812F); gl.glTexParameteri(0x0DE1, 0x2803, 0x812F)  # CLAMP_TO_EDGE
    _, fmt, ifmt, bpp = page_bytes(kind, [0])
    gl.glPixelStorei(0x0CF5, 1)  # UNPACK_ALIGNMENT 1 (GoldSrc's font code sets it)
    zero = ctypes.create_string_buffer(PAGE * PAGE * bpp)
    gl.glTexImage2D(0x0DE1, 0, ifmt, PAGE, PAGE, 0, fmt, 0x1401, zero)
    pbo = ctypes.c_uint()
    if mode == "pbo":
        gl.glGenBuffers(1, ctypes.byref(pbo))
    glyphs = []
    x = y = 1; rowh = 0
    sheet = None
    if mode == "rowlen":  # every glyph lives in one big sheet; uploads pick it out with ROW_LENGTH / SKIP_*
        sw = 200
        sheet_cov = [((i * 13) ^ (i // sw * 7)) & 255 for i in range(sw * 64)]
        sheet = page_bytes(kind, sheet_cov)[0]
    ch = 0
    for frame in range(24):
        for _ in range(6):  # new glyphs this frame
            gw, gh = 5 + (ch * 3) % 9, 12 + ch % 6
            if x + gw + 1 > PAGE: x = 1; y += rowh + 1; rowh = 0
            if mode == "rowlen":
                sx, sy = (ch * 17) % (200 - gw), (ch * 5) % (64 - gh)
                gl.glPixelStorei(0x0CF2, 200); gl.glPixelStorei(0x0CF4, sx); gl.glPixelStorei(0x0CF3, sy)
                gl.glTexSubImage2D(0x0DE1, 0, x, y, gw, gh, fmt, 0x1401, sheet)
                gl.glPixelStorei(0x0CF2, 0); gl.glPixelStorei(0x0CF4, 0); gl.glPixelStorei(0x0CF3, 0)
            else:
                data = page_bytes(kind, glyph(ch, gw, gh))[0]
                if mode == "pbo":
                    gl.glBindBuffer(0x88EC, pbo)
                    gl.glBufferData(0x88EC, len(data), data, 0x88E0)  # STREAM_DRAW
                    gl.glTexSubImage2D(0x0DE1, 0, x, y, gw, gh, fmt, 0x1401, None)
                    gl.glBindBuffer(0x88EC, 0)
                else:
                    gl.glTexSubImage2D(0x0DE1, 0, x, y, gw, gh, fmt, 0x1401, data)
            glyphs.append((x, y, gw, gh))
            x += gw + 1; rowh = max(rowh, gh); ch += 1
        gl.glClearColor(0.1, 0.2, 0.15, 1); gl.glClear(0x4000)
        gl.glBegin(0x0007)  # QUADS
        for i, (gx, gy, gw, gh) in enumerate(glyphs):
            px, py = 4 + (i % 30) * 16, 4 + (i // 30) * 22
            gl.glColor4f(1.0, 0.85 if i % 2 else 1.0, 0.3 + 0.1 * (i % 4), 1.0)
            u0, v0, u1, v1 = gx / PAGE, gy / PAGE, (gx + gw) / PAGE, (gy + gh) / PAGE
            gl.glTexCoord2f(u0, v0); gl.glVertex2f(px, py)
            gl.glTexCoord2f(u1, v0); gl.glVertex2f(px + gw, py)
            gl.glTexCoord2f(u1, v1); gl.glVertex2f(px + gw, py + gh)
            gl.glTexCoord2f(u0, v1); gl.glVertex2f(px, py + gh)
        gl.glEnd()
        gl.glFlush()
    out = ctypes.create_string_buffer(W * H * 4)
    gl.glReadPixels(0, 0, W, H, 0x1908, 0x1401, out)
    gl.glDeleteTextures(1, ctypes.byref(tex))
    if mode == "pbo":
        gl.glDeleteBuffers(1, ctypes.byref(pbo))
    return out.raw

cases = [(k, m) for k in ("rgba", "alpha", "la") for m in ("client", "pbo", "rowlen")]
got = b""
res = []
for k, m in cases:
    got += run_case(k, m)
err = gl.glGetError()
open(sys.argv[1], "wb").write(got)
if len(sys.argv) > 2:
    ref = open(sys.argv[2], "rb").read()
    bad = []
    sz = W * H * 4
    for i, (k, m) in enumerate(cases):
        a, b = got[i * sz:(i + 1) * sz], ref[i * sz:(i + 1) * sz]
        diff = sum(1 for p in range(0, sz, 4) if max(abs(a[p + c] - b[p + c]) for c in range(4)) > 2)
        print(f"  {k}/{m}: {diff} of {W * H} pixels differ")
        if diff: bad.append(f"{k}/{m}:{diff}")
    if bad:
        print("RESULT WRONG", " ".join(bad)); sys.exit(1)
    print(f"RESULT ok {len(cases)} cases match the reference (glError {err:#x})")
else:
    print(f"RESULT ok {len(cases)} cases written (glError {err:#x})")
