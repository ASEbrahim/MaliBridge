#version 450
// Pass-through geometry stage that only picks the layer (UE4's and DXVK's layered passes).
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in vec4 icol[];
layout(location = 0) out vec4 col;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PointSize = 1.0;
    col = icol[0];
    gl_Layer = 1;
    EmitVertex();
}
