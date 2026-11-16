#version 450
#extension GL_ARB_shader_viewport_layer_array : require
// pv3d_check: a full-screen triangle into layer first + instance * step (gl_Layer from the vertex shader)
layout(push_constant) uniform P { int first; int step; int base; int mode; } pc;
layout(location = 0) flat out uint layer;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    int l = pc.first + gl_InstanceIndex * pc.step;
    gl_Layer = l;
    layer = uint(l);
}
