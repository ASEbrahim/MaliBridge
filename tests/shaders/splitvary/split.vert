#version 450
// splitvary_check: as vec4.vert, but the texcoord is written split like Zink's linked GLSL programs
// (vec2 at location 1 component 0 + float at location 1 component 3).
layout(location = 0) out vec4 col;
layout(location = 1) out vec2 tc_st;
layout(location = 1, component = 3) out float tc_q;
void main() {
    const vec2 q[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 uv = q[gl_VertexIndex];
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    col = vec4(uv.x, uv.y, 0.0, 1.0);
    tc_st = uv;
    tc_q = 1.0 + uv.x;
}
