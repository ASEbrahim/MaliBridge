#version 450
// pv3d_check, the plain 2D-array image cases (pv_fbloop_split, PV-06): pv3d_rd.frag's read of the layer the pass renders
// (the image itself, while the pass renders it, or in a later subpass), from a sampler2DArray
layout(push_constant) uniform P { int first; int step; int base; int mode; } pc;
layout(set = 0, binding = 0) uniform sampler2DArray tex;
layout(std430, set = 0, binding = 1) buffer B { uint o[]; } buf;
layout(location = 0) flat in uint layer;
layout(location = 0) out vec4 o;
void main()
{
    ivec2 xy = ivec2(gl_FragCoord.xy);
    vec4 t = texelFetch(tex, ivec3(xy, pc.base + int(layer)), 0);
    o = t.bgra;
    if (pc.mode == 1) {
        ivec3 s = textureSize(tex, 0);
        buf.o[(int(layer) * s.y + xy.y) * s.x + xy.x] = packUnorm4x8(t);
    }
}
