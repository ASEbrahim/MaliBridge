#version 450
// D3D11 stream output as DXVK builds it: a point in, its inputs copied to captured outputs, a point out.
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in vec4 a_in[];
layout(location = 1) in vec2 b_in[];
layout(location = 2) in float c_in[];
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 32) out vec4 a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 16) out vec2 b;
layout(location = 2, xfb_buffer = 1, xfb_offset = 0, xfb_stride = 4) out float c;
void main() {
    a = a_in[0];
    b = b_in[0];
    c = c_in[0];
    EmitVertex();
}
