#version 450
// dynstate_check: a triangle over the whole 4x4 target at depth pc.zf.x, clockwise or (pc.zf.y > 0.5)
// counter-clockwise; its colour is pc.color plus the instance-rate attribute a (vertex strides).
layout(location = 0) in vec4 a;
layout(push_constant) uniform P { vec4 color; vec4 zf; } pc;
layout(location = 0) out vec4 c;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    if (pc.zf.y > 0.5) p = p.yx;
    gl_Position = vec4(p * 2.0 - 1.0, pc.zf.x, 1.0);
    c = pc.color + a;
}
