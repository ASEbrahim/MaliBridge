#version 450
// Captures its own outputs (VK_EXT_transform_feedback in the vertex stage): buffer 0 holds a and b
// with an 8-byte gap per vertex (stride 32), buffer 1 holds c.
out gl_PerVertex { vec4 gl_Position; }; // only the position, as DXVK declares it
layout(push_constant) uniform P { float pass; } p;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 32) out vec4 a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 16) out vec2 b;
layout(location = 2, xfb_buffer = 1, xfb_offset = 0, xfb_stride = 4) out float c;
void main() {
    float v = float(gl_VertexIndex), i = float(gl_InstanceIndex);
    a = vec4(v, i, 1.0 + p.pass, 2.0);
    b = vec2(v * 0.5, i * 0.25);
    c = v + 100.0 * i;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
