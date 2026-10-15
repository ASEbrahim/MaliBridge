#version 450
#ifdef MULTI
#extension GL_ARB_shader_viewport_layer_array : require
#endif
// viewport_check: two triangles of opposite winding, asymmetric (upper left and lower right with
// y up), and their points. gl_Position.y is what the viewport_flip erratum negates; v_y carries
// the app's own y, so the fragment shader sees where the app's coordinates landed.
layout(push_constant) uniform P { int mode; float z; } pc;
layout(location = 0) out float v_y;
layout(location = 1) flat out int v_id;
const vec2 T[6] = vec2[6](vec2(-0.9, 0.15), vec2(-0.15, 0.15), vec2(-0.9, 0.85),
                          vec2(0.2, -0.1), vec2(0.85, -0.1), vec2(0.85, -0.85));
void main()
{
    vec2 p = T[gl_VertexIndex % 6];
    gl_Position = vec4(p, pc.z, 1.0);
    gl_PointSize = 4.0;
    v_y = p.y;
    v_id = gl_VertexIndex / 3;
#ifdef MULTI
    gl_ViewportIndex = gl_InstanceIndex;
#endif
}
