#version 450
// splitvary_check: reads location 1 as one vec4 (the merged interface). Same output as split.frag.
layout(location = 0) in vec4 col;
layout(location = 1) in vec4 tc;
layout(location = 0) out vec4 o;
void main() { o = vec4(tc.xy, tc.w * 0.5, col.y); }
