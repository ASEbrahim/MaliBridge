/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * nulldesc_check: null image descriptors (robustness2's nullDescriptor) of every view type and
 * numeric class a shader can declare - cube, 3D, uint 2D, sint 2D array, cube array, 1D, an r32ui
 * storage image and a 3D storage image - written null and read by a compute dispatch, through
 * vkUpdateDescriptorSets, an update template and push descriptors. Where the GPU lacks robustness2
 * (Mali r44; on the host VKBRIDGE_FAKE_MISSING=robustness2) the server puts stand-in views in
 * their place (server_nulldesc.c); a stand-in must have the shape the shader declares. The
 * dispatch reports every binding's size: a real null descriptor gives 0 in every dimension, a
 * stand-in 1 in every dimension (cube arrays count cubes, so a 2D stand-in reads as 0 cubes),
 * and what each reads: 0 either way (a stand-in is cleared to zero before the first submission).
 * Then a write through the null storage image: the sampled nulls still read 0 after it.
 *
 *   cc -O2 -Wall -Ithird_party/Vulkan-Headers-1.4.341/include -Icommon -Icommon/gen -Itests \
 *      -o host/out/nulldesc_check tests/nulldesc_check.c -ldl          (from the repository root)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glslc -O --target-env=vulkan1.1 of:
 *   layout(local_size_x = 1) in;
 *   layout(set = 0, binding = 0) uniform samplerCube tc;
 *   layout(set = 0, binding = 1) uniform sampler3D t3;
 *   layout(set = 0, binding = 2) uniform usampler2D tu;
 *   layout(set = 0, binding = 3) uniform isampler2DArray ti;
 *   layout(set = 0, binding = 4) uniform samplerCubeArray tca;
 *   layout(set = 0, binding = 5) uniform sampler1D t1;
 *   layout(set = 0, binding = 6, r32ui) uniform uimage2D su;
 *   layout(set = 0, binding = 7, rgba8) uniform readonly image3D s3;
 *   layout(set = 1, binding = 0, std430) buffer O { uint o[32]; };
 *   void main() {
 *       ivec2 a = textureSize(tc, 0); o[0] = a.x; o[1] = a.y;
 *       ivec3 b = textureSize(t3, 0); o[2] = b.x; o[3] = b.y; o[4] = b.z;
 *       ivec2 c = textureSize(tu, 0); o[5] = c.x; o[6] = c.y;
 *       ivec3 e = textureSize(ti, 0); o[7] = e.x; o[8] = e.y; o[9] = e.z;
 *       ivec3 f = textureSize(tca, 0); o[10] = f.x; o[11] = f.y; o[12] = f.z;
 *       o[13] = textureSize(t1, 0);
 *       ivec2 h = imageSize(su); o[14] = h.x; o[15] = h.y;
 *       ivec3 k = imageSize(s3); o[16] = k.x; o[17] = k.y; o[18] = k.z;
 *       o[19] = floatBitsToUint(textureLod(tc, vec3(0.3, -0.5, 1.0), 0.0).x);
 *       o[20] = floatBitsToUint(textureLod(t3, vec3(0.5), 0.0).x);
 *       o[21] = texelFetch(tu, ivec2(0), 0).x;
 *       o[22] = uint(texelFetch(ti, ivec3(0), 0).x);
 *       o[23] = floatBitsToUint(textureLod(tca, vec4(0.3, -0.5, 1.0, 0.0), 0.0).x);
 *       o[24] = floatBitsToUint(textureLod(t1, 0.5, 0.0).x);
 *       o[25] = imageLoad(su, ivec2(0)).x;
 *       o[26] = floatBitsToUint(imageLoad(s3, ivec3(0)).x);
 *   } */
static const uint32_t spv_nd_comp[] = {
    0x07230203,0x00010300,0x000d000a,0x000000ef,0x00000000,0x00020011,
    0x00000001,0x00020011,0x0000002b,0x00020011,0x0000002d,0x00020011,
    0x00000032,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,0x0005000f,0x00000005,
    0x00000004,0x6e69616d,0x00000000,0x00060010,0x00000004,0x00000011,
    0x00000001,0x00000001,0x00000001,0x00040047,0x0000000e,0x00000022,
    0x00000000,0x00040047,0x0000000e,0x00000021,0x00000000,0x00040047,
    0x00000015,0x00000006,0x00000004,0x00050048,0x00000016,0x00000000,
    0x00000023,0x00000000,0x00030047,0x00000016,0x00000002,0x00040047,
    0x00000018,0x00000022,0x00000001,0x00040047,0x00000018,0x00000021,
    0x00000000,0x00040047,0x0000002c,0x00000022,0x00000000,0x00040047,
    0x0000002c,0x00000021,0x00000001,0x00040047,0x00000044,0x00000022,
    0x00000000,0x00040047,0x00000044,0x00000021,0x00000002,0x00040047,
    0x00000056,0x00000022,0x00000000,0x00040047,0x00000056,0x00000021,
    0x00000003,0x00040047,0x0000006d,0x00000022,0x00000000,0x00040047,
    0x0000006d,0x00000021,0x00000004,0x00040047,0x00000084,0x00000022,
    0x00000000,0x00040047,0x00000084,0x00000021,0x00000005,0x00040047,
    0x0000008d,0x00000022,0x00000000,0x00040047,0x0000008d,0x00000021,
    0x00000006,0x00040047,0x0000009d,0x00000022,0x00000000,0x00040047,
    0x0000009d,0x00000021,0x00000007,0x00030047,0x0000009d,0x00000018,
    0x00040047,0x000000ee,0x0000000b,0x00000019,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,
    0x00000001,0x00040017,0x00000007,0x00000006,0x00000002,0x00030016,
    0x0000000a,0x00000020,0x00090019,0x0000000b,0x0000000a,0x00000003,
    0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,
    0x0000000c,0x0000000b,0x00040020,0x0000000d,0x00000000,0x0000000c,
    0x0004003b,0x0000000d,0x0000000e,0x00000000,0x0004002b,0x00000006,
    0x00000010,0x00000000,0x00040015,0x00000013,0x00000020,0x00000000,
    0x0004002b,0x00000013,0x00000014,0x00000020,0x0004001c,0x00000015,
    0x00000013,0x00000014,0x0003001e,0x00000016,0x00000015,0x00040020,
    0x00000017,0x0000000c,0x00000016,0x0004003b,0x00000017,0x00000018,
    0x0000000c,0x00040020,0x0000001e,0x0000000c,0x00000013,0x0004002b,
    0x00000006,0x00000020,0x00000001,0x0004002b,0x00000013,0x00000021,
    0x00000001,0x00040017,0x00000026,0x00000006,0x00000003,0x00090019,
    0x00000029,0x0000000a,0x00000002,0x00000000,0x00000000,0x00000000,
    0x00000001,0x00000000,0x0003001b,0x0000002a,0x00000029,0x00040020,
    0x0000002b,0x00000000,0x0000002a,0x0004003b,0x0000002b,0x0000002c,
    0x00000000,0x0004002b,0x00000006,0x00000030,0x00000002,0x0004002b,
    0x00000006,0x00000035,0x00000003,0x0004002b,0x00000006,0x0000003a,
    0x00000004,0x00090019,0x00000041,0x00000013,0x00000001,0x00000000,
    0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,0x00000042,
    0x00000041,0x00040020,0x00000043,0x00000000,0x00000042,0x0004003b,
    0x00000043,0x00000044,0x00000000,0x0004002b,0x00000006,0x00000048,
    0x00000005,0x0004002b,0x00000006,0x0000004d,0x00000006,0x00090019,
    0x00000053,0x00000006,0x00000001,0x00000000,0x00000001,0x00000000,
    0x00000001,0x00000000,0x0003001b,0x00000054,0x00000053,0x00040020,
    0x00000055,0x00000000,0x00000054,0x0004003b,0x00000055,0x00000056,
    0x00000000,0x0004002b,0x00000006,0x0000005a,0x00000007,0x0004002b,
    0x00000006,0x0000005f,0x00000008,0x0004002b,0x00000006,0x00000064,
    0x00000009,0x00090019,0x0000006a,0x0000000a,0x00000003,0x00000000,
    0x00000001,0x00000000,0x00000001,0x00000000,0x0003001b,0x0000006b,
    0x0000006a,0x00040020,0x0000006c,0x00000000,0x0000006b,0x0004003b,
    0x0000006c,0x0000006d,0x00000000,0x0004002b,0x00000006,0x00000071,
    0x0000000a,0x0004002b,0x00000006,0x00000076,0x0000000b,0x0004002b,
    0x00000006,0x0000007b,0x0000000c,0x0004002b,0x00000006,0x00000080,
    0x0000000d,0x00090019,0x00000081,0x0000000a,0x00000000,0x00000000,
    0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,0x00000082,
    0x00000081,0x00040020,0x00000083,0x00000000,0x00000082,0x0004003b,
    0x00000083,0x00000084,0x00000000,0x00090019,0x0000008b,0x00000013,
    0x00000001,0x00000000,0x00000000,0x00000000,0x00000002,0x00000021,
    0x00040020,0x0000008c,0x00000000,0x0000008b,0x0004003b,0x0000008c,
    0x0000008d,0x00000000,0x0004002b,0x00000006,0x00000090,0x0000000e,
    0x0004002b,0x00000006,0x00000095,0x0000000f,0x00090019,0x0000009b,
    0x0000000a,0x00000002,0x00000000,0x00000000,0x00000000,0x00000002,
    0x00000004,0x00040020,0x0000009c,0x00000000,0x0000009b,0x0004003b,
    0x0000009c,0x0000009d,0x00000000,0x0004002b,0x00000006,0x000000a0,
    0x00000010,0x0004002b,0x00000006,0x000000a5,0x00000011,0x0004002b,
    0x00000006,0x000000aa,0x00000012,0x0004002b,0x00000006,0x000000af,
    0x00000013,0x00040017,0x000000b1,0x0000000a,0x00000003,0x0004002b,
    0x0000000a,0x000000b2,0x3e99999a,0x0004002b,0x0000000a,0x000000b3,
    0xbf000000,0x0004002b,0x0000000a,0x000000b4,0x3f800000,0x0006002c,
    0x000000b1,0x000000b5,0x000000b2,0x000000b3,0x000000b4,0x0004002b,
    0x0000000a,0x000000b6,0x00000000,0x00040017,0x000000b7,0x0000000a,
    0x00000004,0x0004002b,0x00000006,0x000000bc,0x00000014,0x0004002b,
    0x0000000a,0x000000be,0x3f000000,0x0006002c,0x000000b1,0x000000bf,
    0x000000be,0x000000be,0x000000be,0x0004002b,0x00000006,0x000000c4,
    0x00000015,0x0005002c,0x00000007,0x000000c6,0x00000010,0x00000010,
    0x00040017,0x000000c8,0x00000013,0x00000004,0x0004002b,0x00000006,
    0x000000cc,0x00000016,0x0006002c,0x00000026,0x000000ce,0x00000010,
    0x00000010,0x00000010,0x00040017,0x000000d0,0x00000006,0x00000004,
    0x0004002b,0x00000006,0x000000d5,0x00000017,0x0007002c,0x000000b7,
    0x000000d7,0x000000b2,0x000000b3,0x000000b4,0x000000b6,0x0004002b,
    0x00000006,0x000000dc,0x00000018,0x0004002b,0x00000006,0x000000e2,
    0x00000019,0x0004002b,0x00000006,0x000000e7,0x0000001a,0x00040017,
    0x000000ed,0x00000013,0x00000003,0x0006002c,0x000000ed,0x000000ee,
    0x00000021,0x00000021,0x00000021,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x0000000c,
    0x0000000f,0x0000000e,0x00040064,0x0000000b,0x00000011,0x0000000f,
    0x00050067,0x00000007,0x00000012,0x00000011,0x00000010,0x00050051,
    0x00000006,0x0000001c,0x00000012,0x00000000,0x0004007c,0x00000013,
    0x0000001d,0x0000001c,0x00060041,0x0000001e,0x0000001f,0x00000018,
    0x00000010,0x00000010,0x0003003e,0x0000001f,0x0000001d,0x00050051,
    0x00000006,0x00000023,0x00000012,0x00000001,0x0004007c,0x00000013,
    0x00000024,0x00000023,0x00060041,0x0000001e,0x00000025,0x00000018,
    0x00000010,0x00000020,0x0003003e,0x00000025,0x00000024,0x0004003d,
    0x0000002a,0x0000002d,0x0000002c,0x00040064,0x00000029,0x0000002e,
    0x0000002d,0x00050067,0x00000026,0x0000002f,0x0000002e,0x00000010,
    0x00050051,0x00000006,0x00000032,0x0000002f,0x00000000,0x0004007c,
    0x00000013,0x00000033,0x00000032,0x00060041,0x0000001e,0x00000034,
    0x00000018,0x00000010,0x00000030,0x0003003e,0x00000034,0x00000033,
    0x00050051,0x00000006,0x00000037,0x0000002f,0x00000001,0x0004007c,
    0x00000013,0x00000038,0x00000037,0x00060041,0x0000001e,0x00000039,
    0x00000018,0x00000010,0x00000035,0x0003003e,0x00000039,0x00000038,
    0x00050051,0x00000006,0x0000003d,0x0000002f,0x00000002,0x0004007c,
    0x00000013,0x0000003e,0x0000003d,0x00060041,0x0000001e,0x0000003f,
    0x00000018,0x00000010,0x0000003a,0x0003003e,0x0000003f,0x0000003e,
    0x0004003d,0x00000042,0x00000045,0x00000044,0x00040064,0x00000041,
    0x00000046,0x00000045,0x00050067,0x00000007,0x00000047,0x00000046,
    0x00000010,0x00050051,0x00000006,0x0000004a,0x00000047,0x00000000,
    0x0004007c,0x00000013,0x0000004b,0x0000004a,0x00060041,0x0000001e,
    0x0000004c,0x00000018,0x00000010,0x00000048,0x0003003e,0x0000004c,
    0x0000004b,0x00050051,0x00000006,0x0000004f,0x00000047,0x00000001,
    0x0004007c,0x00000013,0x00000050,0x0000004f,0x00060041,0x0000001e,
    0x00000051,0x00000018,0x00000010,0x0000004d,0x0003003e,0x00000051,
    0x00000050,0x0004003d,0x00000054,0x00000057,0x00000056,0x00040064,
    0x00000053,0x00000058,0x00000057,0x00050067,0x00000026,0x00000059,
    0x00000058,0x00000010,0x00050051,0x00000006,0x0000005c,0x00000059,
    0x00000000,0x0004007c,0x00000013,0x0000005d,0x0000005c,0x00060041,
    0x0000001e,0x0000005e,0x00000018,0x00000010,0x0000005a,0x0003003e,
    0x0000005e,0x0000005d,0x00050051,0x00000006,0x00000061,0x00000059,
    0x00000001,0x0004007c,0x00000013,0x00000062,0x00000061,0x00060041,
    0x0000001e,0x00000063,0x00000018,0x00000010,0x0000005f,0x0003003e,
    0x00000063,0x00000062,0x00050051,0x00000006,0x00000066,0x00000059,
    0x00000002,0x0004007c,0x00000013,0x00000067,0x00000066,0x00060041,
    0x0000001e,0x00000068,0x00000018,0x00000010,0x00000064,0x0003003e,
    0x00000068,0x00000067,0x0004003d,0x0000006b,0x0000006e,0x0000006d,
    0x00040064,0x0000006a,0x0000006f,0x0000006e,0x00050067,0x00000026,
    0x00000070,0x0000006f,0x00000010,0x00050051,0x00000006,0x00000073,
    0x00000070,0x00000000,0x0004007c,0x00000013,0x00000074,0x00000073,
    0x00060041,0x0000001e,0x00000075,0x00000018,0x00000010,0x00000071,
    0x0003003e,0x00000075,0x00000074,0x00050051,0x00000006,0x00000078,
    0x00000070,0x00000001,0x0004007c,0x00000013,0x00000079,0x00000078,
    0x00060041,0x0000001e,0x0000007a,0x00000018,0x00000010,0x00000076,
    0x0003003e,0x0000007a,0x00000079,0x00050051,0x00000006,0x0000007d,
    0x00000070,0x00000002,0x0004007c,0x00000013,0x0000007e,0x0000007d,
    0x00060041,0x0000001e,0x0000007f,0x00000018,0x00000010,0x0000007b,
    0x0003003e,0x0000007f,0x0000007e,0x0004003d,0x00000082,0x00000085,
    0x00000084,0x00040064,0x00000081,0x00000086,0x00000085,0x00050067,
    0x00000006,0x00000087,0x00000086,0x00000010,0x0004007c,0x00000013,
    0x00000088,0x00000087,0x00060041,0x0000001e,0x00000089,0x00000018,
    0x00000010,0x00000080,0x0003003e,0x00000089,0x00000088,0x0004003d,
    0x0000008b,0x0000008e,0x0000008d,0x00040068,0x00000007,0x0000008f,
    0x0000008e,0x00050051,0x00000006,0x00000092,0x0000008f,0x00000000,
    0x0004007c,0x00000013,0x00000093,0x00000092,0x00060041,0x0000001e,
    0x00000094,0x00000018,0x00000010,0x00000090,0x0003003e,0x00000094,
    0x00000093,0x00050051,0x00000006,0x00000097,0x0000008f,0x00000001,
    0x0004007c,0x00000013,0x00000098,0x00000097,0x00060041,0x0000001e,
    0x00000099,0x00000018,0x00000010,0x00000095,0x0003003e,0x00000099,
    0x00000098,0x0004003d,0x0000009b,0x0000009e,0x0000009d,0x00040068,
    0x00000026,0x0000009f,0x0000009e,0x00050051,0x00000006,0x000000a2,
    0x0000009f,0x00000000,0x0004007c,0x00000013,0x000000a3,0x000000a2,
    0x00060041,0x0000001e,0x000000a4,0x00000018,0x00000010,0x000000a0,
    0x0003003e,0x000000a4,0x000000a3,0x00050051,0x00000006,0x000000a7,
    0x0000009f,0x00000001,0x0004007c,0x00000013,0x000000a8,0x000000a7,
    0x00060041,0x0000001e,0x000000a9,0x00000018,0x00000010,0x000000a5,
    0x0003003e,0x000000a9,0x000000a8,0x00050051,0x00000006,0x000000ac,
    0x0000009f,0x00000002,0x0004007c,0x00000013,0x000000ad,0x000000ac,
    0x00060041,0x0000001e,0x000000ae,0x00000018,0x00000010,0x000000aa,
    0x0003003e,0x000000ae,0x000000ad,0x00070058,0x000000b7,0x000000b8,
    0x0000000f,0x000000b5,0x00000002,0x000000b6,0x00050051,0x0000000a,
    0x000000b9,0x000000b8,0x00000000,0x0004007c,0x00000013,0x000000ba,
    0x000000b9,0x00060041,0x0000001e,0x000000bb,0x00000018,0x00000010,
    0x000000af,0x0003003e,0x000000bb,0x000000ba,0x00070058,0x000000b7,
    0x000000c0,0x0000002d,0x000000bf,0x00000002,0x000000b6,0x00050051,
    0x0000000a,0x000000c1,0x000000c0,0x00000000,0x0004007c,0x00000013,
    0x000000c2,0x000000c1,0x00060041,0x0000001e,0x000000c3,0x00000018,
    0x00000010,0x000000bc,0x0003003e,0x000000c3,0x000000c2,0x00040064,
    0x00000041,0x000000c7,0x00000045,0x0007005f,0x000000c8,0x000000c9,
    0x000000c7,0x000000c6,0x00000002,0x00000010,0x00050051,0x00000013,
    0x000000ca,0x000000c9,0x00000000,0x00060041,0x0000001e,0x000000cb,
    0x00000018,0x00000010,0x000000c4,0x0003003e,0x000000cb,0x000000ca,
    0x00040064,0x00000053,0x000000cf,0x00000057,0x0007005f,0x000000d0,
    0x000000d1,0x000000cf,0x000000ce,0x00000002,0x00000010,0x00050051,
    0x00000006,0x000000d2,0x000000d1,0x00000000,0x0004007c,0x00000013,
    0x000000d3,0x000000d2,0x00060041,0x0000001e,0x000000d4,0x00000018,
    0x00000010,0x000000cc,0x0003003e,0x000000d4,0x000000d3,0x00070058,
    0x000000b7,0x000000d8,0x0000006e,0x000000d7,0x00000002,0x000000b6,
    0x00050051,0x0000000a,0x000000d9,0x000000d8,0x00000000,0x0004007c,
    0x00000013,0x000000da,0x000000d9,0x00060041,0x0000001e,0x000000db,
    0x00000018,0x00000010,0x000000d5,0x0003003e,0x000000db,0x000000da,
    0x00070058,0x000000b7,0x000000de,0x00000085,0x000000be,0x00000002,
    0x000000b6,0x00050051,0x0000000a,0x000000df,0x000000de,0x00000000,
    0x0004007c,0x00000013,0x000000e0,0x000000df,0x00060041,0x0000001e,
    0x000000e1,0x00000018,0x00000010,0x000000dc,0x0003003e,0x000000e1,
    0x000000e0,0x0004003d,0x0000008b,0x000000e3,0x0000008d,0x00050062,
    0x000000c8,0x000000e4,0x000000e3,0x000000c6,0x00050051,0x00000013,
    0x000000e5,0x000000e4,0x00000000,0x00060041,0x0000001e,0x000000e6,
    0x00000018,0x00000010,0x000000e2,0x0003003e,0x000000e6,0x000000e5,
    0x00050062,0x000000b7,0x000000e9,0x0000009e,0x000000ce,0x00050051,
    0x0000000a,0x000000ea,0x000000e9,0x00000000,0x0004007c,0x00000013,
    0x000000eb,0x000000ea,0x00060041,0x0000001e,0x000000ec,0x00000018,
    0x00000010,0x000000e7,0x0003003e,0x000000ec,0x000000eb,0x000100fd,
    0x00010038
};

/* Written by hand (SPIR-V 1.0), the same as:
 *   layout(local_size_x = 1) in;
 *   layout(set = 0, binding = 6, r32ui) uniform uimage2D su;
 *   void main() { imageStore(su, ivec2(0), uvec4(0xdeadbeef)); } */
static const uint32_t spv_nd_write[] = {
    0x07230203, 0x00010000, 0x00000000, 17, 0,
    0x00020011, 1,                                   /* OpCapability Shader */
    0x0003000e, 0, 1,                                /* OpMemoryModel Logical GLSL450 */
    0x0005000f, 5, 1, 0x6e69616d, 0x00000000,        /* OpEntryPoint GLCompute %1 "main" */
    0x00060010, 1, 17, 1, 1, 1,                      /* OpExecutionMode %1 LocalSize 1 1 1 */
    0x00040047, 2, 34, 0,                            /* OpDecorate %2 DescriptorSet 0 */
    0x00040047, 2, 33, 6,                            /* OpDecorate %2 Binding 6 */
    0x00020013, 3,                                   /* %3 = OpTypeVoid */
    0x00030021, 4, 3,                                /* %4 = OpTypeFunction %3 */
    0x00040015, 5, 32, 0,                            /* %5 = OpTypeInt 32 0 */
    0x00040015, 6, 32, 1,                            /* %6 = OpTypeInt 32 1 */
    0x00040017, 7, 6, 2,                             /* %7 = OpTypeVector %6 2 */
    0x00040017, 8, 5, 4,                             /* %8 = OpTypeVector %5 4 */
    0x00090019, 9, 5, 1, 0, 0, 0, 2, 33,             /* %9 = OpTypeImage %5 2D 0 0 0 2 R32ui */
    0x00040020, 10, 0, 9,                            /* %10 = OpTypePointer UniformConstant %9 */
    0x0004003b, 10, 2, 0,                            /* %2 = OpVariable %10 UniformConstant */
    0x0004002b, 6, 11, 0,                            /* %11 = OpConstant %6 0 */
    0x0005002c, 7, 12, 11, 11,                       /* %12 = OpConstantComposite %7 %11 %11 */
    0x0004002b, 5, 13, 0xdeadbeef,                   /* %13 = OpConstant %5 0xdeadbeef */
    0x0007002c, 8, 14, 13, 13, 13, 13,               /* %14 = OpConstantComposite %8 %13 %13 %13 %13 */
    0x00050036, 3, 1, 0, 4,                          /* %1 = OpFunction %3 None %4 */
    0x000200f8, 15,                                  /* %15 = OpLabel */
    0x0004003d, 9, 16, 2,                            /* %16 = OpLoad %9 %2 */
    0x00040063, 16, 12, 14,                          /* OpImageWrite %16 %12 %14 */
    0x000100fd,                                      /* OpReturn */
    0x00010038,                                      /* OpFunctionEnd */
};

#define NB 8 /* image bindings in set 0 */

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceFeatures2); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkEnumerateDeviceExtensionProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance); I(vkDestroyDevice); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements);
I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdPipelineBarrier); I(vkCreateShaderModule); I(vkCreateSampler);
I(vkCreateDescriptorSetLayout); I(vkCreatePipelineLayout); I(vkCreateComputePipelines); I(vkCreateDescriptorPool);
I(vkAllocateDescriptorSets); I(vkUpdateDescriptorSets); I(vkCreateDescriptorUpdateTemplate); I(vkUpdateDescriptorSetWithTemplate);
I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets); I(vkCmdDispatch); I(vkCmdPushDescriptorSetKHR);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* Set 0's bindings as the shader declares them. */
static const VkDescriptorType types[NB] = {
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};

/* The sizes the dispatch wrote, binding by binding: [first word, dimensions]. */
static const struct { const char *name; int at, dims; } shapes[NB] = {
    {"cube", 0, 2}, {"3D", 2, 3}, {"usampler2D", 5, 2}, {"isampler2DArray", 7, 3},
    {"cube array", 10, 3}, {"1D", 13, 1}, {"uimage2D r32ui", 14, 2}, {"image3D rgba8", 16, 3}};

/* Each binding's size must be all zeros (a null descriptor) or all ones (a 1x1 stand-in of the
 * declared shape). */
static void check_sizes(const uint32_t *o, const char *how)
{
    char d[600] = "", what[96];
    int bad = 0;
    for (int i = 0; i < NB; i++) {
        int zeros = 0, ones = 0;
        for (int k = 0; k < shapes[i].dims; k++) {
            zeros += o[shapes[i].at + k] == 0;
            ones += o[shapes[i].at + k] == 1;
        }
        int ok = zeros == shapes[i].dims || ones == shapes[i].dims;
        size_t l = strlen(d);
        snprintf(d + l, sizeof(d) - l, "%s%s%s (", l ? ", " : "", ok ? "" : "WRONG ", shapes[i].name);
        for (int k = 0; k < shapes[i].dims; k++) {
            l = strlen(d);
            snprintf(d + l, sizeof(d) - l, "%s%d", k ? "x" : "", (int)o[shapes[i].at + k]);
        }
        l = strlen(d);
        snprintf(d + l, sizeof(d) - l, ")");
        bad |= !ok;
    }
    snprintf(what, sizeof(what), "null image descriptors, %s", how);
    check(!bad, what, d);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-nulldesc", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        check(0, "instance", NULL);
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceFeatures2); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkEnumerateDeviceExtensionProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
    G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        check(0, "physical device", "none");
        return 1;
    }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    const char *exts[2];
    uint32_t nexts = 0;
    int has_r2 = 0, has_push = 0;
    for (uint32_t i = 0; i < ne; i++) {
        if (!strcmp(ep[i].extensionName, "VK_EXT_robustness2")) has_r2 = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_push_descriptor")) has_push = 1;
    }
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, has_r2 ? &r2 : NULL};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!has_r2 || !r2.nullDescriptor || !f2.features.imageCubeArray) {
        printf("OK: skipped (no nullDescriptor or imageCubeArray)\n");
        return 0;
    }
    exts[nexts++] = "VK_EXT_robustness2";
    if (has_push) exts[nexts++] = "VK_KHR_push_descriptor";
    VkPhysicalDeviceRobustness2FeaturesEXT er2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    er2.nullDescriptor = VK_TRUE;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &er2};
    ef.features.imageCubeArray = VK_TRUE;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, nexts, exts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        check(0, "device", NULL);
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkDestroyDevice); G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkQueueWaitIdle); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCmdPipelineBarrier); G(vkCreateShaderModule); G(vkCreateSampler);
    G(vkCreateDescriptorSetLayout); G(vkCreatePipelineLayout); G(vkCreateComputePipelines); G(vkCreateDescriptorPool);
    G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets); G(vkCreateDescriptorUpdateTemplate); G(vkUpdateDescriptorSetWithTemplate);
    G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdDispatch);
    if (has_push) G(vkCmdPushDescriptorSetKHR);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPool cpool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);

    /* The output buffer, host-visible and coherent. */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer out;
    VkMemoryRequirements req;
    VkDeviceMemory mem;
    uint32_t *o = NULL;
    int ok = !vkCreateBuffer(dev, &bci, NULL, &out);
    if (ok) vkGetBufferMemoryRequirements(dev, out, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, ok ? req.size : 0,
                                ok ? mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : 0};
    ok = ok && mai.memoryTypeIndex != UINT32_MAX && !vkAllocateMemory(dev, &mai, NULL, &mem) && !vkBindBufferMemory(dev, out, mem, 0) &&
         !vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&o);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler smp;
    ok = ok && !vkCreateSampler(dev, &sci, NULL, &smp);

    /* Set 0: the images (plain, and a push descriptor variant); set 1: the output. */
    VkDescriptorSetLayoutBinding b[NB];
    for (int i = 0; i < NB; i++) b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, NB, b};
    VkDescriptorSetLayoutBinding ob = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo olci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &ob};
    VkDescriptorSetLayout L[2], PL[2];
    ok = ok && !vkCreateDescriptorSetLayout(dev, &lci, NULL, &L[0]) && !vkCreateDescriptorSetLayout(dev, &olci, NULL, &L[1]);
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    if (has_push) ok = ok && !vkCreateDescriptorSetLayout(dev, &lci, NULL, &PL[0]);
    PL[1] = L[1];
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_nd_comp), spv_nd_comp};
    VkShaderModule sm;
    ok = ok && !vkCreateShaderModule(dev, &smci, NULL, &sm);
    VkPipelineLayout pl, ppl = VK_NULL_HANDLE;
    VkPipeline pipe, ppipe = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, L, 0, NULL};
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm,
                                                   "main", NULL};
    ok = ok && !vkCreatePipelineLayout(dev, &plci, NULL, &pl) && (cpci.layout = pl) &&
         !vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    if (has_push) {
        plci.pSetLayouts = PL;
        ok = ok && !vkCreatePipelineLayout(dev, &plci, NULL, &ppl) && (cpci.layout = ppl) &&
             !vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &ppipe);
    }
    check(ok, "setup (layouts, compute pipeline)", NULL);
    if (!ok) return 1;

    /* Sets made after the pipeline, as games do: the image set from a pool without
     * FREE_DESCRIPTOR_SET (the client names those sets itself) and one from a pool with it. */
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 3, ps};
    VkDescriptorPool pool[2];
    VkDescriptorSet S[2], T[2];
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, 2, L};
    ok = !vkCreateDescriptorPool(dev, &dpci, NULL, &pool[0]) && (dsai.descriptorPool = pool[0]) && !vkAllocateDescriptorSets(dev, &dsai, S);
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ok = ok && !vkCreateDescriptorPool(dev, &dpci, NULL, &pool[1]) && (dsai.descriptorPool = pool[1]) && !vkAllocateDescriptorSets(dev, &dsai, T);
    check(ok, "descriptor sets", NULL);
    if (!ok) return 1;

    /* Every image null (combined image samplers keep their sampler). */
    VkDescriptorImageInfo ii[NB];
    for (int i = 0; i < NB; i++)
        ii[i] = (VkDescriptorImageInfo){types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? smp : VK_NULL_HANDLE, VK_NULL_HANDLE,
                                        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {out, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[NB + 1];
    for (int i = 0; i < NB; i++)
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, S[0], (uint32_t)i, 0, 1, types[i], &ii[i], NULL, NULL};
    w[NB] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, S[1], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
    vkUpdateDescriptorSets(dev, NB + 1, w, 0, NULL);
    w[NB].dstSet = T[1];
    vkUpdateDescriptorSets(dev, 1, &w[NB], 0, NULL);
    VkDescriptorUpdateTemplateEntry te[NB];
    for (int i = 0; i < NB; i++) te[i] = (VkDescriptorUpdateTemplateEntry){(uint32_t)i, 0, 1, types[i], i * sizeof(ii[0]), sizeof(ii[0])};
    VkDescriptorUpdateTemplateCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, NULL, 0, NB, te,
                                                VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, L[0], 0, VK_NULL_HANDLE, 0};
    VkDescriptorUpdateTemplate tmpl;
    ok = !vkCreateDescriptorUpdateTemplate(dev, &tci, NULL, &tmpl);
    check(ok, "update template", NULL);
    if (ok) vkUpdateDescriptorSetWithTemplate(dev, T[0], tmpl, ii);

    /* One dispatch per way of writing: the sets written directly, by template, then pushed. */
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkAllocateCommandBuffers(dev, &cai, &cb);
    const char *how[3] = {"vkUpdateDescriptorSets", "update template", "push descriptors"};
    for (int k = 0; k < 3; k++) {
        if (k == 1 && !ok) continue;
        if (k == 2 && !has_push) {
            check(1, "null image descriptors, push descriptors", "skipped (no VK_KHR_push_descriptor)");
            continue;
        }
        memset(o, 0xee, 128);
        vkBeginCommandBuffer(cb, &cbi);
        if (k < 2) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, k ? T : S, 0, NULL);
        } else {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ppipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ppl, 1, 1, &S[1], 0, NULL);
            vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ppl, 0, NB, w);
        }
        vkCmdDispatch(cb, 1, 1, 1);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = vkQueueWaitIdle(queue);
        if (r != VK_SUCCESS) {
            char d[64];
            snprintf(d, sizeof(d), "dispatch: %d", r);
            check(0, how[k], d);
            break;
        }
        check_sizes(o, how[k]);
        /* What they read: 0 from a null descriptor, and from a stand-in too (cleared before the
         * first submission; before that it was whatever the memory held). */
        char d[200] = "", what[96];
        int bad = 0;
        for (int i = 19; i <= 26; i++) {
            size_t l = strlen(d);
            snprintf(d + l, sizeof(d) - l, "%s%08x", l ? " " : "", o[i]);
            bad |= o[i] != 0;
        }
        snprintf(what, sizeof(what), "null images read 0, %s", how[k]);
        check(!bad, what, d);
    }

    /* A write through the null storage image (binding 6), then the reads again in the next
     * submission. A null descriptor drops the write; a stand-in takes it into a sink of its own
     * (server_nulldesc.c), so the sampled nulls, stand-ins or not, still read 0. The storage image
     * itself reads 0 when it is a real null; a stand-in's sink gives back what was written into
     * it (robustness2 says 0), which is said but not counted. */
    VkShaderModule wsm;
    VkPipeline wpipe;
    smci.codeSize = sizeof(spv_nd_write);
    smci.pCode = spv_nd_write;
    cpci.layout = pl;
    ok = !vkCreateShaderModule(dev, &smci, NULL, &wsm) && (cpci.stage.module = wsm) &&
         !vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &wpipe);
    check(ok, "pipeline writing through a null storage image", NULL);
    for (int k = 0; ok && k < 2; k++) {
        memset(o, 0xee, 128);
        vkBeginCommandBuffer(cb, &cbi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k ? pipe : wpipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, S, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = vkQueueWaitIdle(queue);
        if (r != VK_SUCCESS) {
            char d[64];
            snprintf(d, sizeof(d), "%s: %d", k ? "read" : "write", r);
            check(0, "write through a null storage image", d);
            break;
        }
    }
    if (ok) {
        char d[200] = "";
        int bad = 0;
        for (int i = 19; i <= 24; i++) {
            size_t l = strlen(d);
            snprintf(d + l, sizeof(d) - l, "%s%08x", l ? " " : "", o[i]);
            bad |= o[i] != 0;
        }
        int standin = o[14] == 1 && o[15] == 1;
        size_t l = strlen(d);
        snprintf(d + l, sizeof(d) - l, "; the storage image %s reads %08x%s", standin ? "(a stand-in)" : "(null)", o[25],
                 standin && o[25] ? ", its sink's" : "");
        bad |= !standin && o[25] != 0;
        check(!bad, "null images read 0 after a write through a null storage image", d);
    }

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
