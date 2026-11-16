#version 450
// pv3d_check: one colour per layer, exact in UNORM8: (20 + 40 l, 200 - 30 l, 0x55, 0xff) / 255
layout(location = 0) flat in uint layer;
layout(location = 0) out vec4 o;
void main()
{
    o = vec4(float(20u + 40u * layer), float(200u - 30u * layer), 85.0, 255.0) / 255.0;
}
