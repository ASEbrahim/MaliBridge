#version 450
// pv3d_check: the triangle into layer first + instance * step (gl_Layer from a geometry shader)
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(push_constant) uniform P { int first; int step; int base; int mode; } pc;
layout(location = 0) flat in int inst[];
layout(location = 0) flat out uint layer;
void main()
{
    int l = pc.first + inst[0] * pc.step;
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        gl_Layer = l;
        layer = uint(l);
        EmitVertex();
    }
    EndPrimitive();
}
