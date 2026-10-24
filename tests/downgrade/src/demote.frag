#version 450
#extension GL_EXT_demote_to_helper_invocation : require
layout(location = 0) in vec4 c;
layout(location = 0) out vec4 o;
void main()
{
    if (c.a < 0.5) demote;
    o = helperInvocationEXT() ? vec4(0.0) : c;
}
