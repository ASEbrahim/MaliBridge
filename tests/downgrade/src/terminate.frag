#version 450
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 o;
void main()
{
    if (c.a < 0.5) discard;
    o = c;
}
