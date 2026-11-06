#version 450
// bcn_check: what Mesa's st_CompressedTexSubImage does with a pixel buffer bound - it renders the BC
// blocks, one texel a block, into an R32G32B32A32_UINT (16-byte blocks) or R16G16B16A16_UINT (8-byte
// blocks) view of the compressed texture. Block (x - ox, y - oy) of the buffer, `pitch` blocks a row.
layout(std430, set = 0, binding = 0) readonly buffer Blocks { uint w[]; };
layout(push_constant) uniform P { int ox; int oy; int pitch; int half16; } pc;
layout(location = 0) out uvec4 o;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int i = (p.y - pc.oy) * pc.pitch + (p.x - pc.ox);
    if (pc.half16 == 0) {
        o = uvec4(w[4 * i], w[4 * i + 1], w[4 * i + 2], w[4 * i + 3]);
    } else {
        uint a = w[2 * i], b = w[2 * i + 1];
        o = uvec4(a & 0xffffu, a >> 16, b & 0xffffu, b >> 16);
    }
}
