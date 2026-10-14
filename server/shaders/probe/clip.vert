#version 450
// As vid.vert, with odd columns clipped away by gl_ClipDistance.
layout(push_constant) uniform P { vec4 col; float z; } pc;
layout(location = 0) out vec4 col;
out float gl_ClipDistance[1];
void main() {
    gl_Position = vec4((float(gl_VertexIndex) + 0.5) / 4.0 - 1.0, 0.0, pc.z, 1.0);
    gl_PointSize = 1.0;
    gl_ClipDistance[0] = (gl_VertexIndex & 1) != 0 ? -1.0 : 1.0;
    col = pc.col;
}
