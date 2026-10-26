#!/usr/bin/env python3
# Two vertex layouts, one program: the left half drawn from a VAO with stride 12 (position, then an
# RGBA8 colour at 8), the right half from one with stride 20 (position, a zero uv, the colour at 16).
# Only the vertex elements differ between the two draws, the case in which Zink 26.0 to 26.2.4
# without VK_EXT_vertex_input_dynamic_state kept the first draw's pipeline on a Mali tablet (Mesa
# 167df22a; Balatro's missing text; the desktop's Mesa did not show it). Here it checks Zink's
# vkCmdSetVertexInputEXT path through the server's emulation (server_vidyn.c): the left pixel reads
# (255, 0, 0) and the right one (0, 0, 255); a vertex input kept from the first draw would read the
# right one's colour from the uv: black.
# Desktop GL 3.3 core over EGL surfaceless.
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

W = H = 16
fb = ctypes.c_uint(); rb = ctypes.c_uint()
gl.glGenFramebuffers(1, ctypes.byref(fb)); gl.glBindFramebuffer(0x8D40, fb)
gl.glGenRenderbuffers(1, ctypes.byref(rb)); gl.glBindRenderbuffer(0x8D41, rb)
gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)
gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
gl.glViewport(0, 0, W, H)

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

class A(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("c", ctypes.c_uint32)]
class B(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("u", ctypes.c_float), ("v", ctypes.c_float), ("c", ctypes.c_uint32)]
red, blue = 0xFF0000FF, 0xFFFF0000  # RGBA8 in memory order
left = (A * 3)(A(-1, -1, red), A(0, -1, red), A(-1, 3, red))
right = (B * 3)(B(0, -1, 0, 0, blue), B(1, -1, 0, 0, blue), B(1, 3, 0, 0, blue))

def vao_for(data, stride, coff):
    vao = ctypes.c_uint(); vb = ctypes.c_uint()
    gl.glGenVertexArrays(1, ctypes.byref(vao)); gl.glBindVertexArray(vao)
    gl.glGenBuffers(1, ctypes.byref(vb)); gl.glBindBuffer(0x8892, vb)
    gl.glBufferData(0x8892, ctypes.sizeof(data), data, 0x88E4)
    gl.glVertexAttribPointer(0, 2, 0x1406, 0, stride, None)
    gl.glVertexAttribPointer(1, 4, 0x1401, 1, stride, ctypes.c_void_p(coff))  # GL_UNSIGNED_BYTE, normalized
    gl.glEnableVertexAttribArray(0); gl.glEnableVertexAttribArray(1)
    return vao
va = vao_for(left, 12, 8)
vb_ = vao_for(right, 20, 16)
gl.glClearColor(0, 0, 0, 1); gl.glClear(0x4000)
gl.glBindVertexArray(va); gl.glDrawArrays(4, 0, 3)
gl.glBindVertexArray(vb_); gl.glDrawArrays(4, 0, 3)
pl = (ctypes.c_ubyte * 4)(); pr = (ctypes.c_ubyte * 4)()
gl.glReadPixels(2, 8, 1, 1, 0x1908, 0x1401, pl)
gl.glReadPixels(13, 8, 1, 1, 0x1908, 0x1401, pr)
err = gl.glGetError()
ok = list(pl)[:3] == [255, 0, 0] and list(pr)[:3] == [0, 0, 255]
print(("RESULT ok" if ok else "RESULT WRONG"), "left", list(pl), "right", list(pr), "(want [255, 0, 0, 255] and [0, 0, 255, 255]) glError 0x%x" % err)
