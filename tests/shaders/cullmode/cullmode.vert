#version 450
// cullmode_check: pc.mode.x picks the primitive - 0: a clockwise triangle over the whole 4x4 target,
// 1: a line along pixel row 1, 2: a point on pixel (1, 1); coloured pc.color.
layout(push_constant) uniform P { vec4 color; vec4 mode; } pc;
layout(location = 0) out vec4 c;
void main() {
    int m = int(pc.mode.x);
    vec2 p;
    if (m == 0) p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    else if (m == 1) p = vec2(gl_VertexIndex == 0 ? -1.0 : 1.0, -0.25);
    else p = vec2(-0.25, -0.25);
    gl_Position = vec4(p, 0.5, 1.0);
    gl_PointSize = 1.0;
    c = pc.color;
}
