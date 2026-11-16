#version 450
// view3d_layers' feedback-loop sub-cases: the triangle into layer inst (gl_Layer from a geometry shader)
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in int inst[];
layout(location = 0) flat out uint layer;
void main()
{
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        gl_Layer = inst[0];
        layer = uint(inst[0]);
        EmitVertex();
    }
    EndPrimitive();
}
