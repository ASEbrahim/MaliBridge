#version 450
// fragdepth_check: a value to a colour output AND to gl_FragDepth, the shape of vkd3d-proton's layered image copy that never
// finishes on the Mali-G925 r49p1 when the rendering has no depth attachment (MK-49, server_errata_fragdepth.c)
layout(location = 0) out float o_color;
void main()
{
    o_color = 0.25;
    gl_FragDepth = 0.75;
}
