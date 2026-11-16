#version 450
// pv3d_check: a full-screen triangle, the layer chosen by the geometry shader after it
layout(push_constant) uniform P { int first; int step; int base; int mode; } pc;
layout(location = 0) flat out int inst;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    inst = gl_InstanceIndex;
}
