#version 450
// fragdepth_check: one full-screen triangle
void main()
{
    gl_Position = vec4(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex & 2) * 2 - 1), 0.0, 1.0);
}
