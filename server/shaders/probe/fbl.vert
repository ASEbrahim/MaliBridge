#version 450
#extension GL_ARB_shader_viewport_layer_array : require
// view3d_layers' feedback-loop sub-cases (pv_view3d_base): a full-screen triangle into layer gl_InstanceIndex (gl_Layer from the vertex shader)
layout(location = 0) flat out uint layer;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    gl_Layer = gl_InstanceIndex;
    layer = uint(gl_InstanceIndex);
}
