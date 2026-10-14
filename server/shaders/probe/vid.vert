#version 450
// One point per vertex at pixel column gl_VertexIndex of an 8x1 target; z and colour from push constants.
layout(push_constant) uniform P { vec4 col; float z; } pc;
layout(location = 0) out vec4 col;
void main() {
    gl_Position = vec4((float(gl_VertexIndex) + 0.5) / 4.0 - 1.0, 0.0, pc.z, 1.0);
    gl_PointSize = 1.0;
    col = pc.col;
}
