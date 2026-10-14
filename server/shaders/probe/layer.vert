#version 450
#extension GL_ARB_shader_viewport_layer_array : require
// As vid.vert, drawn into layer 1 by the vertex shader (shaderOutputLayer).
layout(push_constant) uniform P { vec4 col; float z; } pc;
layout(location = 0) out vec4 col;
void main() {
    gl_Position = vec4((float(gl_VertexIndex) + 0.5) / 4.0 - 1.0, 0.0, pc.z, 1.0);
    gl_PointSize = 1.0;
    gl_Layer = 1;
    col = pc.col;
}
