#!/usr/bin/env python3
# GL_ARB_vertex_type_10f_11f_11f_rev: a full-screen triangle whose vertex colour is packed as
# GL_UNSIGNED_INT_10F_11F_11F_REV (Vulkan's B10G11R11_UFLOAT_PACK32). Red 0.75, green 0.25, blue 0.5:
# the centre pixel should read about (191, 64, 128). Also prints the GL version, which needs the
# format for 4.4 and up. Desktop GL 3.3 core over EGL surfaceless.
import ctypes, ctypes.util

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
attrs = (ctypes.c_int * 5)(0x3033, 0, 0x3040, 0x8, 0x3038)  # SURFACE_TYPE none, RENDERABLE_TYPE OPENGL_BIT
cfg = P(); n = ctypes.c_int()
assert egl.eglChooseConfig(d, attrs, ctypes.byref(cfg), 1, ctypes.byref(n)) and n.value, "config"
cattrs = (ctypes.c_int * 7)(0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038)  # 3.3 core
ctx = egl.eglCreateContext(d, cfg, None, cattrs)
assert ctx and egl.eglMakeCurrent(d, None, None, ctx), "no GL 3.3 core context"
gl.glGetString.restype = ctypes.c_char_p
print("renderer:", gl.glGetString(0x1F01).decode(), "|", gl.glGetString(0x1F02).decode())
maj = ctypes.c_int(); mnr = ctypes.c_int()
gl.glGetIntegerv(0x821B, ctypes.byref(maj)); gl.glGetIntegerv(0x821C, ctypes.byref(mnr))  # the newest core version
print("GL_VERSION_MAX %d.%d" % (maj.value, mnr.value))

W = H = 16
fb = ctypes.c_uint(); rb = ctypes.c_uint(); vao = ctypes.c_uint(); vb = ctypes.c_uint()
gl.glGenFramebuffers(1, ctypes.byref(fb)); gl.glBindFramebuffer(0x8D40, fb)
gl.glGenRenderbuffers(1, ctypes.byref(rb)); gl.glBindRenderbuffer(0x8D41, rb)
gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)
gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
gl.glViewport(0, 0, W, H)
gl.glGenVertexArrays(1, ctypes.byref(vao)); gl.glBindVertexArray(vao)

def shader(kind, src):
    s = gl.glCreateShader(kind)
    b = ctypes.c_char_p(src.encode())
    gl.glShaderSource(s, 1, ctypes.byref(b), None)
    gl.glCompileShader(s)
    return s
vs = shader(0x8B31, "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec4 c; out vec4 col;"
                    " void main(){ col = c; gl_Position = vec4(p,0,1); }\n")
fs = shader(0x8B30, "#version 330 core\nin vec4 col; out vec4 o; void main(){ o = col; }\n")
prog = gl.glCreateProgram(); gl.glAttachShader(prog, vs); gl.glAttachShader(prog, fs); gl.glLinkProgram(prog); gl.glUseProgram(prog)

def uf(x, mbits):  # a positive normal float as the packed 5-bit-exponent unsigned float
    import math
    e = math.floor(math.log2(x)); m = round((x / 2 ** e - 1) * (1 << mbits))
    return ((e + 15) << mbits) | m
packed = uf(0.75, 6) | (uf(0.25, 6) << 11) | (uf(0.5, 5) << 22)
class V(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("c", ctypes.c_uint32)]
verts = (V * 3)(V(-1, -1, packed), V(3, -1, packed), V(-1, 3, packed))
gl.glGenBuffers(1, ctypes.byref(vb)); gl.glBindBuffer(0x8892, vb)
gl.glBufferData(0x8892, ctypes.sizeof(verts), verts, 0x88E4)
gl.glVertexAttribPointer(0, 2, 0x1406, 0, 12, None)
gl.glVertexAttribPointer(1, 3, 0x8C3B, 0, 12, ctypes.c_void_p(8))  # GL_UNSIGNED_INT_10F_11F_11F_REV
gl.glEnableVertexAttribArray(0); gl.glEnableVertexAttribArray(1)
gl.glClearColor(0, 0, 0, 1); gl.glClear(0x4000)
gl.glDrawArrays(4, 0, 3)
px = (ctypes.c_ubyte * 4)()
gl.glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, px)
err = gl.glGetError()
ok = abs(px[0] - 191) <= 2 and abs(px[1] - 64) <= 2 and abs(px[2] - 128) <= 2
print(("RESULT ok" if ok else "RESULT WRONG"), list(px), "(want ~[191, 64, 128, 255]) glError 0x%x" % err)
