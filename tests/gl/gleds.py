#!/usr/bin/env python3
# Extended dynamic state 1 and 2 through Zink (server_vidyn.c, VKB_VIDYN_EDS): one program, and
# between draws only the GL state that Zink turns into those dynamic states changes - depth test /
# write / compare op, cull mode, front face, primitive topology (points, lines, triangles),
# stencil test / op, scissor, rasterizer discard, primitive restart. On a Vulkan 1.1 Mali without
# the extensions Zink bakes all of it into its pipelines and keeps the previous one when only the
# vertex layout changes (Mesa 167df22a); the server offers the extensions and makes the states
# static in pipeline variants. Every case reads its own pixels and the expected colour is fixed
# here, so the test checks itself on the desktop driver too.
# Desktop GL 3.3 core over EGL surfaceless. Last line: "RESULT ok ..." or "RESULT WRONG ...".
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
attrs = (ctypes.c_int * 5)(0x3033, 0, 0x3040, 0x8, 0x3038)
cfg = P(); n = ctypes.c_int()
assert egl.eglChooseConfig(d, attrs, ctypes.byref(cfg), 1, ctypes.byref(n)) and n.value, "config"
cattrs = (ctypes.c_int * 7)(0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038)  # 3.3 core
ctx = egl.eglCreateContext(d, cfg, None, cattrs)
assert ctx and egl.eglMakeCurrent(d, None, None, ctx), "no GL 3.3 core context"
gl.glGetString.restype = ctypes.c_char_p
print("renderer:", gl.glGetString(0x1F01).decode(), "|", gl.glGetString(0x1F02).decode())
gl.glClearDepth.argtypes = [ctypes.c_double]
gl.glClearColor.argtypes = [ctypes.c_float] * 4
gl.glUniform4f.argtypes = [ctypes.c_int] + [ctypes.c_float] * 4
gl.glUniform1f.argtypes = [ctypes.c_int, ctypes.c_float]

W = H = 64
fb, rb, ds = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
gl.glGenFramebuffers(1, ctypes.byref(fb)); gl.glBindFramebuffer(0x8D40, fb)
gl.glGenRenderbuffers(1, ctypes.byref(rb)); gl.glBindRenderbuffer(0x8D41, rb)
gl.glRenderbufferStorage(0x8D41, 0x8058, W, H)  # RGBA8
gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
gl.glGenRenderbuffers(1, ctypes.byref(ds)); gl.glBindRenderbuffer(0x8D41, ds)
gl.glRenderbufferStorage(0x8D41, 0x88F0, W, H)  # DEPTH24_STENCIL8
gl.glFramebufferRenderbuffer(0x8D40, 0x821A, 0x8D41, ds)  # DEPTH_STENCIL_ATTACHMENT
assert gl.glCheckFramebufferStatus(0x8D40) == 0x8CD5, "framebuffer incomplete"
gl.glViewport(0, 0, W, H)

def shader(kind, src):
    s = gl.glCreateShader(kind)
    b = ctypes.c_char_p(src.encode())
    gl.glShaderSource(s, 1, ctypes.byref(b), None)
    gl.glCompileShader(s)
    return s
vs = shader(0x8B31, "#version 330 core\nlayout(location=0) in vec3 p; uniform float z;"
                    " void main(){ gl_Position = vec4(p.xy, z, 1); gl_PointSize = 4.0; }\n")
fs = shader(0x8B30, "#version 330 core\nuniform vec4 c; out vec4 o; void main(){ o = c; }\n")
prog = gl.glCreateProgram(); gl.glAttachShader(prog, vs); gl.glAttachShader(prog, fs); gl.glLinkProgram(prog); gl.glUseProgram(prog)
uc = gl.glGetUniformLocation(prog, b"c"); uz = gl.glGetUniformLocation(prog, b"z")
gl.glEnable(0x8642)  # GL_PROGRAM_POINT_SIZE

vao, vb, ib = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
gl.glGenVertexArrays(1, ctypes.byref(vao)); gl.glBindVertexArray(vao)
gl.glGenBuffers(1, ctypes.byref(vb)); gl.glBindBuffer(0x8892, vb)
gl.glGenBuffers(1, ctypes.byref(ib)); gl.glBindBuffer(0x8893, ib)
gl.glVertexAttribPointer(0, 3, 0x1406, 0, 12, None); gl.glEnableVertexAttribArray(0)

def px(x):  # pixel x/y -> NDC
    return x * 2.0 / W - 1.0
def quad(x0, y0, x1, y1, cw=False):  # two triangles, counter-clockwise (front) unless cw
    a, b, c, e = (px(x0), px(y0)), (px(x1), px(y0)), (px(x1), px(y1)), (px(x0), px(y1))
    tris = [a, b, c, a, c, e] if not cw else [a, c, b, a, e, c]
    return [v for t in tris for v in (t[0], t[1], 0.0)]
def draw(verts, mode=4, color=(1, 0, 0, 1), z=0.0):
    data = (ctypes.c_float * len(verts))(*verts)
    gl.glBufferData(0x8892, ctypes.sizeof(data), data, 0x88E8)  # STREAM_DRAW
    gl.glUniform4f(uc, *color); gl.glUniform1f(uz, z)
    gl.glDrawArrays(mode, 0, len(verts) // 3)
def read(x, y):
    p = (ctypes.c_ubyte * 4)()
    gl.glReadPixels(x, y, 1, 1, 0x1908, 0x1401, p)
    return tuple(p)[:3]

R, G, B, K, Y = (255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 0, 0), (255, 255, 0)
gl.glClearColor(0, 0, 0, 1); gl.glClearDepth(1.0); gl.glClearStencil(0)
gl.glClear(0x4000 | 0x100 | 0x400)
checks = []

# 1 depth: red at z 0.0 written; green behind it at 0.5 with LESS fails; with depth test off it shows
# in the next cell; GREATER lets yellow at 0.5 over the red; depth writes off: blue at -0.5 drawn but
# leaves the depth, so green at -0.25 with LESS still passes there
gl.glEnable(0x0B71); gl.glDepthFunc(0x0201); gl.glDepthMask(1)       # DEPTH_TEST, LESS
draw(quad(0, 0, 8, 8), color=(1, 0, 0, 1), z=0.0)
draw(quad(0, 0, 8, 8), color=(0, 1, 0, 1), z=0.5)
checks.append(("depth LESS keeps the nearer", read(4, 4), R))
draw(quad(8, 0, 16, 8), color=(1, 0, 0, 1), z=0.0)
gl.glDisable(0x0B71)
draw(quad(8, 0, 16, 8), color=(0, 1, 0, 1), z=0.5)
checks.append(("depth test off", read(12, 4), G))
gl.glEnable(0x0B71); gl.glDepthFunc(0x0204)                           # GREATER
draw(quad(16, 0, 24, 8), color=(1, 0, 0, 1), z=-1.0)  # depth 0 -> fails GREATER vs 1.0
draw(quad(16, 0, 24, 8), color=(1, 1, 0, 1), z=0.9)   # fails too (0.95 < 1)
checks.append(("depth GREATER against the clear", read(20, 4), K))
gl.glDepthFunc(0x0201); gl.glDepthMask(0)                             # LESS, no writes
draw(quad(24, 0, 32, 8), color=(0, 0, 1, 1), z=-0.5)
gl.glDepthMask(1)
draw(quad(24, 0, 32, 8), color=(0, 1, 0, 1), z=-0.25)
checks.append(("depth writes off", read(28, 4), G))
gl.glDisable(0x0B71)

# 2 cull mode and front face: a clockwise quad is a back face; culled with CULL_FACE BACK, drawn
# once the front face is clockwise, culled again with FRONT
gl.glEnable(0x0B44); gl.glCullFace(0x0405)                            # CULL_FACE, BACK
draw(quad(0, 8, 8, 16), color=(1, 0, 0, 1))
draw(quad(0, 8, 8, 16, cw=True), color=(0, 1, 0, 1))
checks.append(("cull back: clockwise culled", read(4, 12), R))
gl.glFrontFace(0x0900)                                                # CW
draw(quad(8, 8, 16, 16, cw=True), color=(0, 0, 1, 1))
checks.append(("front face clockwise: drawn", read(12, 12), B))
gl.glCullFace(0x0404)                                                 # FRONT
draw(quad(16, 8, 24, 16, cw=True), color=(0, 1, 0, 1))
checks.append(("cull front: clockwise front culled", read(20, 12), K))
gl.glFrontFace(0x0901); gl.glDisable(0x0B44)

# 3 topology: points, a line, then triangles again, one program
draw([px(4.5), px(20.5), 0.0], mode=0, color=(0, 1, 0, 1))          # POINTS
checks.append(("points", read(4, 20), G))
draw([px(8), px(20.5), 0.0, px(16), px(20.5), 0.0], mode=1, color=(0, 0, 1, 1))  # LINES
checks.append(("lines", read(12, 20), B))
draw(quad(16, 16, 24, 24), color=(1, 1, 0, 1))
checks.append(("triangles after lines", read(20, 20), Y))
checks.append(("lines draw only their row", read(12, 17), K))

# 4 stencil: replace 1 where a quad is, then draw only where it is 1
gl.glEnable(0x0B90)                                                   # STENCIL_TEST
gl.glStencilFunc(0x0207, 1, 0xFF); gl.glStencilOp(0x1E00, 0x1E00, 0x1E01)  # ALWAYS; KEEP KEEP REPLACE
gl.glColorMask(0, 0, 0, 0)
draw(quad(0, 24, 8, 32))
gl.glColorMask(1, 1, 1, 1)
gl.glStencilFunc(0x0202, 1, 0xFF); gl.glStencilOp(0x1E00, 0x1E00, 0x1E00)  # EQUAL
draw(quad(0, 24, 16, 32), color=(0, 1, 0, 1))
checks.append(("stencil EQUAL inside", read(4, 28), G))
checks.append(("stencil EQUAL outside", read(12, 28), K))
gl.glStencilOpSeparate(0x0405, 0x1E00, 0x1E00, 0x150A)               # BACK faces: INVERT (front unchanged)
gl.glStencilFunc(0x0207, 0, 0xFF)
draw(quad(16, 24, 24, 32), color=(1, 0, 0, 1))
gl.glDisable(0x0B90)

# 5 scissor
gl.glEnable(0x0C11); gl.glScissor(0, 32, 4, 8)
draw(quad(0, 32, 16, 40), color=(0, 0, 1, 1))
gl.glDisable(0x0C11)
checks.append(("scissor inside", read(2, 36), B))
checks.append(("scissor outside", read(10, 36), K))

# 6 rasterizer discard
gl.glEnable(0x8C89)                                                   # RASTERIZER_DISCARD
draw(quad(0, 40, 8, 48), color=(1, 0, 0, 1))
gl.glDisable(0x8C89)
checks.append(("rasterizer discard", read(4, 44), K))
draw(quad(8, 40, 16, 48), color=(0, 1, 0, 1))
checks.append(("rasterizer discard off again", read(12, 44), G))

# 7 primitive restart: a strip of two quads split by the restart index; the middle gap stays black
verts = [px(0), px(48), 0, px(8), px(48), 0, px(0), px(56), 0, px(8), px(56), 0,
         px(16), px(48), 0, px(24), px(48), 0, px(16), px(56), 0, px(24), px(56), 0]
data = (ctypes.c_float * len(verts))(*verts)
gl.glBufferData(0x8892, ctypes.sizeof(data), data, 0x88E8)
idx = (ctypes.c_ushort * 9)(0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7)
gl.glBufferData(0x8893, ctypes.sizeof(idx), idx, 0x88E8)
gl.glEnable(0x8F9D)                                                   # PRIMITIVE_RESTART_FIXED_INDEX
gl.glUniform4f(uc, 1, 1, 0, 1); gl.glUniform1f(uz, 0.0)
gl.glDrawElements(5, 9, 0x1403, None)                                 # TRIANGLE_STRIP, UNSIGNED_SHORT
gl.glDisable(0x8F9D)
checks.append(("restart: first strip", read(4, 52), Y))
checks.append(("restart: the gap", read(12, 52), K))
checks.append(("restart: second strip", read(20, 52), Y))

err = gl.glGetError()
bad = [(n, got, want) for n, got, want in checks if got != want]
for n, got, want in bad:
    print("  wrong:", n, "got", got, "want", want)
print(("RESULT ok" if not bad and not err else "RESULT WRONG"), "%d of %d" % (len(checks) - len(bad), len(checks)), "glError 0x%x" % err)
