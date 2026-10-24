#version 450
// The same values as xfb.vert, not captured here: the stream output geometry stage captures them.
out gl_PerVertex { vec4 gl_Position; }; // only the position, as DXVK declares it
layout(push_constant) uniform P { float pass; } p;
layout(location = 0) out vec4 a;
layout(location = 1) out vec2 b;
layout(location = 2) out float c;
void main() {
    float v = float(gl_VertexIndex), i = float(gl_InstanceIndex);
    a = vec4(v, i, 1.0 + p.pass, 2.0);
    b = vec2(v * 0.5, i * 0.25);
    c = v + 100.0 * i;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
