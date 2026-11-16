#version 450
// view3d_layers' feedback-loop sub-cases: the same triangle, the layer chosen by the geometry shader after it (fbl.geom)
layout(location = 0) flat out int inst;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    inst = gl_InstanceIndex;
}
