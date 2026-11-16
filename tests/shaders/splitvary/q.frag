#version 450
// splitvary_check: reads only a float at location 1 component 3; s,t from col.
layout(location = 0) in vec4 col;
layout(location = 1, component = 3) in float tc_q;
layout(location = 0) out vec4 o;
void main() { o = vec4(col.xy, tc_q * 0.5, col.y); }
