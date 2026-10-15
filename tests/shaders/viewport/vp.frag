#version 450
// viewport_check: what rasterization made of each fragment, as exact as it can be read back
//   r  facing (1 front, 2 back) | primitive << 4 | sign of dFdy(v_y) << 8 (1 +, 2 -, 3 zero)
//   g  gl_FragCoord.y    b  gl_FragCoord.z    a  v_y (the app's y here), or gl_PointCoord.y
layout(push_constant) uniform P { int mode; float z; } pc;
layout(location = 0) in float v_y;
layout(location = 1) flat in int v_id;
layout(location = 0) out uvec4 o;
void main()
{
    float dy = dFdy(v_y);
    uint sy = dy > 0.0 ? 1u : dy < 0.0 ? 2u : 3u;
    o.r = (gl_FrontFacing ? 1u : 2u) | (uint(v_id) << 4) | (sy << 8);
    o.g = floatBitsToUint(gl_FragCoord.y);
    o.b = floatBitsToUint(gl_FragCoord.z);
    o.a = pc.mode == 1 ? floatBitsToUint(gl_PointCoord.y) : floatBitsToUint(v_y);
}
