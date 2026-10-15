#version 450
#extension GL_ARB_shader_viewport_layer_array : require
// As quad.vert, into viewport gl_InstanceIndex (shaderOutputViewportIndex): viewport_flip's two
// flipped viewports.
layout(push_constant) uniform P { vec4 rect; vec4 col; float z; } pc;
layout(location = 0) out vec4 col;
void main() {
    const int corner[6] = int[6](0, 1, 2, 2, 1, 3);
    int k = corner[gl_VertexIndex % 6];
    gl_Position = vec4((k & 1) != 0 ? pc.rect.z : pc.rect.x, (k & 2) != 0 ? pc.rect.w : pc.rect.y, pc.z, 1.0);
    gl_ViewportIndex = gl_InstanceIndex;
    col = pc.col;
}
