#version 450
// vidyn_check: a point at pos, coloured col * tint (tint: a constant, stride-0 binding as Zink's)
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 col;
layout(location = 2) in vec4 tint;
layout(location = 0) out vec4 c;
void main()
{
    gl_Position = vec4(pos, 0.0, 1.0);
    gl_PointSize = 1.0;
    c = col * tint;
}
