#version 450
#extension GL_ARB_shader_viewport_layer_array : require
void main()
{
    gl_Position = vec4(0.0);
    gl_Layer = gl_VertexIndex & 1;
    gl_ViewportIndex = gl_InstanceIndex & 1;
}
