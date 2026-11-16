#version 450
// splitvary_check: reads location 1 as vec2 (component 0) + float (component 3), like Zink's
// fixed-function texturing fragment shader. Output (s, t, q/2, col.y) = (u, v, (1+u)/2, v).
layout(location = 0) in vec4 col;
layout(location = 1) in vec2 tc_st;
layout(location = 1, component = 3) in float tc_q;
layout(location = 0) out vec4 o;
void main() { o = vec4(tc_st, tc_q * 0.5, col.y); }
