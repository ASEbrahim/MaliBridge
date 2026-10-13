/* gpubench - headless GPU cost microbenchmark, native vs through the Vulkan bridge.
 *
 * Build (Android, runs straight on the device driver from /data/local/tmp):
 *   aarch64-linux-android26-clang -O2 -fPIE -pie -I third_party/Vulkan-Headers-1.4.341/include tests/gpubench.c -ldl -lm -o gpubench
 * Build (Linux, runs through the loader, e.g. inside the container on the bridge ICD):
 *   aarch64-linux-gnu-gcc -O2 -I third_party/Vulkan-Headers-1.4.341/include tests/gpubench.c -ldl -lm -o gpubench
 * Run: ./gpubench [filter-substring]   env: GPUBENCH_REPS (default 9), GPUBENCH_QUICK=1 (720p only)
 * Bisecting a native/bridge difference (all optional):
 *   GPUBENCH_VERBOSE=1     pipeline and attachment state used, min/median/max GPU time per test
 *   GPUBENCH_MEMTYPE=n     images in memory type n when their requirements allow it
 *   GPUBENCH_LOOP=k        every draw loop k times longer (layers x k): a long GPU job per submit
 *   GPUBENCH_ROBUST=1      device with robustBufferAccess on (the bridge forces it on Mali)
 *   GPUBENCH_EXTS=a,b|bridge  extra device extensions, if the driver has them; "bridge" = the ones
 *                          the bridge server always adds (external memory/dma-buf/DRM modifiers/
 *                          external fence+semaphore fd, AHB set where present)
 *   GPUBENCH_BRIDGELIKE=1  ROBUST + EXTS=bridge + depthClamp + shaderStorageImageExtendedFormats:
 *                          the device the bridge server creates for this program, natively
 *
 * Every test is one command buffer: reset query pool, timestamp, work, timestamp. It is submitted
 * once to warm up, then REPS times (fence wait after each); the table shows the median GPU time
 * (timestamps) and the median wall time (submit -> fence signalled). Same binary, same numbers
 * expected natively and through the bridge, unless the bridge changes what the GPU is asked to do.
 *
 * Tests:
 *   fill   N full-screen triangles into an RGBA8 optimal target, opaque / alpha-blended, for
 *          several target usage variants (minimal vs DXVK-like, which can cost Mali its AFBC)
 *   tex    full-screen pass sampling a 4096^2 mipmapped RGBA8 texture TAPS times per pixel,
 *          trilinear, ~1.5 texels per pixel, for the same usage variants on the texture
 *   depth  N full-screen layers with depth test, back-to-front (all pass) and front-to-back
 *          (early-Z rejects), D24S8 and D32
 *   comp   bandwidth-bound read+write over a 256 MB buffer and an ALU-bound FMA loop
 * Each image line reports what was asked for (tiling/usage/flags/memory type) and, when the driver
 * has VK_EXT_image_compression_control, whether it compresses the image (AFBC on Mali).
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* GLSL sources of the arrays below (glslc -O --target-env=vulkan1.1):
 * //// vs.vert
 * #version 450
 * layout(push_constant) uniform PC { vec4 color; vec2 uvscale; float z; int taps; } pc;
 * layout(location = 0) out vec2 uv;
 * void main() {
 *     vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
 *     uv = p;
 *     gl_Position = vec4(p * 2.0 - 1.0, pc.z, 1.0);
 * }
 * //// color.frag
 * #version 450
 * layout(push_constant) uniform PC { vec4 color; vec2 uvscale; float z; int taps; } pc;
 * layout(location = 0) out vec4 o;
 * void main() { o = pc.color; }
 * //// tex.frag
 * #version 450
 * layout(push_constant) uniform PC { vec4 color; vec2 uvscale; float z; int taps; } pc;
 * layout(set = 0, binding = 0) uniform sampler2D tex;
 * layout(location = 0) in vec2 uv;
 * layout(location = 0) out vec4 o;
 * void main() {
 *     vec4 acc = vec4(0.0);
 *     vec2 base = uv * pc.uvscale;
 *     for (int i = 0; i < pc.taps; i++)
 *         acc += texture(tex, base + vec2(float(i) * 0.1371, float(i) * 0.0713));
 *     o = acc / float(pc.taps);
 * }
 * //// bw.comp
 * #version 450
 * layout(local_size_x = 256) in;
 * layout(std430, set = 0, binding = 0) buffer B { vec4 d[]; } b;
 * layout(push_constant) uniform P { uint n; uint iters; } p;
 * void main() {
 *     uint stride = gl_NumWorkGroups.x * 256u;
 *     for (uint i = gl_GlobalInvocationID.x; i < p.n; i += stride)
 *         b.d[i] = b.d[i] * 1.0001 + 1.0;
 * }
 * //// alu.comp
 * #version 450
 * layout(local_size_x = 256) in;
 * layout(std430, set = 0, binding = 0) buffer B { vec4 d[]; } b;
 * layout(push_constant) uniform P { uint n; uint iters; } p;
 * void main() {
 *     vec4 x = vec4(float(gl_GlobalInvocationID.x) * 1e-7) + vec4(0.1, 0.2, 0.3, 0.4);
 *     vec4 y = x.wzyx;
 *     for (uint i = 0u; i < p.iters; i++) {
 *         x = fma(x, y, vec4(0.0001));
 *         y = fma(y, x, vec4(-0.0001));
 *         x = fma(x, vec4(0.999), y * 0.001);
 *         y = fma(y, vec4(0.999), x * 0.001);
 *     }
 *     if (x.x + y.y == 12345.678) b.d[gl_GlobalInvocationID.x & 1023u] = x + y;
 * }
 */
static const uint32_t spv_vs_vert[] = {
    0x07230203,0x00010300,0x000d000a,0x00000034,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x00000018,0x00000020,
    0x00040047,0x0000000c,0x0000000b,0x0000002a,0x00040047,0x00000018,0x0000001e,0x00000000,
    0x00050048,0x0000001e,0x00000000,0x0000000b,0x00000000,0x00050048,0x0000001e,0x00000001,
    0x0000000b,0x00000001,0x00050048,0x0000001e,0x00000002,0x0000000b,0x00000003,0x00050048,
    0x0000001e,0x00000003,0x0000000b,0x00000004,0x00030047,0x0000001e,0x00000002,0x00050048,
    0x00000028,0x00000000,0x00000023,0x00000000,0x00050048,0x00000028,0x00000001,0x00000023,
    0x00000010,0x00050048,0x00000028,0x00000002,0x00000023,0x00000018,0x00050048,0x00000028,
    0x00000003,0x00000023,0x0000001c,0x00030047,0x00000028,0x00000002,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
    0x00000006,0x00000002,0x00040015,0x0000000a,0x00000020,0x00000001,0x00040020,0x0000000b,
    0x00000001,0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000001,0x0004002b,0x0000000a,
    0x0000000e,0x00000001,0x0004002b,0x0000000a,0x00000010,0x00000002,0x00040020,0x00000017,
    0x00000003,0x00000007,0x0004003b,0x00000017,0x00000018,0x00000003,0x00040017,0x0000001a,
    0x00000006,0x00000004,0x00040015,0x0000001b,0x00000020,0x00000000,0x0004002b,0x0000001b,
    0x0000001c,0x00000001,0x0004001c,0x0000001d,0x00000006,0x0000001c,0x0006001e,0x0000001e,
    0x0000001a,0x00000006,0x0000001d,0x0000001d,0x00040020,0x0000001f,0x00000003,0x0000001e,
    0x0004003b,0x0000001f,0x00000020,0x00000003,0x0004002b,0x0000000a,0x00000021,0x00000000,
    0x0004002b,0x00000006,0x00000023,0x40000000,0x0004002b,0x00000006,0x00000025,0x3f800000,
    0x0006001e,0x00000028,0x0000001a,0x00000007,0x00000006,0x0000000a,0x00040020,0x00000029,
    0x00000009,0x00000028,0x0004003b,0x00000029,0x0000002a,0x00000009,0x00040020,0x0000002b,
    0x00000009,0x00000006,0x00040020,0x00000031,0x00000003,0x0000001a,0x0005002c,0x00000007,
    0x00000033,0x00000025,0x00000025,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
    0x000200f8,0x00000005,0x0004003d,0x0000000a,0x0000000d,0x0000000c,0x000500c4,0x0000000a,
    0x0000000f,0x0000000d,0x0000000e,0x000500c7,0x0000000a,0x00000011,0x0000000f,0x00000010,
    0x0004006f,0x00000006,0x00000012,0x00000011,0x000500c7,0x0000000a,0x00000014,0x0000000d,
    0x00000010,0x0004006f,0x00000006,0x00000015,0x00000014,0x00050050,0x00000007,0x00000016,
    0x00000012,0x00000015,0x0003003e,0x00000018,0x00000016,0x0005008e,0x00000007,0x00000024,
    0x00000016,0x00000023,0x00050083,0x00000007,0x00000027,0x00000024,0x00000033,0x00050041,
    0x0000002b,0x0000002c,0x0000002a,0x00000010,0x0004003d,0x00000006,0x0000002d,0x0000002c,
    0x00050051,0x00000006,0x0000002e,0x00000027,0x00000000,0x00050051,0x00000006,0x0000002f,
    0x00000027,0x00000001,0x00070050,0x0000001a,0x00000030,0x0000002e,0x0000002f,0x0000002d,
    0x00000025,0x00050041,0x00000031,0x00000032,0x00000020,0x00000021,0x0003003e,0x00000032,
    0x00000030,0x000100fd,0x00010038,
};
static const uint32_t spv_color_frag[] = {
    0x07230203,0x00010300,0x000d000a,0x00000013,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00030010,0x00000004,
    0x00000007,0x00040047,0x00000009,0x0000001e,0x00000000,0x00050048,0x0000000c,0x00000000,
    0x00000023,0x00000000,0x00050048,0x0000000c,0x00000001,0x00000023,0x00000010,0x00050048,
    0x0000000c,0x00000002,0x00000023,0x00000018,0x00050048,0x0000000c,0x00000003,0x00000023,
    0x0000001c,0x00030047,0x0000000c,0x00000002,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,
    0x00040020,0x00000008,0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,
    0x00040017,0x0000000a,0x00000006,0x00000002,0x00040015,0x0000000b,0x00000020,0x00000001,
    0x0006001e,0x0000000c,0x00000007,0x0000000a,0x00000006,0x0000000b,0x00040020,0x0000000d,
    0x00000009,0x0000000c,0x0004003b,0x0000000d,0x0000000e,0x00000009,0x0004002b,0x0000000b,
    0x0000000f,0x00000000,0x00040020,0x00000010,0x00000009,0x00000007,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000010,0x00000011,
    0x0000000e,0x0000000f,0x0004003d,0x00000007,0x00000012,0x00000011,0x0003003e,0x00000009,
    0x00000012,0x000100fd,0x00010038,
};
static const uint32_t spv_tex_frag[] = {
    0x07230203,0x00010300,0x000d000a,0x00000049,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000010,0x00000040,0x00030010,
    0x00000004,0x00000007,0x00040047,0x00000010,0x0000001e,0x00000000,0x00050048,0x00000013,
    0x00000000,0x00000023,0x00000000,0x00050048,0x00000013,0x00000001,0x00000023,0x00000010,
    0x00050048,0x00000013,0x00000002,0x00000023,0x00000018,0x00050048,0x00000013,0x00000003,
    0x00000023,0x0000001c,0x00030047,0x00000013,0x00000002,0x00040047,0x0000002d,0x00000022,
    0x00000000,0x00040047,0x0000002d,0x00000021,0x00000000,0x00040047,0x00000040,0x0000001e,
    0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,
    0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x0004002b,0x00000006,0x0000000a,
    0x00000000,0x0007002c,0x00000007,0x0000000b,0x0000000a,0x0000000a,0x0000000a,0x0000000a,
    0x00040017,0x0000000c,0x00000006,0x00000002,0x00040020,0x0000000f,0x00000001,0x0000000c,
    0x0004003b,0x0000000f,0x00000010,0x00000001,0x00040015,0x00000012,0x00000020,0x00000001,
    0x0006001e,0x00000013,0x00000007,0x0000000c,0x00000006,0x00000012,0x00040020,0x00000014,
    0x00000009,0x00000013,0x0004003b,0x00000014,0x00000015,0x00000009,0x0004002b,0x00000012,
    0x00000016,0x00000001,0x00040020,0x00000017,0x00000009,0x0000000c,0x0004002b,0x00000012,
    0x0000001d,0x00000000,0x0004002b,0x00000012,0x00000024,0x00000003,0x00040020,0x00000025,
    0x00000009,0x00000012,0x00020014,0x00000028,0x00090019,0x0000002a,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,0x0000002b,0x0000002a,
    0x00040020,0x0000002c,0x00000000,0x0000002b,0x0004003b,0x0000002c,0x0000002d,0x00000000,
    0x0004002b,0x00000006,0x00000032,0x3e0c63f1,0x0004002b,0x00000006,0x00000036,0x3d9205bc,
    0x00040020,0x0000003f,0x00000003,0x00000007,0x0004003b,0x0000003f,0x00000040,0x00000003,
    0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,
    0x0000000c,0x00000011,0x00000010,0x00050041,0x00000017,0x00000018,0x00000015,0x00000016,
    0x0004003d,0x0000000c,0x00000019,0x00000018,0x000200f9,0x0000001e,0x000200f8,0x0000001e,
    0x000700f5,0x00000007,0x00000048,0x0000000b,0x00000005,0x0000003c,0x0000001f,0x000700f5,
    0x00000012,0x00000047,0x0000001d,0x00000005,0x0000003e,0x0000001f,0x00050041,0x00000025,
    0x00000026,0x00000015,0x00000024,0x0004003d,0x00000012,0x00000027,0x00000026,0x000500b1,
    0x00000028,0x00000029,0x00000047,0x00000027,0x000400f6,0x00000020,0x0000001f,0x00000000,
    0x000400fa,0x00000029,0x0000001f,0x00000020,0x000200f8,0x0000001f,0x0004003d,0x0000002b,
    0x0000002e,0x0000002d,0x0004006f,0x00000006,0x00000031,0x00000047,0x00050085,0x00000006,
    0x00000033,0x00000031,0x00000032,0x00050085,0x00000006,0x00000037,0x00000031,0x00000036,
    0x00050050,0x0000000c,0x00000038,0x00000033,0x00000037,0x0008000c,0x0000000c,0x00000039,
    0x00000001,0x00000032,0x00000011,0x00000019,0x00000038,0x00050057,0x00000007,0x0000003a,
    0x0000002e,0x00000039,0x00050081,0x00000007,0x0000003c,0x00000048,0x0000003a,0x00050080,
    0x00000012,0x0000003e,0x00000047,0x00000016,0x000200f9,0x0000001e,0x000200f8,0x00000020,
    0x0004006f,0x00000006,0x00000044,0x00000027,0x00070050,0x00000007,0x00000045,0x00000044,
    0x00000044,0x00000044,0x00000044,0x00050088,0x00000007,0x00000046,0x00000048,0x00000045,
    0x0003003e,0x00000040,0x00000046,0x000100fd,0x00010038,
};
static const uint32_t spv_bw_comp[] = {
    0x07230203,0x00010300,0x000d000a,0x0000003e,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x00000013,0x00060010,
    0x00000004,0x00000011,0x00000100,0x00000001,0x00000001,0x00040047,0x0000000b,0x0000000b,
    0x00000018,0x00040047,0x00000013,0x0000000b,0x0000001c,0x00050048,0x0000001c,0x00000000,
    0x00000023,0x00000000,0x00050048,0x0000001c,0x00000001,0x00000023,0x00000004,0x00030047,
    0x0000001c,0x00000002,0x00040047,0x00000028,0x00000006,0x00000010,0x00050048,0x00000029,
    0x00000000,0x00000023,0x00000000,0x00030047,0x00000029,0x00000002,0x00040047,0x0000002b,
    0x00000022,0x00000000,0x00040047,0x0000002b,0x00000021,0x00000000,0x00040047,0x0000003b,
    0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00040015,
    0x00000006,0x00000020,0x00000000,0x00040017,0x00000009,0x00000006,0x00000003,0x00040020,
    0x0000000a,0x00000001,0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x0004002b,
    0x00000006,0x0000000c,0x00000000,0x00040020,0x0000000d,0x00000001,0x00000006,0x0004002b,
    0x00000006,0x00000010,0x00000100,0x0004003b,0x0000000a,0x00000013,0x00000001,0x0004001e,
    0x0000001c,0x00000006,0x00000006,0x00040020,0x0000001d,0x00000009,0x0000001c,0x0004003b,
    0x0000001d,0x0000001e,0x00000009,0x00040015,0x0000001f,0x00000020,0x00000001,0x0004002b,
    0x0000001f,0x00000020,0x00000000,0x00040020,0x00000021,0x00000009,0x00000006,0x00020014,
    0x00000024,0x00030016,0x00000026,0x00000020,0x00040017,0x00000027,0x00000026,0x00000004,
    0x0003001d,0x00000028,0x00000027,0x0003001e,0x00000029,0x00000028,0x00040020,0x0000002a,
    0x0000000c,0x00000029,0x0004003b,0x0000002a,0x0000002b,0x0000000c,0x00040020,0x0000002e,
    0x0000000c,0x00000027,0x0004002b,0x00000026,0x00000031,0x3f800347,0x0004002b,0x00000026,
    0x00000033,0x3f800000,0x0004002b,0x00000006,0x0000003a,0x00000001,0x0006002c,0x00000009,
    0x0000003b,0x00000010,0x0000003a,0x0000003a,0x0007002c,0x00000027,0x0000003d,0x00000033,
    0x00000033,0x00000033,0x00000033,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
    0x000200f8,0x00000005,0x00050041,0x0000000d,0x0000000e,0x0000000b,0x0000000c,0x0004003d,
    0x00000006,0x0000000f,0x0000000e,0x00050084,0x00000006,0x00000011,0x0000000f,0x00000010,
    0x00050041,0x0000000d,0x00000014,0x00000013,0x0000000c,0x0004003d,0x00000006,0x00000015,
    0x00000014,0x000200f9,0x00000016,0x000200f8,0x00000016,0x000700f5,0x00000006,0x0000003c,
    0x00000015,0x00000005,0x00000039,0x00000017,0x00050041,0x00000021,0x00000022,0x0000001e,
    0x00000020,0x0004003d,0x00000006,0x00000023,0x00000022,0x000500b0,0x00000024,0x00000025,
    0x0000003c,0x00000023,0x000400f6,0x00000018,0x00000017,0x00000000,0x000400fa,0x00000025,
    0x00000017,0x00000018,0x000200f8,0x00000017,0x00060041,0x0000002e,0x0000002f,0x0000002b,
    0x00000020,0x0000003c,0x0004003d,0x00000027,0x00000030,0x0000002f,0x0005008e,0x00000027,
    0x00000032,0x00000030,0x00000031,0x00050081,0x00000027,0x00000035,0x00000032,0x0000003d,
    0x0003003e,0x0000002f,0x00000035,0x00050080,0x00000006,0x00000039,0x0000003c,0x00000011,
    0x000200f9,0x00000016,0x000200f8,0x00000018,0x000100fd,0x00010038,
};
static const uint32_t spv_alu_comp[] = {
    0x07230203,0x00010300,0x000d000a,0x00000068,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00060010,0x00000004,
    0x00000011,0x00000100,0x00000001,0x00000001,0x00040047,0x0000000d,0x0000000b,0x0000001c,
    0x00050048,0x00000027,0x00000000,0x00000023,0x00000000,0x00050048,0x00000027,0x00000001,
    0x00000023,0x00000004,0x00030047,0x00000027,0x00000002,0x00040047,0x00000053,0x00000006,
    0x00000010,0x00050048,0x00000054,0x00000000,0x00000023,0x00000000,0x00030047,0x00000054,
    0x00000002,0x00040047,0x00000056,0x00000022,0x00000000,0x00040047,0x00000056,0x00000021,
    0x00000000,0x00040047,0x00000062,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,
    0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,
    0x00000004,0x00040015,0x0000000a,0x00000020,0x00000000,0x00040017,0x0000000b,0x0000000a,
    0x00000003,0x00040020,0x0000000c,0x00000001,0x0000000b,0x0004003b,0x0000000c,0x0000000d,
    0x00000001,0x0004002b,0x0000000a,0x0000000e,0x00000000,0x00040020,0x0000000f,0x00000001,
    0x0000000a,0x0004002b,0x00000006,0x00000013,0x33d6bf95,0x0004002b,0x00000006,0x00000016,
    0x3dcccccd,0x0004002b,0x00000006,0x00000017,0x3e4ccccd,0x0004002b,0x00000006,0x00000018,
    0x3e99999a,0x0004002b,0x00000006,0x00000019,0x3ecccccd,0x0007002c,0x00000007,0x0000001a,
    0x00000016,0x00000017,0x00000018,0x00000019,0x0004001e,0x00000027,0x0000000a,0x0000000a,
    0x00040020,0x00000028,0x00000009,0x00000027,0x0004003b,0x00000028,0x00000029,0x00000009,
    0x00040015,0x0000002a,0x00000020,0x00000001,0x0004002b,0x0000002a,0x0000002b,0x00000001,
    0x00040020,0x0000002c,0x00000009,0x0000000a,0x00020014,0x0000002f,0x0004002b,0x00000006,
    0x00000033,0x38d1b717,0x0007002c,0x00000007,0x00000034,0x00000033,0x00000033,0x00000033,
    0x00000033,0x0004002b,0x00000006,0x00000038,0xb8d1b717,0x0007002c,0x00000007,0x00000039,
    0x00000038,0x00000038,0x00000038,0x00000038,0x0004002b,0x00000006,0x0000003c,0x3f7fbe77,
    0x0007002c,0x00000007,0x0000003d,0x0000003c,0x0000003c,0x0000003c,0x0000003c,0x0004002b,
    0x00000006,0x0000003f,0x3a83126f,0x0004002b,0x0000000a,0x0000004b,0x00000001,0x0004002b,
    0x00000006,0x0000004f,0x4640e6b6,0x0003001d,0x00000053,0x00000007,0x0003001e,0x00000054,
    0x00000053,0x00040020,0x00000055,0x0000000c,0x00000054,0x0004003b,0x00000055,0x00000056,
    0x0000000c,0x0004002b,0x0000002a,0x00000057,0x00000000,0x0004002b,0x0000000a,0x0000005a,
    0x000003ff,0x00040020,0x0000005f,0x0000000c,0x00000007,0x0004002b,0x0000000a,0x00000061,
    0x00000100,0x0006002c,0x0000000b,0x00000062,0x00000061,0x0000004b,0x0000004b,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x0000000f,
    0x00000010,0x0000000d,0x0000000e,0x0004003d,0x0000000a,0x00000011,0x00000010,0x00040070,
    0x00000006,0x00000012,0x00000011,0x00050085,0x00000006,0x00000014,0x00000012,0x00000013,
    0x00070050,0x00000007,0x00000015,0x00000014,0x00000014,0x00000014,0x00000014,0x00050081,
    0x00000007,0x0000001b,0x00000015,0x0000001a,0x0009004f,0x00000007,0x0000001e,0x0000001b,
    0x0000001b,0x00000003,0x00000002,0x00000001,0x00000000,0x000200f9,0x00000021,0x000200f8,
    0x00000021,0x000700f5,0x00000007,0x00000067,0x0000001e,0x00000005,0x00000045,0x00000022,
    0x000700f5,0x00000007,0x00000066,0x0000001b,0x00000005,0x00000041,0x00000022,0x000700f5,
    0x0000000a,0x00000065,0x0000000e,0x00000005,0x00000047,0x00000022,0x00050041,0x0000002c,
    0x0000002d,0x00000029,0x0000002b,0x0004003d,0x0000000a,0x0000002e,0x0000002d,0x000500b0,
    0x0000002f,0x00000030,0x00000065,0x0000002e,0x000400f6,0x00000023,0x00000022,0x00000000,
    0x000400fa,0x00000030,0x00000022,0x00000023,0x000200f8,0x00000022,0x0008000c,0x00000007,
    0x00000035,0x00000001,0x00000032,0x00000066,0x00000067,0x00000034,0x0008000c,0x00000007,
    0x0000003a,0x00000001,0x00000032,0x00000067,0x00000035,0x00000039,0x0005008e,0x00000007,
    0x00000040,0x0000003a,0x0000003f,0x0008000c,0x00000007,0x00000041,0x00000001,0x00000032,
    0x00000035,0x0000003d,0x00000040,0x0005008e,0x00000007,0x00000044,0x00000041,0x0000003f,
    0x0008000c,0x00000007,0x00000045,0x00000001,0x00000032,0x0000003a,0x0000003d,0x00000044,
    0x00050080,0x0000000a,0x00000047,0x00000065,0x0000002b,0x000200f9,0x00000021,0x000200f8,
    0x00000023,0x00050051,0x00000006,0x0000004a,0x00000066,0x00000000,0x00050051,0x00000006,
    0x0000004d,0x00000067,0x00000001,0x00050081,0x00000006,0x0000004e,0x0000004a,0x0000004d,
    0x000500b4,0x0000002f,0x00000050,0x0000004e,0x0000004f,0x000300f7,0x00000052,0x00000000,
    0x000400fa,0x00000050,0x00000051,0x00000052,0x000200f8,0x00000051,0x000500c7,0x0000000a,
    0x0000005b,0x00000011,0x0000005a,0x00050081,0x00000007,0x0000005e,0x00000066,0x00000067,
    0x00060041,0x0000005f,0x00000060,0x00000056,0x00000057,0x0000005b,0x0003003e,0x00000060,
    0x0000005e,0x000200f9,0x00000052,0x000200f8,0x00000052,0x000100fd,0x00010038,
};

#define FNS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DFNS(X) \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDispatch) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdFillBuffer) \
    X(vkQueueWaitIdle) X(vkResetCommandPool) X(vkDeviceWaitIdle)
#define DECL(n) static PFN_##n n;
FNS(DECL)
DFNS(DECL)
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCmdBeginRendering pBeginRendering;
static PFN_vkCmdEndRendering pEndRendering;
static PFN_vkGetImageSubresourceLayout2EXT pGetSubLayout2;

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkFence fence;
static VkQueryPool qpool;
static VkPhysicalDeviceMemoryProperties memprops;
static double ts_period;
static uint32_t ts_bits;
static int reps = 9;
static int have_compression_query;
static const char *filter;
static int verbose;
static int force_memtype = -1;
static int loop = 1;
static int layers; /* LAYERS * loop */

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags *got)
{
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (pass || (memprops.memoryTypes[i].propertyFlags & want) == want)) {
                if (got) *got = memprops.memoryTypes[i].propertyFlags;
                return i;
            }
    fprintf(stderr, "no memory type for bits %x\n", bits);
    exit(1);
}

/* ---------------------------------------------------------------- printing helpers */
static const char *usage_str(VkImageUsageFlags u)
{
    static char b[160];
    b[0] = 0;
    static const struct { VkImageUsageFlags f; const char *n; } t[] = {
        {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "TSRC"}, {VK_IMAGE_USAGE_TRANSFER_DST_BIT, "TDST"},
        {VK_IMAGE_USAGE_SAMPLED_BIT, "SAMPLED"}, {VK_IMAGE_USAGE_STORAGE_BIT, "STORAGE"},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "COLOR"}, {VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "DS"},
        {VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, "INPUT"}};
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++)
        if (u & t[i].f) { if (b[0]) strcat(b, "|"); strcat(b, t[i].n); }
    return b;
}

static const char *flags_str(VkImageCreateFlags f, int nlist)
{
    static char b[96];
    b[0] = 0;
    if (f & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) strcat(b, "MUTABLE");
    if (f & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) strcat(b, b[0] ? "|EXTUSAGE" : "EXTUSAGE");
    if (nlist) sprintf(b + strlen(b), "%sfmtlist[%d]", b[0] ? "+" : "", nlist);
    if (!b[0]) strcpy(b, "-");
    return b;
}

static const char *mem_str(VkMemoryPropertyFlags f)
{
    static char b[64];
    snprintf(b, sizeof b, "%s%s%s%s%s%s", f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DL" : "",
             f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "|HV" : "", f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "|HC" : "",
             f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "|CACHED" : "", f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? "|LAZY" : "",
             f & VK_MEMORY_PROPERTY_PROTECTED_BIT ? "|PROT" : "");
    return b;
}

/* ---------------------------------------------------------------- images */
typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat fmt;
    uint32_t w, h, mips;
    VkImageAspectFlags aspect;
} image;

/* Usage variants applied to color targets and textures. */
enum { V_PLAIN, V_DXVK, V_DXVK_LIST, V_DXVK_NOSTORAGE, V_COUNT };
static const char *vname[V_COUNT] = {"plain", "dxvk", "dxvk+list", "dxvk-nostor"};

static void variant_info(int v, int is_rt, VkImageUsageFlags *usage, VkImageCreateFlags *flags, int *list)
{
    VkImageUsageFlags dx = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    *flags = 0;
    *list = 0;
    switch (v) {
    case V_PLAIN:
        *usage = is_rt ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                       : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        break;
    case V_DXVK: *usage = dx; *flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; break;
    case V_DXVK_LIST: *usage = dx; *flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; *list = 2; break;
    case V_DXVK_NOSTORAGE:
        *usage = dx & ~VK_IMAGE_USAGE_STORAGE_BIT;
        *flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        *list = 2;
        break;
    }
}

static int image_supported(VkFormat fmt, VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL, fmt,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags};
    VkImageFormatProperties2 p = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    return vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &p) == VK_SUCCESS;
}

static const char *compression_str(VkImage img, VkImageAspectFlags aspect)
{
    if (!have_compression_query) return "n/a";
    VkImageCompressionPropertiesEXT cp = {VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT};
    cp.imageCompressionFlags = 0x7fffffff; /* sentinel: DEFAULT is 0, so detect a layer that drops the pNext chain */
    VkSubresourceLayout2EXT sl = {VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_EXT, &cp};
    VkImageSubresource2EXT sr = {VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_EXT, NULL, {aspect, 0, 0}};
    pGetSubLayout2(dev, img, &sr, &sl);
    if (cp.imageCompressionFlags == 0x7fffffff) return "unanswered(pNext dropped)";
    if (cp.imageCompressionFlags & VK_IMAGE_COMPRESSION_DISABLED_EXT) return "DISABLED";
    if (cp.imageCompressionFlags & VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT) return "fixed-rate";
    if (cp.imageCompressionFlags & VK_IMAGE_COMPRESSION_FIXED_RATE_DEFAULT_EXT) return "fixed-rate";
    return "default(lossless)";
}

static int make_image(image *im, const char *label, VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips,
                      VkImageUsageFlags usage, VkImageCreateFlags flags, int nlist, VkImageAspectFlags aspect)
{
    VkFormat list[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, (uint32_t)nlist, list};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = nlist ? &fl : NULL;
    ci.flags = flags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!image_supported(fmt, usage, flags)) {
        printf("  [image] %-28s fmt %d %ux%u usage %s flags %s: UNSUPPORTED, skipped\n", label, fmt, w, h,
               usage_str(usage), flags_str(flags, nlist));
        return 0;
    }
    memset(im, 0, sizeof *im);
    im->fmt = fmt; im->w = w; im->h = h; im->mips = mips; im->aspect = aspect;
    CK(vkCreateImage(dev, &ci, NULL, &im->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryPropertyFlags got = 0;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &got)};
    if (force_memtype >= 0) {
        if ((uint32_t)force_memtype < memprops.memoryTypeCount && (mr.memoryTypeBits & (1u << force_memtype))) {
            ai.memoryTypeIndex = (uint32_t)force_memtype;
            got = memprops.memoryTypes[force_memtype].propertyFlags;
        } else {
            printf("  (GPUBENCH_MEMTYPE=%d not allowed for %s: memoryTypeBits 0x%x; default kept)\n", force_memtype, label, mr.memoryTypeBits);
        }
    }
    if (verbose)
        printf("  [mem] %s: memoryTypeBits 0x%x, chose type %u (%s, heap %u), size %llu align %llu\n", label, mr.memoryTypeBits,
               ai.memoryTypeIndex, mem_str(got), memprops.memoryTypes[ai.memoryTypeIndex].heapIndex, (unsigned long long)mr.size,
               (unsigned long long)mr.alignment);
    CK(vkAllocateMemory(dev, &ai, NULL, &im->mem));
    CK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->img, VK_IMAGE_VIEW_TYPE_2D, fmt};
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, mips, 0, 1};
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                     usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)};
    vi.pNext = &vu;
    CK(vkCreateImageView(dev, &vi, NULL, &im->view));
    printf("  [image] %-28s fmt %3d %4ux%-4u mips %2u OPTIMAL usage %-36s flags %-26s mem %-18s %6.1f MB align %llu type %u compression %s\n",
           label, fmt, w, h, mips, usage_str(usage), flags_str(flags, nlist), mem_str(got), mr.size / 1048576.0,
           (unsigned long long)mr.alignment, ai.memoryTypeIndex, compression_str(im->img, aspect));
    return 1;
}

static void free_image(image *im)
{
    vkDestroyImageView(dev, im->view, NULL);
    vkDestroyImage(dev, im->img, NULL);
    vkFreeMemory(dev, im->mem, NULL);
}

static void barrier(VkImage img, VkImageAspectFlags aspect, uint32_t mips, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {aspect, 0, mips, 0, 1}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------- command buffer + timing */
static void begin(void)
{
    CK(vkResetCommandPool(dev, pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdResetQueryPool(cb, qpool, 0, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
}

static void submit_wait(void)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CK(vkResetFences(dev, 1, &fence));
    CK(vkQueueSubmit(queue, 1, &si, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
}

typedef struct { double gpu, wall; } result;

/* Ends the recorded command buffer and runs it 1 + reps times; medians. */
static result run(void)
{
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
    CK(vkEndCommandBuffer(cb));
    double g[64], w[64];
    int n = reps > 64 ? 64 : reps;
    submit_wait();
    for (int i = 0; i < n; i++) {
        double t0 = now_ms();
        submit_wait();
        w[i] = now_ms() - t0;
        uint64_t ts[2] = {0, 0};
        VkResult r = vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        uint64_t mask = ts_bits >= 64 ? ~0ull : ((1ull << ts_bits) - 1);
        g[i] = (r == VK_SUCCESS && ts_bits) ? (double)((ts[1] - ts[0]) & mask) * ts_period / 1e6 : -1;
    }
    qsort(g, n, sizeof *g, cmpd);
    qsort(w, n, sizeof *w, cmpd);
    if (verbose && n > 1)
        printf("    (gpu min %.3f median %.3f max %.3f ms; wall min %.3f max %.3f ms over %d reps)\n", g[0], g[n / 2], g[n - 1], w[0],
               w[n - 1], n);
    return (result){g[n / 2], w[n / 2]};
}

static void report(const char *name, result r, double work, const char *unit)
{
    double t = r.gpu > 0 ? r.gpu : r.wall;
    printf("%-46s gpu %8.3f ms  wall %8.3f ms", name, r.gpu, r.wall);
    if (work > 0 && t > 0) printf("  %8.2f %s", work / (t * 1e-3) / 1e9, unit);
    printf("\n");
    fflush(stdout);
}

static int want(const char *name) { return !filter || strstr(name, filter); }

/* ---------------------------------------------------------------- pipelines */
typedef struct { float color[4]; float uvscale[2]; float z; int taps; } pcs;

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m;
    CK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

static VkPipelineLayout gfx_layout, tex_layout, comp_layout;
static VkDescriptorSetLayout tex_dsl, buf_dsl;
static VkShaderModule m_vs, m_color, m_tex, m_bw, m_alu;

static VkPipeline gfx_pipeline(VkShaderModule fs, VkPipelineLayout layout, int blend, VkFormat depth_fmt)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, m_vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = depth_fmt != VK_FORMAT_UNDEFINED;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = 0xf;
    if (blend) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cf, depth_fmt,
                                        depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? depth_fmt : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy;
    ci.layout = layout;
    VkPipeline p;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    if (verbose)
        printf("  [pipeline] fs %s: dynamic rendering color fmt %d depth fmt %d stencil fmt %d, no cache, flags 0; blend %s "
               "(color %d*src + %d*dst op %d, alpha %d*src + %d*dst op %d, writemask 0x%x); depth test %d write %d op %d; "
               "cull none, fill, 1 sample, dynamic viewport+scissor\n",
               fs == m_color ? "color" : fs == m_tex ? "tex" : "?", cf, depth_fmt, ri.stencilAttachmentFormat, blend ? "ON" : "off",
               ba.srcColorBlendFactor, ba.dstColorBlendFactor, ba.colorBlendOp, ba.srcAlphaBlendFactor, ba.dstAlphaBlendFactor,
               ba.alphaBlendOp, ba.colorWriteMask, ds.depthTestEnable, ds.depthWriteEnable, ds.depthCompareOp);
    return p;
}

static VkPipeline comp_pipeline(VkShaderModule m)
{
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
    ci.layout = comp_layout;
    VkPipeline p;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    return p;
}

static int rendering_told;

/* Begins dynamic rendering into rt (+ optional depth), clear, store. */
static void begin_rendering(image *rt, image *depth)
{
    barrier(rt->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (depth)
        barrier(depth->img, depth->aspect, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = rt->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = (VkClearColorValue){{0.1f, 0.2f, 0.3f, 1}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
        da.imageView = depth->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
        sa = da;
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = (VkExtent2D){rt->w, rt->h};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (depth) {
        ri.pDepthAttachment = &da;
        if (depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &sa;
    }
    if (verbose > 1 || (verbose && !rendering_told++))
        printf("  [rendering] %ux%u color loadOp %d storeOp %d layout %d%s, barrier UNDEFINED->COLOR_ATTACHMENT_OPTIMAL "
               "(TOP_OF_PIPE -> COLOR_ATTACHMENT_OUTPUT)\n",
               rt->w, rt->h, ca.loadOp, ca.storeOp, ca.imageLayout, depth ? ", depth/stencil loadOp CLEAR storeOp STORE" : "");
    pBeginRendering(cb, &ri);
    VkViewport v = {0, 0, (float)rt->w, (float)rt->h, 0, 1};
    VkRect2D s = {{0, 0}, {rt->w, rt->h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
}

/* ---------------------------------------------------------------- setup */
static void load_vulkan(void)
{
#ifdef __ANDROID__
    void *lib = dlopen("libvulkan.so", RTLD_NOW);
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
#endif
    if (!lib) { fprintf(stderr, "dlopen libvulkan: %s\n", dlerror()); exit(1); }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gpubench", 1, "gpubench", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    CK(vkCreateInstance(&ici, NULL, &inst));
#define LI(n) n = (PFN_##n)gipa(inst, #n);
    FNS(LI)
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    if (!n) { fprintf(stderr, "no physical devices\n"); exit(1); }
    const char *pick = getenv("GPUBENCH_DEVICE");
    pd = pds[pick ? atoi(pick) % n : 0];
}

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static void create_device(void)
{
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    VkPhysicalDeviceProperties *p = &p2.properties;
    printf("device: %s  api %u.%u.%u  driverVersion 0x%x  vendor 0x%x device 0x%x\n", p->deviceName,
           VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion),
           p->driverVersion, p->vendorID, p->deviceID);
    printf("driver: %s / %s (id %d)\n", drv.driverName, drv.driverInfo, drv.driverID);
    ts_period = p->limits.timestampPeriod;
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        printf("  memtype %u heap %u %s\n", i, memprops.memoryTypes[i].heapIndex, mem_str(memprops.memoryTypes[i].propertyFlags));

    uint32_t nq = 16;
    VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (qfam = 0; qfam < nq; qfam++)
        if ((qf[qfam].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) break;
    if (qfam == nq) { fprintf(stderr, "no graphics+compute queue\n"); exit(1); }
    ts_bits = qf[qfam].timestampValidBits;
    printf("queue family %u, timestampValidBits %u, timestampPeriod %.3f ns%s\n", qfam, ts_bits, ts_period,
           ts_bits ? "" : "  (no timestamps: GPU column is -1, use wall)");

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne, sizeof *ext);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);

    VkPhysicalDeviceImageCompressionControlFeaturesEXT icc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    int want_icc = has_ext(ext, ne, VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME);
    if (want_icc) f13.pNext = &icc;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering) { fprintf(stderr, "dynamicRendering not supported\n"); exit(1); }
    have_compression_query = want_icc && icc.imageCompressionControl;
    printf("VK_EXT_image_compression_control: %s\n", have_compression_query ? "yes (compression column is the driver's answer)" : "no");

    /* Only what the benchmark needs: no robustness, no extra features (unless asked, see the top). */
    VkPhysicalDeviceImageCompressionControlFeaturesEXT icc_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT,
                                                                 NULL, VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13_on.dynamicRendering = VK_TRUE;
    if (have_compression_query) f13_on.pNext = &icc_on;
    VkPhysicalDeviceFeatures2 f2_on = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13_on};
    int bridgelike = getenv("GPUBENCH_BRIDGELIKE") && atoi(getenv("GPUBENCH_BRIDGELIKE"));
    if (bridgelike || (getenv("GPUBENCH_ROBUST") && atoi(getenv("GPUBENCH_ROBUST")))) {
        if (f2.features.robustBufferAccess) f2_on.features.robustBufferAccess = VK_TRUE;
        else printf("robustBufferAccess: not supported, left off\n");
    }
    if (bridgelike) {
        f2_on.features.depthClamp = f2.features.depthClamp;
        f2_on.features.shaderStorageImageExtendedFormats = f2.features.shaderStorageImageExtendedFormats;
    }
    /* What the bridge server adds to every device (server_instance.c wsi_exts, server_memory.c strategy lists). */
    static const char *const bridge_exts[] = {
        "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_image_format_list",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd",
#ifdef __ANDROID__
        "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_KHR_sampler_ycbcr_conversion",
        "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
#endif
    };
    const char *exts[64];
    uint32_t nexts = 0;
    if (have_compression_query) exts[nexts++] = VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME;
    const char *extra = getenv("GPUBENCH_EXTS");
    if (bridgelike && !extra) extra = "bridge";
    static char extbuf[2048];
    if (extra && !strcmp(extra, "bridge")) {
        for (unsigned i = 0; i < sizeof bridge_exts / sizeof *bridge_exts && nexts < 64; i++)
            if (has_ext(ext, ne, bridge_exts[i])) exts[nexts++] = bridge_exts[i];
    } else if (extra) {
        snprintf(extbuf, sizeof extbuf, "%s", extra);
        for (char *t = strtok(extbuf, ","); t && nexts < 64; t = strtok(NULL, ","))
            if (has_ext(ext, ne, t)) exts[nexts++] = t;
            else printf("extension %s: not supported, skipped\n", t);
    }
    printf("device: robustBufferAccess %u depthClamp %u storageImageExtendedFormats %u; %u extension(s):", f2_on.features.robustBufferAccess,
           f2_on.features.depthClamp, f2_on.features.shaderStorageImageExtendedFormats, nexts);
    for (uint32_t i = 0; i < nexts; i++) printf(" %s", exts[i]);
    printf("\n");
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2_on, 0, 1, &qci};
    dci.enabledExtensionCount = nexts;
    dci.ppEnabledExtensionNames = exts;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    free(ext);
#define LD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n); if (!n) { fprintf(stderr, "missing %s\n", #n); exit(1); }
    DFNS(LD)
    pBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering");
    pEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering");
    if (!pBeginRendering) {
        pBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR");
        pEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR");
    }
    if (have_compression_query) {
        pGetSubLayout2 = (PFN_vkGetImageSubresourceLayout2EXT)vkGetDeviceProcAddr(dev, "vkGetImageSubresourceLayout2EXT");
        if (!pGetSubLayout2) have_compression_query = 0;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CK(vkCreateFence(dev, &fci, NULL, &fence));
    VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2};
    CK(vkCreateQueryPool(dev, &qpci, NULL, &qpool));
}

static void create_layouts(void)
{
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcs)};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pr};
    CK(vkCreatePipelineLayout(dev, &lci, NULL, &gfx_layout));
    VkDescriptorSetLayoutBinding tb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &tb};
    CK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &tex_dsl));
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &tex_dsl;
    CK(vkCreatePipelineLayout(dev, &lci, NULL, &tex_layout));
    VkDescriptorSetLayoutBinding bb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    dci.pBindings = &bb;
    CK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &buf_dsl));
    VkPushConstantRange cpr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    lci.pSetLayouts = &buf_dsl;
    lci.pPushConstantRanges = &cpr;
    CK(vkCreatePipelineLayout(dev, &lci, NULL, &comp_layout));
    m_vs = shader(spv_vs_vert, sizeof spv_vs_vert);
    m_color = shader(spv_color_frag, sizeof spv_color_frag);
    m_tex = shader(spv_tex_frag, sizeof spv_tex_frag);
    m_bw = shader(spv_bw_comp, sizeof spv_bw_comp);
    m_alu = shader(spv_alu_comp, sizeof spv_alu_comp);
}

static VkDescriptorPool dpool;
static VkDescriptorSet alloc_set(VkDescriptorSetLayout l)
{
    if (!dpool) {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
        VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 20, 2, ps};
        CK(vkCreateDescriptorPool(dev, &ci, NULL, &dpool));
    }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &l};
    VkDescriptorSet s;
    CK(vkAllocateDescriptorSets(dev, &ai, &s));
    return s;
}

/* ---------------------------------------------------------------- tests */
static const struct { uint32_t w, h; } res[2] = {{1280, 720}, {2560, 1440}};
static int nres = 2;
#define LAYERS 32
#define TAPS 16
#define TEX 4096

static void draw_layers(VkPipelineLayout layout, int n, float alpha, int depth_mode)
{
    for (int i = 0; i < n; i++) {
        pcs pc = {{(i & 1) ? 0.8f : 0.2f, (i % 3) * 0.3f, 0.5f, alpha}, {0, 0}, 0.5f, 0};
        if (depth_mode == 1) pc.z = 0.99f - 0.9f * i / n; /* back to front: every layer passes */
        if (depth_mode == 2) pc.z = 0.05f + 0.9f * i / n; /* front to back: early-Z rejects all but the first */
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
}

static void test_fill(void)
{
    VkPipeline opaque = gfx_pipeline(m_color, gfx_layout, 0, VK_FORMAT_UNDEFINED);
    VkPipeline blend = gfx_pipeline(m_color, gfx_layout, 1, VK_FORMAT_UNDEFINED);
    for (int r = 0; r < nres; r++)
        for (int v = 0; v < V_COUNT; v++) {
            char name[96], label[64];
            snprintf(name, sizeof name, "fill %ux%u rt=%s", res[r].w, res[r].h, vname[v]);
            if (!want(name)) continue;
            VkImageUsageFlags u; VkImageCreateFlags f; int nl;
            variant_info(v, 1, &u, &f, &nl);
            snprintf(label, sizeof label, "rt %ux%u %s", res[r].w, res[r].h, vname[v]);
            image rt;
            if (!make_image(&rt, label, VK_FORMAT_R8G8B8A8_UNORM, res[r].w, res[r].h, 1, u, f, nl, VK_IMAGE_ASPECT_COLOR_BIT)) continue;
            double px = (double)res[r].w * res[r].h * layers;
            for (int b = 0; b < 2; b++) {
                begin();
                begin_rendering(&rt, NULL);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b ? blend : opaque);
                draw_layers(gfx_layout, layers, 0.5f, 0);
                pEndRendering(cb);
                char n2[128];
                snprintf(n2, sizeof n2, "%s %s x%d", name, b ? "blend " : "opaque", layers);
                report(n2, run(), px, "Gpix/s");
            }
            CK(vkQueueWaitIdle(queue));
            free_image(&rt);
        }
}

static void upload_texture(image *tex)
{
    /* Image-like content (smooth gradients + a little noise), all mips uploaded from one staging buffer,
       so the texture never needs TRANSFER_SRC for blits. */
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < tex->mips; m++) total += (VkDeviceSize)(TEX >> m) * (TEX >> m) * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    static VkBuffer sb;
    static VkDeviceMemory sm;
    static uint8_t *map;
    if (!sb) {
        CK(vkCreateBuffer(dev, &bci, NULL, &sb));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, sb, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                   memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL)};
        CK(vkAllocateMemory(dev, &ai, NULL, &sm));
        CK(vkBindBufferMemory(dev, sb, sm, 0));
        CK(vkMapMemory(dev, sm, 0, VK_WHOLE_SIZE, 0, (void **)&map));
        uint32_t seed = 12345;
        uint8_t *p = map;
        for (uint32_t m = 0; m < tex->mips; m++) {
            uint32_t s = TEX >> m;
            for (uint32_t y = 0; y < s; y++)
                for (uint32_t x = 0; x < s; x++) {
                    seed = seed * 1664525u + 1013904223u;
                    float fx = (float)x / s, fy = (float)y / s;
                    *p++ = (uint8_t)(127 + 100 * sinf(fx * 13 + fy * 7) + ((seed >> 24) & 15));
                    *p++ = (uint8_t)(127 + 100 * sinf(fy * 17 - fx * 5) + ((seed >> 16) & 15));
                    *p++ = (uint8_t)(127 + 100 * cosf((fx + fy) * 11) + ((seed >> 8) & 15));
                    *p++ = 255;
                }
        }
    }
    begin();
    barrier(tex->img, VK_IMAGE_ASPECT_COLOR_BIT, tex->mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < tex->mips; m++) {
        uint32_t s = TEX >> m;
        VkBufferImageCopy c = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {s, s, 1}};
        vkCmdCopyBufferToImage(cb, sb, tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        off += (VkDeviceSize)s * s * 4;
    }
    barrier(tex->img, VK_IMAGE_ASPECT_COLOR_BIT, tex->mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    int saved = reps;
    reps = 1;
    run();
    reps = saved;
}

static void test_tex(void)
{
    VkPipeline p = gfx_pipeline(m_tex, tex_layout, 0, VK_FORMAT_UNDEFINED);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler smp;
    CK(vkCreateSampler(dev, &sci, NULL, &smp));
    uint32_t mips = 13; /* 4096 -> 1 */
    for (int v = 0; v < V_COUNT; v++) {
        char any[64];
        snprintf(any, sizeof any, "tex tex=%s", vname[v]);
        int wanted = 0;
        for (int r = 0; r < nres; r++) {
            char name[96];
            snprintf(name, sizeof name, "tex %ux%u tex=%s", res[r].w, res[r].h, vname[v]);
            wanted |= want(name);
        }
        if (!wanted) continue;
        VkImageUsageFlags u; VkImageCreateFlags f; int nl;
        variant_info(v, 0, &u, &f, &nl);
        char label[64];
        snprintf(label, sizeof label, "tex %ux%u %s", TEX, TEX, vname[v]);
        image tex;
        if (!make_image(&tex, label, VK_FORMAT_R8G8B8A8_UNORM, TEX, TEX, mips, u, f, nl, VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        upload_texture(&tex);
        VkDescriptorSet set = alloc_set(tex_dsl);
        VkDescriptorImageInfo dii = {smp, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        for (int r = 0; r < nres; r++) {
            char name[96], l2[64];
            snprintf(name, sizeof name, "tex %ux%u tex=%s taps %d", res[r].w, res[r].h, vname[v], TAPS);
            if (!want(name)) continue;
            snprintf(l2, sizeof l2, "rt %ux%u plain (tex)", res[r].w, res[r].h);
            image rt;
            int vp; VkImageUsageFlags ru; VkImageCreateFlags rf;
            variant_info(V_PLAIN, 1, &ru, &rf, &vp);
            if (!make_image(&rt, l2, VK_FORMAT_R8G8B8A8_UNORM, res[r].w, res[r].h, 1, ru, rf, vp, VK_IMAGE_ASPECT_COLOR_BIT)) continue;
            begin();
            begin_rendering(&rt, NULL);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tex_layout, 0, 1, &set, 0, NULL);
            /* ~1.5 texels per pixel: trilinear between mip 0 and 1 */
            pcs pc = {{0, 0, 0, 0}, {1.5f * res[r].w / TEX, 1.5f * res[r].h / TEX}, 0.5f, TAPS};
            vkCmdPushConstants(cb, tex_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            for (int k = 0; k < loop; k++) vkCmdDraw(cb, 3, 1, 0, 0);
            pEndRendering(cb);
            report(name, run(), (double)res[r].w * res[r].h * TAPS * loop, "Gtap/s");
            CK(vkQueueWaitIdle(queue));
            free_image(&rt);
        }
        CK(vkQueueWaitIdle(queue));
        free_image(&tex);
    }
}

static void test_depth(void)
{
    static const struct { VkFormat f; const char *n; VkImageAspectFlags a; } fmts[2] = {
        {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8", VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
        {VK_FORMAT_D32_SFLOAT, "D32", VK_IMAGE_ASPECT_DEPTH_BIT}};
    for (int fi = 0; fi < 2; fi++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, fmts[fi].f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            printf("depth %s: not supported as attachment, skipped\n", fmts[fi].n);
            continue;
        }
        VkPipeline p = gfx_pipeline(m_color, gfx_layout, 0, fmts[fi].f);
        for (int r = 0; r < nres; r++)
            for (int dv = 0; dv < 2; dv++) {
                char name[96], label[64];
                const char *dvn = dv ? "dxvk" : "plain";
                snprintf(name, sizeof name, "depth %ux%u %s ds=%s", res[r].w, res[r].h, fmts[fi].n, dvn);
                if (!want(name)) continue;
                VkImageUsageFlags du = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                if (dv) du |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                image rt, ds;
                VkImageUsageFlags ru; VkImageCreateFlags rf; int nl;
                variant_info(V_PLAIN, 1, &ru, &rf, &nl);
                snprintf(label, sizeof label, "rt %ux%u plain (depth)", res[r].w, res[r].h);
                if (!make_image(&rt, label, VK_FORMAT_R8G8B8A8_UNORM, res[r].w, res[r].h, 1, ru, rf, nl, VK_IMAGE_ASPECT_COLOR_BIT)) continue;
                snprintf(label, sizeof label, "ds %ux%u %s %s", res[r].w, res[r].h, fmts[fi].n, dvn);
                if (!make_image(&ds, label, fmts[fi].f, res[r].w, res[r].h, 1, du, 0, 0, fmts[fi].a)) { free_image(&rt); continue; }
                for (int order = 1; order <= 2; order++) {
                    begin();
                    begin_rendering(&rt, &ds);
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                    draw_layers(gfx_layout, layers, 1.0f, order);
                    pEndRendering(cb);
                    char n2[128];
                    snprintf(n2, sizeof n2, "%s %s x%d", name, order == 1 ? "b2f" : "f2b", layers);
                    report(n2, run(), (double)res[r].w * res[r].h * layers, "Gpix/s");
                }
                CK(vkQueueWaitIdle(queue));
                free_image(&ds);
                free_image(&rt);
            }
    }
}

static void test_compute(void)
{
    if (!want("comp bw") && !want("comp alu")) return;
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    VkDeviceSize size = 256ull << 20;
    if (size > p2.properties.limits.maxStorageBufferRange) size = p2.properties.limits.maxStorageBufferRange & ~(VkDeviceSize)0xffff;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    CK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryPropertyFlags got = 0;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &got)};
    VkDeviceMemory mem;
    CK(vkAllocateMemory(dev, &ai, NULL, &mem));
    CK(vkBindBufferMemory(dev, buf, mem, 0));
    printf("  [buffer] storage %.0f MB usage STORAGE|TDST mem %s type %u\n", size / 1048576.0, mem_str(got), ai.memoryTypeIndex);
    VkDescriptorSet set = alloc_set(buf_dsl);
    VkDescriptorBufferInfo dbi = {buf, 0, size};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    begin();
    vkCmdFillBuffer(cb, buf, 0, VK_WHOLE_SIZE, 0x3f800000);
    run();
    VkPipeline bw = comp_pipeline(m_bw), alu = comp_pipeline(m_alu);
    uint32_t pc[2] = {(uint32_t)(size / 16), 0};
    if (want("comp bw")) {
        begin();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bw);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        vkCmdDispatch(cb, 4096, 1, 1);
        char name[96];
        snprintf(name, sizeof name, "comp bw %.0fMB read+write", size / 1048576.0);
        report(name, run(), 2.0 * size, "GB/s");
    }
    if (want("comp alu")) {
        pc[1] = 512;
        uint32_t groups = 4096;
        begin();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, alu);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        vkCmdDispatch(cb, groups, 1, 1);
        /* 4 vec4 FMAs per iteration (the y*0.001 muls fold into nothing extra on most compilers): 32 flops */
        report("comp alu 1M threads x512 iters", run(), (double)groups * 256 * pc[1] * 32, "GFLOP/s");
    }
    CK(vkQueueWaitIdle(queue));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    filter = argc > 1 ? argv[1] : NULL;
    if (getenv("GPUBENCH_REPS")) reps = atoi(getenv("GPUBENCH_REPS"));
    if (reps < 1) reps = 1;
    if (getenv("GPUBENCH_QUICK")) nres = 1;
    if (getenv("GPUBENCH_VERBOSE")) verbose = atoi(getenv("GPUBENCH_VERBOSE"));
    if (getenv("GPUBENCH_MEMTYPE")) force_memtype = atoi(getenv("GPUBENCH_MEMTYPE"));
    if (getenv("GPUBENCH_LOOP")) loop = atoi(getenv("GPUBENCH_LOOP"));
    if (loop < 1) loop = 1;
    layers = LAYERS * loop;
    load_vulkan();
    create_device();
    create_layouts();
    if (force_memtype >= 0 || loop > 1 || verbose) printf("options: memtype %d, loop %d, verbose %d\n", force_memtype, loop, verbose);
    printf("reps %d (median), layers %d, taps %d, texture %dx%d RGBA8 mipmapped\n", reps, layers, TAPS, TEX, TEX);
    double t0 = now_ms();
    {
        /* Warm-up: ~2 s of fill so DVFS has the GPU at speed before the first measurement. */
        VkPipeline p = gfx_pipeline(m_color, gfx_layout, 1, VK_FORMAT_UNDEFINED);
        image rt;
        VkImageUsageFlags u; VkImageCreateFlags f; int nl;
        variant_info(V_PLAIN, 1, &u, &f, &nl);
        make_image(&rt, "rt warm-up", VK_FORMAT_R8G8B8A8_UNORM, 1920, 1080, 1, u, f, nl, VK_IMAGE_ASPECT_COLOR_BIT);
        begin();
        begin_rendering(&rt, NULL);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        draw_layers(gfx_layout, LAYERS, 0.5f, 0);
        pEndRendering(cb);
        int saved = reps;
        reps = 1;
        run();
        reps = saved;
        for (double tw = now_ms(); now_ms() - tw < 2000;) submit_wait();
        CK(vkQueueWaitIdle(queue));
        free_image(&rt);
    }
    test_fill();
    test_tex();
    test_depth();
    test_compute();
    CK(vkDeviceWaitIdle(dev));
    printf("done in %.1f s\n", (now_ms() - t0) / 1e3);
    return 0;
}
