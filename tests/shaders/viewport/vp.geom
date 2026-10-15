#version 450
// viewport_check: a pass-through geometry stage (the erratum flips before each EmitVertex)
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in float i_y[];
layout(location = 1) flat in int i_id[];
layout(location = 0) out float v_y;
layout(location = 1) flat out int v_id;
void main()
{
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        v_y = i_y[i];
        v_id = i_id[i];
        EmitVertex();
    }
    EndPrimitive();
}
