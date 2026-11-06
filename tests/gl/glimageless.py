#!/usr/bin/env python3
# Zink over the bridge on a driver without VK_KHR_imageless_framebuffer (Mali r18-r26, Vulkan 1.1:
# Mesa 26.0.8's Zink refuses such a device, "ZINK: VK_KHR_imageless_framebuffer required!", and
# Steam's UI died with "failed to load driver: zink" on a HUAWEI P40 Pro). The bridge makes
# imageless framebuffers itself (server_emu_imageless.c). Two framebuffer objects drawn in turn,
# several rounds: each cleared to its
# colour with a white triangle over its lower-left half, read back. (Zink 26.x renders with dynamic
# rendering only: the extension is its start-up check, no imageless framebuffer is made.)
# Prints the renderer (must be zink) and "RESULT ok" / "RESULT WRONG ...".
# Desktop GL 2.1 (compatibility, GLSL 1.20, framebuffer objects) over EGL surfaceless: what Zink offers on
# those drivers (no transform feedback there, so no OpenGL 3.0).
import ctypes, ctypes.util, sys

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
if not egl.eglInitialize(d, None, None):
    print("RESULT WRONG eglInitialize failed (no Zink screen)")
    sys.exit(1)
assert egl.eglBindAPI(0x30A2), "eglBindAPI(OpenGL)"
attrs = (ctypes.c_int * 5)(0x3033, 0, 0x3040, 0x8, 0x3038)  # SURFACE_TYPE none, RENDERABLE_TYPE OPENGL_BIT
cfg = P(); n = ctypes.c_int()
assert egl.eglChooseConfig(d, attrs, ctypes.byref(cfg), 1, ctypes.byref(n)) and n.value, "config"
ctx = egl.eglCreateContext(d, cfg, None, None)  # the default: compatibility
if not ctx or not egl.eglMakeCurrent(d, None, None, ctx):
    print("RESULT WRONG no GL context")
    sys.exit(1)
gl.glGetString.restype = ctypes.c_char_p
renderer = gl.glGetString(0x1F01).decode()
print("renderer:", renderer, "|", gl.glGetString(0x1F02).decode())

W = H = 16
def target():
    fb = ctypes.c_uint(); rb = ctypes.c_uint()
    gl.glGenFramebuffers(1, ctypes.byref(fb)); gl.glBindFramebuffer(0x8D40, fb)
    gl.glGenRenderbuffers(1, ctypes.byref(rb)); gl.glBindRenderbuffer(0x8D41, rb)
    gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)  # RGBA8
    gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
    return fb
fbs = [target(), target()]
gl.glViewport(0, 0, W, H)

def shader(kind, src):
    s = gl.glCreateShader(kind)
    b = ctypes.c_char_p(src.encode())
    gl.glShaderSource(s, 1, ctypes.byref(b), None)
    gl.glCompileShader(s)
    return s
vs = shader(0x8B31, "#version 120\nattribute vec2 p; void main(){ gl_Position = vec4(p,0,1); }\n")
fs = shader(0x8B30, "#version 120\nvoid main(){ gl_FragColor = vec4(1.0); }\n")
prog = gl.glCreateProgram(); gl.glAttachShader(prog, vs); gl.glAttachShader(prog, fs)
gl.glBindAttribLocation(prog, 0, b"p"); gl.glLinkProgram(prog); gl.glUseProgram(prog)
tri = (ctypes.c_float * 6)(-1, -1, 1, -1, -1, 1)  # the lower-left half (GL's origin)
vbo = ctypes.c_uint()
gl.glGenBuffers(1, ctypes.byref(vbo)); gl.glBindBuffer(0x8892, vbo)
gl.glBufferData(0x8892, ctypes.sizeof(tri), tri, 0x88E4)
gl.glVertexAttribPointer(0, 2, 0x1406, 0, 8, None)
gl.glEnableVertexAttribArray(0)

colours = [(1.0, 0.0, 0.0), (0.0, 0.0, 1.0)]
want_bg = [[255, 0, 0, 255], [0, 0, 255, 255]]
bad = []
for rnd in range(4):
    for i, fb in enumerate(fbs):
        gl.glBindFramebuffer(0x8D40, fb)
        r, g, b = colours[(i + rnd) % 2]
        gl.glClearColor(ctypes.c_float(r), ctypes.c_float(g), ctypes.c_float(b), ctypes.c_float(1.0)); gl.glClear(0x4000)
        gl.glDrawArrays(4, 0, 3)
    for i, fb in enumerate(fbs):
        gl.glBindFramebuffer(0x8D40, fb)
        tri_px = (ctypes.c_ubyte * 4)(); bg_px = (ctypes.c_ubyte * 4)()
        gl.glReadPixels(2, 2, 1, 1, 0x1908, 0x1401, tri_px)
        gl.glReadPixels(13, 13, 1, 1, 0x1908, 0x1401, bg_px)
        if list(tri_px) != [255, 255, 255, 255] or list(bg_px) != want_bg[(i + rnd) % 2]:
            bad.append("round %d fb %d: triangle %s background %s (want %s)" % (rnd, i, list(tri_px), list(bg_px), want_bg[(i + rnd) % 2]))
err = gl.glGetError()
if "zink" not in renderer.lower():
    print("RESULT WRONG renderer is not zink:", renderer)
    sys.exit(1)
if bad or err:
    print("RESULT WRONG", "; ".join(bad[:3]), "glError 0x%x" % err)
    sys.exit(1)
print("RESULT ok 4 rounds of 2 framebuffer objects, glError 0x%x" % err)
