#version 450
// pv3d_check: reads the 3D image itself at this texel of slice base + layer (while the pass renders it, or in a
// later subpass) and writes it red and blue swapped; mode 1: also into the buffer (a subpass with no colour)
layout(push_constant) uniform P { int first; int step; int base; int mode; } pc;
layout(set = 0, binding = 0) uniform sampler3D tex;
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
