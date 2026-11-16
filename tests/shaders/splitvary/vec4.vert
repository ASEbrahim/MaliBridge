#version 450
// splitvary_check: a quad over the whole target (two triangles, gl_VertexIndex 0..5), u,v = 0..1 across it.
// Writes the texcoord as ONE vec4 at location 1 (like Zink's fixed-function vertex shader).
layout(location = 0) out vec4 col;
layout(location = 1) out vec4 tc;
void main() {
    const vec2 q[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 uv = q[gl_VertexIndex];
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    col = vec4(uv.x, uv.y, 0.0, 1.0);
    tc = vec4(uv.x, uv.y, 0.75, 1.0 + uv.x);
}
