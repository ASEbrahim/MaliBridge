#version 450
// splitvary_check: reads only a vec2 at location 1 (component 0); q taken from col.x (= u).
layout(location = 0) in vec4 col;
layout(location = 1) in vec2 tc_st;
layout(location = 0) out vec4 o;
void main() { o = vec4(tc_st, (1.0 + col.x) * 0.5, col.y); }
