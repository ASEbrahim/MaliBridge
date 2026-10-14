#version 450
// A fullscreen-ish triangle; the slice to draw into comes from a push constant, as UE4's volume passes pass it.
layout(push_constant) uniform P { int slice; } pc;
layout(location = 0) out vec2 uv;
layout(location = 2) flat out int layer_out;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    layer_out = pc.slice;
}
