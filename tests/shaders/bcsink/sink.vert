#version 450
// bcn_check (Zink's compressed upload from a PBO): one triangle over the whole target; the viewport
// says where it lands.
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
