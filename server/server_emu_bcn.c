/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * BC1-BC7 textures on a GPU without them (textureCompressionBC missing; DXVK and vkd3d-proton
 * refuse such a device). The image the app creates in a BC format is created decoded instead
 * (RGBA8 / R8 / RG8 / RGBA16F, 16-bit for most BC1 UNORM; vkb_emu_formats.h) with the app's own
 * usage, and every upload - vkCmdCopyBufferToImage(2), how all BC data arrives - becomes a
 * compute dispatch that decodes the blocks from the source buffer into a scratch buffer
 * (shaders/bc_decode.comp), then a copy from there into the image. The image so needs no STORAGE usage: on Mali-G720 that would turn off
 * AFBC, and a 4096x4096 RGBA8 texture with it sampled 2.3x slower (gpubench: 4.71 ms against
 * 2.05 ms for 16 taps at 720p). The app's compute bindings are restored afterwards
 * (server_emu_cmdstate.c), and the scratch goes back when the command buffer is begun again,
 * reset or freed (bounded meanwhile, see LIVE_MAX).
 *
 * Where the GPU samples ETC2 / EAC / ASTC (Mali does), BC images are transcoded instead of decoded:
 * the image is created in a compressed format with BC's block size (tx_target), and the decoder
 * variant encodes each block again (etc2_enc.glsl, astc_enc.glsl, eac_enc.glsl) into the scratch,
 * which is copied into the image as compressed blocks. Subnautica on a Mali-G720 held 4.75 GB of
 * decoded textures, 0.97 GB as BC, and ran out of memory (2026-10-12). BC6H goes to ASTC 4x4 HDR
 * where the GPU samples it (textureCompressionASTC_HDR, which the device is then created with),
 * instead of RGBA16F at 8x its size. Lossy (PSNR in tests/bcn_check.c); VKBRIDGE_BC_TRANSCODE=0
 * decodes as before.
 *
 * A copy into a BC image from a size-compatible uncompressed one (R32G32B32A32_UINT for 16-byte
 * blocks, R32G32_UINT for 8-byte ones: D3D11 CopyResource between block-compatible formats, vkd3d's
 * BC aliasing) takes the source's texels as blocks: they are copied to a staging buffer and decoded
 * (or transcoded) from there like an upload (idea from bcn_layer f3cb7e9, Pipetto-crypto / Lee Gao, MIT).
 *
 * A copy between a transcoded and a decoded image of one BC format (the conversion budget decodes
 * some images and transcodes others; UE4 copies the levels a streamed texture already has from its
 * old image into its new one) goes from the transcoded one by a blit (the GPU decodes its blocks) and
 * from the decoded one by re-encoding its texels with the transcoder (copy_mixed); both counted.
 *
 * Not emulated (logged once, counted): reading a BC image back to a buffer, copies from a BC image
 * into an uncompressed one (the image holds decoded or transcoded texels; giving BC blocks back would
 * need a BC encoder), and copies between transcoded images of different block sizes.
 */
#define _GNU_SOURCE
#include "server_bc_auto.h"
#include "server_emu.h"
#include "vkb_emu_formats.h"
#include "bc_spv.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* FAM_BC1_16: BC1 UNORM decoded to 16 bits (vkb_bc_packed_format), a decoder variant, not a family;
 * TX_*: the transcoders (TX_BC1 + family); TX6_* / TX8_*: into ASTC 6x6 / 8x8 (VKBRIDGE_BC_ASTC_BLOCK). */
enum { FAM_BC1, FAM_BC2, FAM_BC3, FAM_BC4, FAM_BC5, FAM_BC6H, FAM_BC7, FAM_BC1_16, TX_BC1, TX_BC2, TX_BC3, TX_BC4, TX_BC5,
       TX_BC6H, TX_BC7, TX6_BC2, TX6_BC3, TX6_BC7, TX8_BC2, TX8_BC3, TX8_BC7, TX_BC1_ASTC,
       /* re-encoders (bc_decode.comp SRC_RAW): a decoded image's texels into a transcoded format */
       RAW_ETC2, RAW_ASTC, RAW_EAC_R, RAW_EAC_RG, RAW_ASTC_HDR, RAW6_ASTC, RAW8_ASTC, FAM_COUNT };
#define BC_INDEX(f) ((f) - VK_FORMAT_BC1_RGB_UNORM_BLOCK)

/* Scratch: chunks of 1-16 MiB; one command buffer holds up to CB_MAX before it reuses its own
 * (after copying out what they hold), and the process keeps up to IDLE_MAX for the next ones.
 * All scratch alive (held and idle, every device) stays under LIVE_MAX: past it a command buffer
 * that already has a chunk reuses its own instead of allocating, and one that has none gets only
 * what the upload in hand needs. Command buffers keep their scratch until they are begun, reset or
 * freed, and with CB_MAX at 64 MiB and no global bound, Subnautica loading a save took the server
 * from 2.9 to 5.6 GB in 25 s and the 11.5 GB tablet ran out of memory (2026-10-12). */
#define CHUNK_MIN ((VkDeviceSize)1 << 20)
#define CHUNK_MAX ((VkDeviceSize)16 << 20)
#define CB_MAX ((VkDeviceSize)16 << 20)
#define IDLE_MAX ((VkDeviceSize)32 << 20)
#define LIVE_MAX ((VkDeviceSize)96 << 20)
#define SLOT_ALIGN 256

static VkDeviceSize g_live, g_idle; /* bytes of scratch alive / idle in pools, all devices (atomic) */

VkDeviceSize vkb_emu_bcn_scratch_live(void) { return __atomic_load_n(&g_live, __ATOMIC_RELAXED); }

typedef struct chunk {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
} chunk;

typedef struct bcn_cb {      /* a command buffer's scratch */
    chunk *held;
    uint32_t n, cap;
    uint32_t cur;            /* the chunk the next slot comes from, `used` bytes into it */
    VkDeviceSize used;
    chunk *stage;            /* blocks copied out of uncompressed images, apart from the decode scratch */
    uint32_t nstage, capstage;
    /* blocks drawn into uncompressed views (bc_sink): the render pass (rendering) being recorded */
    VkRect2D vp;             /* the viewports of the last vkCmdSetViewport, as one rect (while sink views exist) */
    int vp_known;
    uint32_t npass;          /* sink attachments of the render pass instance being recorded */
    struct pass_sink {
        VkImage bc;
        struct bc_sink *sink;
        uint32_t layer0, layers;
        VkImageLayout fin;   /* the layout the attachment is left in (the app's image as it sees it, and the sink) */
    } pass[4];
    VkRect2D area;           /* its renderArea */
    VkRect2D drawn[8];       /* what the viewports set and the rects cleared in it cover (more: their bounding box) */
    uint32_t ndrawn;
    int vp_in, clear_in;     /* a viewport was set in it, a rect cleared in it */
    VkRect2D pre;            /* the viewport in effect when it began (pre_known), for draws before any is set in it */
    int pre_known;
    int whole;               /* the renderArea (a clear at load, secondary command buffers, nothing known) */
} bcn_cb;

#define WARM_THREADS 2

typedef struct bcn_state {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipes[3][FAM_COUNT]; /* per BC_QUALITY (decoders: [1] only) */
    VkDeviceSize ssbo_align;
    int push;                /* VK_KHR_push_descriptor; else per-command-buffer sets */
    VkPhysicalDeviceMemoryProperties mp;
    int packed_ok[2];        /* R5G6B5, A1R5G5B5 can hold BC1 UNORM */
    VkFormat tx[16];         /* per BC format (BC_INDEX): what it is transcoded to, UNDEFINED: decoded */
    uint32_t astc_n;         /* VKBRIDGE_BC_ASTC_BLOCK: 4, 6 or 8 (BC2 / BC3 / BC7's ASTC target) */
    uint32_t quality;        /* the transcoders' search (bc_decode.comp's BC_QUALITY): 0 fast, 1 default, 2 high,
                              * QUALITY_AUTO: fast for large mip levels, default for the rest (level_quality) */
    pthread_mutex_t lock;
    /* pipeline builds: once each (a caller finding one under way waits for it, or under auto takes
     * the other quality if that is built); the warm threads build ahead from the first BC image on */
    pthread_mutex_t plock;
    pthread_cond_t pcond;
    uint8_t building[3][FAM_COUNT], failed[3][FAM_COUNT];
    uint32_t urgent[3], want[3]; /* per quality, a bit per family: for the warm threads, urgent first */
    pthread_t warm[WARM_THREADS];
    int nwarm, warm_stop, warm_all;
    chunk *idle;             /* scratch no pending command buffer uses (bytes in g_idle) */
    uint32_t nidle, capidle;
    uint64_t *pin;           /* shapes whose memory requirements were asked before an image (budget_take), PIN_SLOTS */
    uint32_t npin;
    int pin_full;
    /* VKBRIDGE_BC_TX_BUDGET=auto (budget_auto): this GPU's rate and burst, whether short images may be
     * encoded fast instead of decoded, and how the numbers were found */
    double auto_rate, auto_burst;
    int auto_fast;
    char auto_why[96];
    int batch;               /* VKBRIDGE_BC_BATCH (default 1): back-to-back uploads share one barrier pair (bc_batch) */
} bcn_state;

typedef struct pc_t {
    uint32_t src_word, row_blocks, layer_blocks, flags;
    uint32_t dst_word, pad;
    uint32_t extent[2];
    uint32_t nx[4]; /* ASTC 6x6 / 8x8 transcoders only (bc_decode.comp ENC_ASTCN) */
} pc_t;

static void prebuild(vkb_emu_device *e, int fam);
static void prebuild_later(vkb_emu_device *e, int fam);

static int family(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return FAM_BC1;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: return FAM_BC2;
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: return FAM_BC3;
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return FAM_BC4;
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: return FAM_BC5;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: return FAM_BC6H;
    default: return FAM_BC7;
    }
}

static uint32_t family_flags(const vkb_emu_image *img)
{
    VkFormat f = img->client_format;
    uint32_t fl = 0;
    if (f == VK_FORMAT_BC1_RGB_UNORM_BLOCK || f == VK_FORMAT_BC1_RGB_SRGB_BLOCK) fl |= 1;
    if (f == VK_FORMAT_BC4_SNORM_BLOCK || f == VK_FORMAT_BC5_SNORM_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK) fl |= 2;
    if (img->format == VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK || img->format == VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK) fl |= 4;
    return fl;
}

/* What a BC format is transcoded to where the GPU samples it: BC1 -> ETC2 RGB8 (RGB8A1 with alpha;
 * BC1's 4 bits a texel), BC2 / BC3 / BC7 -> ASTC 4x4 (8 bits like them, and better than ETC2
 * RGBA8 at BC7's colours), BC4 -> EAC R11, BC5 -> EAC RG11 (BC4/BC5's sizes, 11-bit), BC6H (UFLOAT
 * and SFLOAT) -> ASTC 4x4 HDR (8 bits, half floats; no negative values, which SFLOAT loses). */
/* VKBRIDGE_BC1_ASTC=1: BC1 to ASTC 4x4 (bc_decode.comp bc1_astc: its colours and indices as they
 * are, no search; twice ETC2's memory) instead of ETC2, whose search took hundreds of ms a large
 * texture: Dark Souls III froze 300-700 ms streaming BC1 textures on the G925. */
static int bc1_astc(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC1_ASTC"); /* <cache dir>/env too */
    return v && strcmp(v, "0");
}

static VkFormat tx_target(VkFormat f, uint32_t n)
{
    if (bc1_astc() && f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
        return f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK
                                                                                     : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    int srgb = f == VK_FORMAT_BC2_SRGB_BLOCK || f == VK_FORMAT_BC3_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK;
    int astc = (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC3_SRGB_BLOCK) || f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK;
    if ((n == 6 || n == 8) && astc)
        return n == 6 ? (srgb ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK : VK_FORMAT_ASTC_6x6_UNORM_BLOCK)
                      : (srgb ? VK_FORMAT_ASTC_8x8_SRGB_BLOCK : VK_FORMAT_ASTC_8x8_UNORM_BLOCK);
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_EAC_R11_SNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    case VK_FORMAT_BC5_SNORM_BLOCK: return VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: return VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* Formats one image's views may share (Vulkan's compatibility classes): 0 for none of these. */
static int tx_class(VkFormat t)
{
    switch (t) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return 1;
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return 2;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return 3;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: case VK_FORMAT_EAC_R11_SNORM_BLOCK: return 4;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return 5;
    case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK: return 6;
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: return 7;
    case VK_FORMAT_ASTC_8x8_UNORM_BLOCK: case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return 8;
    default: return 0;
    }
}

int vkb_emu_bcn_transcode_wanted(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC_TRANSCODE"); /* <cache dir>/env too, read at device creation */
    return !v || strcmp(v, "0");
}

/* VKBRIDGE_BC_TRANSCODE_QUALITY: auto (the default), fast, default or high (unknown values: auto).
 * The same in both modes (server_mode.c): below 1024x1024 auto is default's search (the same
 * picture), and it measured 1.3-4x faster than default on both test tablets. */
#define QUALITY_AUTO 3u
static uint32_t transcode_quality(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC_TRANSCODE_QUALITY"); /* <cache dir>/env too */
    if (!v || !strcmp(v, "auto")) return QUALITY_AUTO;
    if (!strcmp(v, "fast")) return 0;
    if (!strcmp(v, "default")) return 1;
    if (!strcmp(v, "high")) return 2;
    VKB_WARN("BC emulation: VKBRIDGE_BC_TRANSCODE_QUALITY=%s unknown (auto, fast, default, high): auto", v);
    return QUALITY_AUTO;
}

/* VKBRIDGE_BC_ASTC_BLOCK: the ASTC block BC2 / BC3 / BC7 are transcoded to - 4x4 (the default: 8
 * bits a texel, as BC2 / BC3 / BC7), 6x6 (3.56 bits) or 8x8 (2 bits): 2.25 or 4 times less memory
 * for those textures, at a cost in detail (bcn_check measures it). BC1 stays ETC2 (its cut-out
 * alpha lost too much in 8x8 blocks: 20 dB), BC4 / BC5 EAC, BC6H ASTC 4x4 HDR. ASTC 6x6 and 8x8
 * are in every GPU's ASTC LDR (textureCompressionASTC_LDR names all block sizes); the format's
 * features are checked like the others'. */
static uint32_t astc_block(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC_ASTC_BLOCK"); /* <cache dir>/env and presets too */
    if (!v || !*v) return 4;
    uint32_t n = !strcmp(v, "6x6") ? 6 : !strcmp(v, "8x8") ? 8 : !strcmp(v, "4x4") ? 4 : 0;
    if (!n) {
        VKB_WARN("BC emulation: VKBRIDGE_BC_ASTC_BLOCK=%s unknown (4x4, 6x6, 8x8): 4x4", v);
        return 4;
    }
    return n;
}

/* The transcoder of a BC family into a target format. */
static int tx_family(int fam, VkFormat target)
{
    uint32_t n = vkb_tx_block_dim(target);
    if (fam == FAM_BC1 && (target == VK_FORMAT_ASTC_4x4_UNORM_BLOCK || target == VK_FORMAT_ASTC_4x4_SRGB_BLOCK)) return TX_BC1_ASTC;
    if (n == 4) return TX_BC1 + fam;
    int base = n == 6 ? TX6_BC2 : TX8_BC2;
    return fam == FAM_BC2 ? base : fam == FAM_BC3 ? base + 1 : base + 2;
}

/* The re-encoder into a transcoded format (copy_mixed), -1 for none. */
static int raw_family(VkFormat target)
{
    switch (tx_class(target)) {
    case 1: case 2: return RAW_ETC2;
    case 3: return RAW_ASTC;
    case 4: return RAW_EAC_R;
    case 5: return RAW_EAC_RG;
    case 6: return RAW_ASTC_HDR;
    case 7: return RAW6_ASTC;
    case 8: return RAW8_ASTC;
    default: return -1;
    }
}

/* Bytes of a texel of a decoded image the re-encoder raw takes (0: it takes no such texels), and
 * the flags that say how they are packed (bc_decode.comp SRC_RAW). */
static uint32_t raw_texel(int raw, VkFormat decoded, uint32_t *flags)
{
    *flags = 0;
    switch (raw) {
    case RAW_ETC2: case RAW_ASTC: case RAW6_ASTC: case RAW8_ASTC:
        if (decoded == VK_FORMAT_R8G8B8A8_UNORM || decoded == VK_FORMAT_R8G8B8A8_SRGB) return 4;
        if (decoded == VK_FORMAT_R5G6B5_UNORM_PACK16) return *flags = 8, 2;
        if (decoded == VK_FORMAT_A1R5G5B5_UNORM_PACK16) return *flags = 16, 2;
        return 0;
    case RAW_EAC_R: return decoded == VK_FORMAT_R8_UNORM || decoded == VK_FORMAT_R8_SNORM;
    case RAW_EAC_RG: return decoded == VK_FORMAT_R8G8_UNORM || decoded == VK_FORMAT_R8G8_SNORM ? 2 : 0;
    case RAW_ASTC_HDR: return decoded == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 0;
    default: return 0;
    }
}

/* auto: mip levels of AUTO_FAST_TEXELS texels or more (1024x1024, 2048x512, ...) are encoded fast,
 * smaller ones at default. The large levels are nearly all of the work (4096, 2048 and 1024 are
 * 99.6% of a 4096 chain's texels) and the least visible: a game at 720p-1080p samples a 1024+ level
 * at full size only close up, and fast costs 0.3-1.2 dB there (bc-quality report). Small levels,
 * which distant surfaces and small textures (UI, decals) show, cost little to encode at default.
 * The choice is per level (not per region), so a level streamed in pieces is encoded one way. */
#define AUTO_FAST_TEXELS (1024u * 1024u)
static uint32_t level_quality(const bcn_state *s, const vkb_emu_image *img, uint32_t mip)
{
    if (s->quality != QUALITY_AUTO) return s->quality;
    if (img->bc_fast) return 0; /* the auto budget, short of texels: fast everywhere (budget_take) */
    uint32_t w = img->extent.width >> mip, h = img->extent.height >> mip;
    return (uint64_t)(w ? w : 1) * (h ? h : 1) >= AUTO_FAST_TEXELS ? 0 : 1;
}

int vkb_emu_bcn_astc_hdr(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t api)
{
    if (!vkb_emu_bcn_transcode_wanted() || vkb_emu_faked("astchdr") || api < VK_API_VERSION_1_1 || !idt->vkGetPhysicalDeviceFeatures2)
        return 0;
    VkPhysicalDeviceProperties p;
    idt->vkGetPhysicalDeviceProperties(pd, &p);
    int core = p.apiVersion >= VK_API_VERSION_1_3 && api >= VK_API_VERSION_1_3, ext = 0;
    if (!core) {
        uint32_t n = 0;
        idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
        idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
        for (uint32_t i = 0; i < n; i++) ext |= !strcmp(e[i].extensionName, "VK_EXT_texture_compression_astc_hdr");
        free(e);
        if (!ext) return 0;
    }
    VkPhysicalDeviceTextureCompressionASTCHDRFeatures hf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hf};
    idt->vkGetPhysicalDeviceFeatures2(pd, &f2);
    return hf.textureCompressionASTC_HDR ? (core ? 1 : 2) : 0;
}

/* Bytes one block takes in the scratch: transcoded, or 16 decoded texels. */
static uint32_t out_block_bytes(const vkb_emu_image *img)
{
    uint32_t tx = vkb_tx_block_bytes(img->format);
    if (tx) return tx;
    if (vkb_bc_is_packed(img->format)) return 32;
    switch (family(img->client_format)) {
    case FAM_BC4: return 16;
    case FAM_BC5: return 32;
    case FAM_BC6H: return 128;
    default: return 64;
    }
}

int vkb_emu_bcn_supported(const vkb_dispatch *idt, VkPhysicalDevice pd)
{
    VkPhysicalDeviceFeatures f;
    idt->vkGetPhysicalDeviceFeatures(pd, &f);
    if (!f.shaderStorageImageExtendedFormats) {
        VKB_WARN("BC emulation: the GPU lacks shaderStorageImageExtendedFormats");
        return 0;
    }
    static const VkFormat need[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        VkFormatProperties fp;
        idt->vkGetPhysicalDeviceFormatProperties(pd, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            VKB_WARN("BC emulation: format %d cannot be a storage image", need[i]);
            return 0;
        }
    }
    VkPhysicalDeviceProperties p;
    idt->vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion >= VK_API_VERSION_1_4) return 1;
    uint32_t n = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    free(e);
    /* Without VK_KHR_push_descriptor (Mali-G720 on driver 44.1) the decoder binds ordinary sets. */
    return 1;
}

/* Push descriptors for the decoder; without them, sets from the command buffer's scratch pools.
 * VKBRIDGE_FAKE_MISSING=...,push takes the second way on a GPU that has them (host tests). */
static int has_push(vkb_srv_table *dev)
{
    if (vkb_emu_faked("push")) return 0; /* the process environment or <cache dir>/env */
    VkPhysicalDeviceProperties p;
    dev->real.vkGetPhysicalDeviceProperties(dev->physical, &p);
    if (p.apiVersion >= VK_API_VERSION_1_4 && dev->api_version >= VK_API_VERSION_1_4) return dev->real.vkCmdPushDescriptorSet != NULL;
    uint32_t n = 0;
    dev->real.vkEnumerateDeviceExtensionProperties(dev->physical, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    dev->real.vkEnumerateDeviceExtensionProperties(dev->physical, NULL, &n, e);
    int ok = 0;
    for (uint32_t i = 0; i < n; i++) ok |= !strcmp(e[i].extensionName, "VK_KHR_push_descriptor");
    free(e);
    return ok && dev->real.vkCmdPushDescriptorSet != NULL;
}

/* ------------------------------------------------------------------ images */

/* The other decoded format a view of the image may take (sRGB / SNORM), or the format itself. */
static VkFormat sibling(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8_SNORM: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: return VK_FORMAT_EAC_R11_SNORM_BLOCK;
    case VK_FORMAT_EAC_R11_SNORM_BLOCK: return VK_FORMAT_EAC_R11_UNORM_BLOCK;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: return VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
    case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    case VK_FORMAT_ASTC_8x8_UNORM_BLOCK: return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;
    case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    default: return f;
    }
}

/* BC1 UNORM decoded to 16 bits: half the memory of RGBA8 for the format most textures use, and
 * BC1's endpoints are 5:6:5 so only interpolated colours lose precision. Only where every view
 * of the image can be that format: not mutable into another BC format (a list naming only the
 * image's own format or uncompressed block-texel views), not storage. UNDEFINED otherwise.
 * DXVK makes every D3D11 BC image mutable with its DXGI family (BC1: RGBA UNORM + RGBA SRGB),
 * typed or typeless alike, so under DXVK no BC1 image gets here: Subnautica kept 90 BC1 UNORM
 * images (47 MB) in RGBA8 (2026-10-12). A typeless texture may take an sRGB view, which no
 * 16-bit format can serve, and a typed one looks the same from here, so they stay RGBA8. */
static VkFormat packed_format(const bcn_state *s, const VkImageCreateInfo *ci, const VkImageFormatListCreateInfo *fl)
{
    VkFormat p = vkb_bc_packed_format(ci->format);
    if (!p || !s || !s->packed_ok[p == VK_FORMAT_A1R5G5B5_UNORM_PACK16] || (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_FORMAT_UNDEFINED;
    if (ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
        if (!fl || !fl->viewFormatCount) return VK_FORMAT_UNDEFINED;
        for (uint32_t i = 0; i < fl->viewFormatCount; i++)
            if (vkb_is_bc(fl->pViewFormats[i]) && fl->pViewFormats[i] != ci->format) return VK_FORMAT_UNDEFINED;
    }
    return p;
}

/* The format a BC image is transcoded to, UNDEFINED where it is decoded: the GPU samples the
 * target (vkb_emu_bcn_install), the image is only a texture (sampled, copied), every format it may
 * be viewed in is a BC format whose target shares the class (DXVK: UNORM + SRGB of one BC format),
 * and the driver takes the image in the target format. Views in uncompressed formats
 * (BLOCK_TEXEL_VIEW_COMPATIBLE) see the image's format as when decoded (emu_CreateImageView). */
/* ------------------------------------------------------------------ the conversion budget */

/* VKBRIDGE_BC_TX_BUDGET=<rate>[:<burst>] (the game's preset, <cache dir>/env, the server's
 * environment; 0 or off: no budget). A game streaming textures in creates them just before it
 * uploads them, and their transcodes run on the GPU in the frame that uploads them: Dark Souls III
 * froze 300-1000 ms while walking on the G925 with the GPU 98% in compute. With a budget, the
 * transcodes the GPU is given are spread out: a bucket of `burst` million texels refills at `rate`
 * million texels a second, each image to transcode takes its texels (every level and layer,
 * weighted by its transcoder's cost, BC7 -> ASTC 4x4 = 1), and an image that finds the bucket short
 * is decoded instead - a plain copy of its colours, 2-4x the memory of the transcoded image and a
 * fraction of the GPU time, at the original's exact colours. An image of more than the whole burst
 * is transcoded when the bucket is full (VKBRIDGE_BC_TRANSCODE_MAX sends big ones to the decoder
 * always). The decoded images' extra memory is capped (VKBRIDGE_BC_TX_FALLBACK_MB; past it images
 * are transcoded as without a budget). Images of a shape the app asked memory requirements for
 * before making one (vkGetDeviceImageMemoryRequirements: vkd3d's placed resources) are always
 * transcoded: the size it was told is the size it gets. BC1 -> ASTC direct (VKBRIDGE_BC1_ASTC) counts
 * at its low cost but is never decoded. */
/* auto (2026-11-04): on Mali GPUs a bucket sized to the GPU
 * (budget_auto), on other GPUs no budget. Off by default (BUDGET_DEFAULT) until the Ultra A/B. A number
 * means what it did before (Dark Souls III's built-in preset sets 32:4 on the G925 / G720, where
 * the transcode freezes were seen; 32:4 is host, G720-calibrated, FEAT-bc-budget.md, a 32-texture
 * burst 313 -> 86 ms); 0 or off: no budget. Under auto, three things differ from a number:
 *   - an image's texels at levels the default search encodes (auto quality: under 1024x1024) count
 *     AUTO_DEFAULT_COST times (the default search is 1.3-4x the fast one's GPU time on the tablets;
 *     the weights below were measured on 4096 chains, encoded fast);
 *   - an image the bucket is short for is first encoded with the fast search at every level when
 *     that fits (no memory cost; 0.2-2 dB PSNR on those levels), and only decoded when even that
 *     does not (not on the Immortalis G1 / G2 class: their fast search is what showed the D9500's
 *     green squares, DroidDeck#218);
 *   - the decoded images' extra memory is capped lower (AUTO_FALLBACK: a 48th of the heap, 64-256 MB:
 *     8 GB phones are where memory runs out). */
#define BUDGET_DEFAULT "0"
#define PIN_SLOTS 4096 /* per device; full: no more decoding over the budget on it */
#define AUTO_DEFAULT_COST 2.0
#define AUTO_HIGH_COST 3.0

typedef struct budget_cfg {
    double rate, burst; /* million texels a second, million texels */
    int is_auto;
} budget_cfg;

/* VKBRIDGE_TEST_BC_GPU_NAME (host tests, the server's environment only): the device name budget_auto
 * reads ('_' for a space, for scripts that split words). */
static const char *auto_name(const char *real)
{
    static char name[64];
    const char *t = getenv("VKBRIDGE_TEST_BC_GPU_NAME");
    if (!t || !*t) return real;
    snprintf(name, sizeof(name), "%s", t);
    for (char *c = name; *c; c++)
        if (*c == '_') *c = ' ';
    return name;
}

/* The budget for an image on s's device: 0 none, else c filled. */
static int budget_get(const bcn_state *s, budget_cfg *c)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC_TX_BUDGET");
    if (!v || !*v) v = BUDGET_DEFAULT;
    c->is_auto = 0;
    if (!strcmp(v, "auto")) {
        if (!s || !(s->auto_rate > 0)) return 0;
        c->rate = s->auto_rate, c->burst = s->auto_burst, c->is_auto = 1;
        return 1;
    }
    char *end;
    double r = strtod(v, &end), b = 0;
    if (*end == ':' || *end == ',') b = strtod(end + 1, NULL);
    if (!(r > 0)) return 0;
    c->rate = r;
    c->burst = b > 0 ? b : r / 8; /* 125 ms of the rate */
    return 1;
}

/* A transcoder's GPU time a texel against BC7 -> ASTC 4x4's: the G720's bcn_check 4096x4096x2
 * uploads, warm, auto quality (2026-10-15, bcn_check-v04): BC7 139-165 ms, BC1 -> ETC2 182-191,
 * BC3 124-155, BC4 54-56, BC5 104-107. BC1 -> ASTC direct (VKBRIDGE_BC1_ASTC) has no search. */
static double tx_weight(int txfam)
{
    switch (txfam) {
    case TX_BC1: return 1.25;
    case TX_BC2: case TX_BC3: case TX6_BC2: case TX6_BC3: case TX8_BC2: case TX8_BC3: return 0.9;
    case TX_BC4: return 0.4;
    case TX_BC5: return 0.7;
    case TX_BC1_ASTC: return 0.25;
    default: return 1.0;
    }
}

/* Texels of every level and layer. */
static uint64_t image_texels(const VkImageCreateInfo *ci)
{
    uint64_t n = 0;
    for (uint32_t m = 0; m < ci->mipLevels && m < 32; m++) {
        uint64_t w = ci->extent.width >> m, h = ci->extent.height >> m, d = ci->extent.depth >> m;
        n += (w ? w : 1) * (h ? h : 1) * (ci->imageType == VK_IMAGE_TYPE_3D ? (d ? d : 1) : 1);
    }
    return n * (ci->imageType == VK_IMAGE_TYPE_3D ? 1 : ci->arrayLayers);
}

/* auto: the image's texels weighted by each level's search (fast 1, default AUTO_DEFAULT_COST, high
 * AUTO_HIGH_COST), as level_quality picks it; all_fast: as if every level were fast. */
static double image_cost_q(const bcn_state *s, const VkImageCreateInfo *ci, int all_fast)
{
    double n = 0;
    for (uint32_t m = 0; m < ci->mipLevels && m < 32; m++) {
        uint64_t w = ci->extent.width >> m, h = ci->extent.height >> m, d = ci->extent.depth >> m;
        w = w ? w : 1, h = h ? h : 1;
        double t = (double)(w * h * (ci->imageType == VK_IMAGE_TYPE_3D ? (d ? d : 1) : 1));
        uint32_t q = all_fast ? 0 : s->quality != QUALITY_AUTO ? s->quality : w * h >= AUTO_FAST_TEXELS ? 0 : 1;
        n += t * (q == 0 ? 1.0 : q == 2 ? AUTO_HIGH_COST : AUTO_DEFAULT_COST);
    }
    return n * (ci->imageType == VK_IMAGE_TYPE_3D ? 1 : ci->arrayLayers);
}

/* What decoding an image costs in memory over transcoding it to t, in bytes. */
static int64_t fallback_bytes(const bcn_state *s, const VkImageCreateInfo *ci, const VkImageFormatListCreateInfo *fl, VkFormat t)
{
    double dec;
    if (packed_format(s, ci, fl)) dec = 2;
    else switch (vkb_bc_view_format(ci->format)) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: dec = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: dec = 2; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: dec = 8; break;
    default: dec = 4;
    }
    uint32_t n = vkb_tx_block_dim(t);
    double txb = (double)vkb_tx_block_bytes(t) / (n * n);
    return (int64_t)((dec - txb) * (double)image_texels(ci));
}

/* VKBRIDGE_BC_TX_FALLBACK_MB: the most decoded-over-the-budget images may cost over transcoded
 * ones, all devices; by default a 16th of the largest device-local heap, 128-512 MB. */
static int64_t fallback_cap(const bcn_state *s, int is_auto)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC_TX_FALLBACK_MB");
    if (v && *v) return (int64_t)atol(v) << 20;
    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < s->mp.memoryHeapCount; i++)
        if ((s->mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && s->mp.memoryHeaps[i].size > heap) heap = s->mp.memoryHeaps[i].size;
    /* auto: a 48th, 64-256 MB (AUTO_FALLBACK); a number: a 16th, 128-512 MB */
    int64_t cap = (int64_t)(heap / (is_auto ? 48 : 16)), lo = (is_auto ? 64ll : 128ll) << 20, hi = (is_auto ? 256ll : 512ll) << 20;
    if (cap < lo) cap = lo;
    if (cap > hi) cap = hi;
    return cap;
}

/* An image's shape, as memory requirements depend on it. */
static uint64_t shape_key(const VkImageCreateInfo *ci)
{
    const uint32_t f[] = {(uint32_t)ci->format, (uint32_t)ci->imageType, ci->extent.width, ci->extent.height, ci->extent.depth,
                          ci->mipLevels, ci->arrayLayers, (uint32_t)ci->samples, (uint32_t)ci->tiling, ci->usage, ci->flags};
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) h = (h ^ f[i]) * 1099511628211ull;
    return h | 1;
}

/* The shape is pinned (always transcoded); add: pin it. */
static int pinned(bcn_state *s, uint64_t key, int add)
{
    int hit = 0;
    pthread_mutex_lock(&s->lock);
    if (!s->pin && add) s->pin = calloc(PIN_SLOTS, sizeof(*s->pin));
    if (s->pin) {
        uint32_t i = (uint32_t)(key >> 20) % PIN_SLOTS, k = 0;
        while (k < PIN_SLOTS && s->pin[i] && s->pin[i] != key) i = (i + 1) % PIN_SLOTS, k++;
        hit = k < PIN_SLOTS && s->pin[i] == key;
        if (!hit && add) {
            if (k < PIN_SLOTS && s->npin < PIN_SLOTS * 3 / 4) s->pin[i] = key, s->npin++;
            else if (!s->pin_full) {
                s->pin_full = 1;
                VKB_WARN("BC budget: memory requirements asked for %u image shapes before they were made; no image of this device "
                         "is decoded over VKBRIDGE_BC_TX_BUDGET any more", s->npin);
            }
        }
    }
    hit = hit || s->pin_full;
    pthread_mutex_unlock(&s->lock);
    return hit;
}

static struct {
    pthread_mutex_t lock;
    int started;
    double tokens;          /* million texels (BC7 -> ASTC equivalents) in the bucket */
    uint64_t t_ns, win_ns;  /* the last refill; the start of the per-second line's window */
    uint32_t n_tx, n_fb, n_cap, n_pin, n_fast;
    double mtex_tx, mtex_fb;
    uint64_t fb_total, tx_total, cap_total, fast_total;
    double mtex_fb_total, mtex_tx_total;
} g_bud = {PTHREAD_MUTEX_INITIALIZER};
static int64_t g_fb_live;     /* bytes decoded images over the budget cost over transcoded ones, all devices (atomic) */
static __thread int64_t tls_fb; /* ... of the image being created, for vkb_emu_bcn_image_made */
static __thread int tls_fast;   /* ... it is to be encoded fast everywhere (auto) */

/* 1: transcode the image to t (*fast: with the fast search at every level); 0: decode it (over the budget). */
static int budget_take(vkb_emu_device *e, const VkImageCreateInfo *ci, const VkImageFormatListCreateInfo *fl, VkFormat t, int query)
{
    bcn_state *s = e->bcn;
    budget_cfg c;
    tls_fast = 0; /* an earlier image's, if its creation ended before vkb_emu_bcn_image_made */
    if (!budget_get(s, &c)) return 1;
    uint64_t key = shape_key(ci);
    if (query) {
        pinned(s, key, 1);
        return 1;
    }
    int txfam = tx_family(family(ci->format), t);
    /* BC1 -> ASTC direct has no search: it counts, but is never decoded (that would cost 2x its memory for little) */
    int pin = txfam == TX_BC1_ASTC || pinned(s, key, 0);
    double w = tx_weight(txfam);
    double cost = c.is_auto && txfam != TX_BC1_ASTC ? image_cost_q(s, ci, 0) * w / 1e6 : (double)image_texels(ci) * w / 1e6;
    double cost_fast = c.is_auto && s->auto_fast && txfam != TX_BC1_ASTC ? image_cost_q(s, ci, 1) * w / 1e6 : cost;
    int64_t extra = fallback_bytes(s, ci, fl, t), cap = fallback_cap(s, c.is_auto);
    uint64_t now = vkb_hitch_now();
    int tx = 1, fast = 0;
    pthread_mutex_lock(&g_bud.lock);
    if (!g_bud.started) {
        g_bud.started = 1, g_bud.tokens = c.burst, g_bud.t_ns = g_bud.win_ns = now;
        if (c.is_auto)
            VKB_INFO("BC budget: auto for %s: transcoding %.1f million texels a second, %.1f at once (BC7 -> ASTC 4x4 equivalents, the "
                     "default search counted %.0fx); short of it, images are %sdecoded, up to %lld MB more memory "
                     "(VKBRIDGE_BC_TX_BUDGET=0 turns it off, a number sets it)", s->auto_why, c.rate, c.burst, AUTO_DEFAULT_COST,
                     s->auto_fast ? "encoded with the fast search at every level when that fits, else " : "", (long long)(cap >> 20));
        else
            VKB_INFO("BC budget: transcoding %.1f million texels a second, %.1f at once (BC7 -> ASTC 4x4 equivalents); images over it are "
                     "decoded, up to %lld MB more memory (VKBRIDGE_BC_TX_BUDGET, VKBRIDGE_BC_TX_FALLBACK_MB)", c.rate, c.burst, (long long)(cap >> 20));
    }
    g_bud.tokens += c.rate * (double)(now - g_bud.t_ns) / 1e9;
    if (g_bud.tokens > c.burst) g_bud.tokens = c.burst;
    g_bud.t_ns = now;
    if (now - g_bud.win_ns >= 1000000000ull) {
        if (g_bud.n_fb || g_bud.n_cap || g_bud.n_fast)
            VKB_INFO("BC budget: in the last %.1f s %u image(s) (%.1f Mtexels) decoded over VKBRIDGE_BC_TX_BUDGET, %u (%.1f) transcoded "
                     "(%u of them with the fast search everywhere); %u transcoded past the decoded images' memory cap, %u of shapes the app "
                     "asked memory requirements for; decoded images cost %lld MB of %lld more",
                     (double)(now - g_bud.win_ns) / 1e9, g_bud.n_fb, g_bud.mtex_fb, g_bud.n_tx, g_bud.mtex_tx, g_bud.n_fast, g_bud.n_cap,
                     g_bud.n_pin, (long long)(__atomic_load_n(&g_fb_live, __ATOMIC_RELAXED) >> 20), (long long)(cap >> 20));
        g_bud.win_ns = now, g_bud.n_tx = g_bud.n_fb = g_bud.n_cap = g_bud.n_pin = g_bud.n_fast = 0, g_bud.mtex_tx = g_bud.mtex_fb = 0;
    }
    if (pin) g_bud.n_pin += txfam != TX_BC1_ASTC;
    else if (g_bud.tokens >= cost || g_bud.tokens >= c.burst * 0.999) {
    } else if (cost_fast < cost && g_bud.tokens >= cost_fast) fast = 1, cost = cost_fast;
    else if (extra <= 0 || __atomic_load_n(&g_fb_live, __ATOMIC_RELAXED) + extra > cap) g_bud.n_cap++, g_bud.cap_total++;
    else tx = 0;
    if (tx) {
        g_bud.tokens -= cost;
        if (g_bud.tokens < -c.burst) g_bud.tokens = -c.burst;
        g_bud.n_tx++, g_bud.mtex_tx += cost, g_bud.tx_total++, g_bud.mtex_tx_total += cost;
        if (fast) g_bud.n_fast++, g_bud.fast_total++, tls_fast = 1;
    } else {
        g_bud.n_fb++, g_bud.mtex_fb += cost, g_bud.fb_total++, g_bud.mtex_fb_total += cost;
        __atomic_add_fetch(&g_fb_live, extra, __ATOMIC_RELAXED);
        tls_fb += extra;
    }
    pthread_mutex_unlock(&g_bud.lock);
    return tx;
}

/* At a device's end: what the budget did since the server started (all devices). */
static void budget_totals(void)
{
    pthread_mutex_lock(&g_bud.lock);
    if (g_bud.started)
        VKB_INFO("BC budget: so far %llu image(s) (%.1f Mtexels) transcoded (%llu with the fast search everywhere), %llu (%.1f) decoded over "
                 "VKBRIDGE_BC_TX_BUDGET, %llu transcoded past the decoded images' memory cap; decoded images alive cost %lld MB more",
                 (unsigned long long)g_bud.tx_total, g_bud.mtex_tx_total, (unsigned long long)g_bud.fast_total,
                 (unsigned long long)g_bud.fb_total, g_bud.mtex_fb_total, (unsigned long long)g_bud.cap_total,
                 (long long)(__atomic_load_n(&g_fb_live, __ATOMIC_RELAXED) >> 20));
    pthread_mutex_unlock(&g_bud.lock);
}

/* A transcode done outside budget_take (a copy re-encoding a decoded image's texels): its texels
 * come off the bucket all the same. */
static void budget_charge(const bcn_state *s, int txfam, uint64_t texels)
{
    budget_cfg c;
    if (!budget_get(s, &c)) return;
    double cost = (double)texels * tx_weight(txfam) / 1e6;
    pthread_mutex_lock(&g_bud.lock);
    if (g_bud.started) {
        g_bud.tokens -= cost;
        if (g_bud.tokens < -c.burst) g_bud.tokens = -c.burst;
        g_bud.mtex_tx += cost, g_bud.mtex_tx_total += cost;
    }
    pthread_mutex_unlock(&g_bud.lock);
}

/* ------------------------------------------------------------------ copies, counted */

/* Copies between BC images held differently (copy_mixed), and BC copies (commands) not done, all
 * devices: a line every 10 s while they change, and at each device's end. */
enum { CP_BLIT, CP_ENC, CP_SKIP_MIXED, CP_SKIP_BLOCKS, CP_SKIP_OTHER, CP_SKIP_EDGE, CP_SKIP_IN, CP_SKIP_READBACK, CP_COUNT };
static uint64_t g_cp[CP_COUNT], g_cp_told[CP_COUNT], g_cp_ns;

static void copies_line(int end)
{
    uint64_t v[CP_COUNT], skip = 0;
    int changed = 0;
    for (int i = 0; i < CP_COUNT; i++) {
        v[i] = __atomic_load_n(&g_cp[i], __ATOMIC_RELAXED);
        changed |= v[i] != __atomic_load_n(&g_cp_told[i], __ATOMIC_RELAXED);
        if (i >= CP_SKIP_MIXED) skip += v[i];
    }
    if (!changed && !(end && (v[CP_BLIT] || v[CP_ENC] || skip))) return;
    for (int i = 0; i < CP_COUNT; i++) __atomic_store_n(&g_cp_told[i], v[i], __ATOMIC_RELAXED);
    VKB_INFO("BC copies%s: %llu from a transcoded into a decoded image (blit), %llu from a decoded into a transcoded image (re-encoded); "
             "%llu skipped (%llu transcoded-decoded that could not be done, %llu between transcoded images of other block sizes, "
             "%llu BC-uncompressed or 16-bit-sRGB, %llu off ASTC block edges, %llu into BC from other formats, %llu BC readbacks)",
             end ? " so far" : " (running totals)", (unsigned long long)v[CP_BLIT], (unsigned long long)v[CP_ENC],
             (unsigned long long)skip, (unsigned long long)v[CP_SKIP_MIXED], (unsigned long long)v[CP_SKIP_BLOCKS],
             (unsigned long long)v[CP_SKIP_OTHER], (unsigned long long)v[CP_SKIP_EDGE], (unsigned long long)v[CP_SKIP_IN],
             (unsigned long long)v[CP_SKIP_READBACK]);
}

static void copy_count(int what)
{
    __atomic_add_fetch(&g_cp[what], 1, __ATOMIC_RELAXED);
    uint64_t now = vkb_hitch_now(), last = __atomic_load_n(&g_cp_ns, __ATOMIC_RELAXED);
    if ((!last || now - last >= 10000000000ull) && __atomic_compare_exchange_n(&g_cp_ns, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        copies_line(0);
}

void vkb_emu_bcn_image_made(vkb_emu_device *e, vkb_emu_image *r)
{
    (void)e;
    int64_t b = tls_fb;
    int fast = tls_fast;
    tls_fb = 0, tls_fast = 0;
    if (r && fast) r->bc_fast = 1;
    if (!b) return;
    if (r) r->bc_fallback += b;
    else __atomic_sub_fetch(&g_fb_live, b, __ATOMIC_RELAXED);
}

static VkFormat tx_format(vkb_emu_device *e, const VkImageCreateInfo *ci, const VkImageFormatListCreateInfo *fl, int query)
{
    const bcn_state *s = e->bcn;
    if (!s) return VK_FORMAT_UNDEFINED;
    VkFormat t = s->tx[BC_INDEX(ci->format)];
    const VkImageUsageFlags texture = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* COLOR_ATTACHMENT is left out of the test: no BC image can be rendered to in Vulkan, and Zink (every
     * OpenGL game) puts it on every texture with the EXTENDED_USAGE / MUTABLE flags (all 16 logged
     * BC images decoded, 620 MB of BC held as 3.1 GB). STORAGE still decodes. The transcoded image
     * is made without it (vkb_emu_image_create_info). */
    const VkImageUsageFlags ignored = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!t || (ci->usage & ~(texture | ignored))) return VK_FORMAT_UNDEFINED;
    /* VKBRIDGE_BC_TRANSCODE_MAX=<texels>: images wider or taller than that are decoded. A large
     * texture streamed in mid-game is one transcode of hundreds of ms in the frame's command
     * stream (Dark Souls III froze 300-700 ms while walking on the G925); decoding it is cheap,
     * at 2-4x its memory. Small images, most of a game's count, stay transcoded. */
    const char *mx = vkb_srv_env("VKBRIDGE_BC_TRANSCODE_MAX"); /* <cache dir>/env and presets too */
    long max = mx ? atol(mx) : 0;
    if (max > 0 && (ci->extent.width > (uint32_t)max || ci->extent.height > (uint32_t)max)) return VK_FORMAT_UNDEFINED;
    /* VKBRIDGE_BC_DECODE=<VkFormat>[,...] (a diagnostic): these BC formats decoded, the rest transcoded */
    const char *dv = vkb_srv_env("VKBRIDGE_BC_DECODE");
    for (const char *p = dv; p && *p;) {
        if (atol(p) == (long)ci->format) return VK_FORMAT_UNDEFINED;
        p = strchr(p, ',');
        if (p) p++;
    }
    if ((ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && fl)
        for (uint32_t i = 0; i < fl->viewFormatCount; i++) {
            VkFormat v = fl->pViewFormats[i];
            if (!vkb_is_bc(v) || tx_class(s->tx[BC_INDEX(v)]) != tx_class(t)) return VK_FORMAT_UNDEFINED;
        }
    const vkb_dispatch *r = &e->dev->real;
    VkImageFormatProperties ip;
    VkImageCreateFlags flags = ci->flags & ~(VkImageCreateFlags)(VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
    if (!r->vkGetPhysicalDeviceImageFormatProperties ||
        r->vkGetPhysicalDeviceImageFormatProperties(e->dev->physical, t, ci->imageType, ci->tiling,
                                                    (ci->usage & ~ignored) | VK_IMAGE_USAGE_TRANSFER_DST_BIT, flags, &ip) != VK_SUCCESS ||
        ci->extent.width > ip.maxExtent.width || ci->extent.height > ip.maxExtent.height || ci->extent.depth > ip.maxExtent.depth ||
        ci->mipLevels > ip.maxMipLevels || ci->arrayLayers > ip.maxArrayLayers) {
        VKB_ONCE(VKB_LOG_INFO, "BC emulation: the GPU takes no %ux%ux%u image of type %d in format %d; BC images like it are decoded "
                 "(logged once)", ci->extent.width, ci->extent.height, ci->extent.depth, ci->imageType, t);
        return VK_FORMAT_UNDEFINED;
    }
    return budget_take(e, ci, fl, t, query) ? t : VK_FORMAT_UNDEFINED;
}

void vkb_emu_image_create_info(vkb_emu_device *e, VkImageCreateInfo *ci, int query)
{
    if (!(e->flags & VKB_EMU_BCN) || !vkb_is_bc(ci->format)) return;
    const VkFormat client = ci->format;
    /* The image keeps the meaning of the app's format (sRGB, SNORM) so blits and copies convert as
     * they would, and nothing but the app's usage plus TRANSFER_DST for the decoded uploads. A
     * mutable image is made mutable only among the decoded formats its views can map to (a format
     * list, which lets Mali keep AFBC where blanket MUTABLE_FORMAT may not); one format, not mutable. */
    static __thread VkFormat list[8];
    static __thread VkImageFormatListCreateInfo own;
    VkImageFormatListCreateInfo *fl = NULL;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) fl = (VkImageFormatListCreateInfo *)b;
    VkFormat tx = tx_format(e, ci, fl, query);
    VkFormat packed = tx ? VK_FORMAT_UNDEFINED : packed_format(e->bcn, ci, fl);
    VkFormat view = tx ? tx : packed ? packed : vkb_bc_view_format(ci->format);
    uint32_t n = 0;
    list[n++] = view;
    if (ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
        /* Views in other BC formats become views in their decoded (transcoded) formats; uncompressed
         * views (BLOCK_TEXEL_VIEW_COMPATIBLE) become views in the image's (emu_CreateImageView). */
        uint32_t m = fl && fl->viewFormatCount ? fl->viewFormatCount : 1;
        for (uint32_t i = 0; i < m; i++) {
            VkFormat f = fl && fl->viewFormatCount ? fl->pViewFormats[i] : sibling(view);
            if (vkb_is_bc(f)) f = tx ? ((bcn_state *)e->bcn)->tx[BC_INDEX(f)] : packed ? packed : vkb_bc_view_format(f);
            else if (fl && fl->viewFormatCount) f = view;
            uint32_t k = 0;
            while (k < n && list[k] != f) k++;
            if (k == n && n < sizeof(list) / sizeof(list[0])) list[n++] = f;
        }
    }
    if (e->bcn) {
        prebuild(e, tx ? tx_family(family(client), tx) : packed ? FAM_BC1_16 : family(client));
        /* decoded where its format is transcoded: a copy between it and a transcoded image re-encodes */
        VkFormat t = ((bcn_state *)e->bcn)->tx[BC_INDEX(client)];
        if (!tx && t && !query && raw_family(t) >= 0) prebuild_later(e, raw_family(t));
    }
    ci->format = view;
    ci->flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    if (tx) { /* ETC2 / EAC / ASTC are not colour attachments; the extended usage was for that and uncompressed views */
        ci->usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci->flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    }
    if (n == 1) ci->flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci->usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fl) {
        fl->viewFormatCount = n;
        fl->pViewFormats = list;
    } else if (n > 1) {
        own = (VkImageFormatListCreateInfo){VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, ci->pNext, n, list};
        ci->pNext = &own;
    }
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 16)
        VKB_INFO("BC emulation: image %ux%ux%u (%u mips, %u layers) %s as format %d, usage 0x%x%s, flags 0x%x, %u view format%s",
                 ci->extent.width, ci->extent.height, ci->extent.depth, ci->mipLevels, ci->arrayLayers, tx ? "transcoded" : "decoded", view, ci->usage,
                 (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? " (STORAGE, the app's)" : "", ci->flags, n, n == 1 ? "" : "s");
}

/* Blocks drawn into uncompressed views. Zink renders BC blocks into uncompressed views of its BC images: Mesa's
 * st_CompressedTexSubImage with a pixel buffer bound draws them, one texel a block, into an R32G32B32A32_UINT
 * (16-byte blocks) or R16G16B16A16_UINT (8-byte blocks) view of the level. Neither a transcoded image (ETC2 / EAC /
 * ASTC: no COLOR_ATTACHMENT, no such view) nor a decoded one (its texels are colours, not blocks) can take that draw,
 * so such a view is a view of a sink: an image of the level's blocks (one texel a block, the view's format, the
 * blocks of that level exactly), made when the first such view of the level is. When the render pass (or rendering)
 * that drew into it ends, what was drawn - the renderArea, bounded by the viewports set and the rects cleared in it
 * - is transcoded (decoded) from the sink into the texture, as an upload of those blocks would be (sink_writeback).
 * SOMA's terrain was black on v0.5.5.2, where the draw stayed in the sink, and garbled on v0.5.5.1
 *, where it went into the decoded image as colours. A view the app only samples stays a view of the image. */
typedef struct bc_sink {
    VkFormat format;
    uint32_t level;          /* the BC image's level whose blocks it holds */
    uint32_t bw, bh, layers; /* blocks of that level; the image's layers */
    VkImage image;
    VkDeviceMemory mem;
    struct bc_sink *next;
} bc_sink;

/* A view the app made of a BC image that is a view of a sink, until the app destroys it (or the image goes). */
typedef struct sink_view {
    VkImageView view;
    VkImage bc;
    bc_sink *sink;
    uint32_t layer0, layers;
    struct sink_view *next;
} sink_view;

typedef struct rp_att {
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    VkImageLayout initial, final;
} rp_att;

typedef struct rp_info { /* the attachments of a render pass the app made */
    VkRenderPass rp;
    uint32_t n;
    rp_att *att;
    struct rp_info *next;
} rp_info;

typedef struct fb_info { /* a framebuffer with a sink view among its attachments */
    VkFramebuffer fb;
    uint32_t n;
    VkImageView *views;
    struct fb_info *next;
} fb_info;

#define SK_BUCKETS 64
#define SK_HASH(h) ((uint32_t)(((uint64_t)(h) >> 4) ^ ((uint64_t)(h) >> 12)) & (SK_BUCKETS - 1))

/* Per device, alive until the device is (vkb_emu_bcsink_free): the hooks below chain to `next` even after the BC
 * state is gone (the dynamic rendering emulation destroys its framebuffers through them at the end). */
typedef struct bcsink {
    pthread_mutex_t lock;
    uint32_t nviews; /* atomic: sink views alive; 0: every hook passes straight through */
    uint32_t nfbs;   /* atomic: fb_info entries */
    sink_view *views[SK_BUCKETS];
    rp_info *rps[SK_BUCKETS];
    fb_info *fbs[SK_BUCKETS];
    PFN_vkCreateRenderPass create_rp;
    PFN_vkCreateRenderPass2 create_rp2;
    PFN_vkDestroyRenderPass destroy_rp;
    PFN_vkCreateFramebuffer create_fb;
    PFN_vkDestroyFramebuffer destroy_fb;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkCmdBeginRenderPass begin_rp;
    PFN_vkCmdBeginRenderPass2 begin_rp2;
    PFN_vkCmdEndRenderPass end_rp;
    PFN_vkCmdEndRenderPass2 end_rp2;
    PFN_vkCmdBeginRendering begin_r;
    PFN_vkCmdEndRendering end_r;
    PFN_vkCmdSetViewport set_vp;
    PFN_vkCmdSetViewportWithCount set_vp_count;
    PFN_vkCmdClearAttachments clear_att;
} bcsink;

#define SINK_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

static void sink_free(vkb_emu_device *e, bc_sink *k)
{
    e->dev->real.vkDestroyImage(e->dev->device, k->image, NULL);
    e->dev->real.vkFreeMemory(e->dev->device, k->mem, NULL);
    free(k);
}

static bc_sink *sink_make(vkb_emu_device *e, const vkb_emu_image *r, VkFormat format, uint32_t level)
{
    bcn_state *s = e->bcn;
    const vkb_dispatch *d = &e->dev->real;
    uint32_t w = r->extent.width >> level, h = r->extent.height >> level;
    w = ((w ? w : 1) + 3) / 4;
    h = ((h ? h : 1) + 3) / 4;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, {w, h, 1}, 1, r->layers,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, SINK_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    bc_sink *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    k->format = format;
    k->level = level;
    k->bw = w;
    k->bh = h;
    k->layers = r->layers;
    if (d->vkCreateImage(e->dev->device, &ci, NULL, &k->image) != VK_SUCCESS) {
        free(k);
        return NULL;
    }
    VkMemoryRequirements req;
    d->vkGetImageMemoryRequirements(e->dev->device, k->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < s->mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (s->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    for (uint32_t i = 0; i < s->mp.memoryTypeCount && type == UINT32_MAX; i++)
        if (req.memoryTypeBits & (1u << i)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || d->vkAllocateMemory(e->dev->device, &ai, NULL, &k->mem) != VK_SUCCESS) {
        d->vkDestroyImage(e->dev->device, k->image, NULL);
        free(k);
        return NULL;
    }
    if (d->vkBindImageMemory(e->dev->device, k->image, k->mem, 0) != VK_SUCCESS) {
        sink_free(e, k);
        return NULL;
    }
    return k;
}

/* The sink for `format` and `level` of image r (made now if there is none), NULL: none can be made. */
static bc_sink *sink_get(vkb_emu_device *e, vkb_emu_image *r, VkFormat format, uint32_t level)
{
    pthread_mutex_lock(&e->lock);
    bc_sink *k = r->sinks;
    while (k && (k->format != format || k->level != level)) k = k->next;
    pthread_mutex_unlock(&e->lock);
    if (k) return k;
    bc_sink *n = sink_make(e, r, format, level);
    if (!n) return NULL;
    pthread_mutex_lock(&e->lock);
    for (k = r->sinks; k && (k->format != format || k->level != level); k = k->next) {}
    if (!k) {
        n->next = r->sinks;
        r->sinks = k = n;
        n = NULL;
    }
    pthread_mutex_unlock(&e->lock);
    if (n) sink_free(e, n);
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
        VKB_INFO("BC emulation: an uncompressed view (format %d) of a %s BC image the app may render to: it is a view of an image of the "
                 "blocks of level %u (%ux%u); what is drawn there goes into the texture when the render pass ends (logged for the first few)",
                 format, vkb_tx_block_bytes(r->format) ? "transcoded" : "decoded", level, k->bw, k->bh);
    return k;
}

static void sink_view_add(vkb_emu_device *e, VkImageView v, VkImage bc, bc_sink *k, uint32_t layer0, uint32_t layers)
{
    bcsink *g = e->bcsink;
    sink_view *n = g ? calloc(1, sizeof(*n)) : NULL;
    if (!n) return;
    *n = (sink_view){v, bc, k, layer0, layers, NULL};
    pthread_mutex_lock(&g->lock);
    n->next = g->views[SK_HASH(v)];
    g->views[SK_HASH(v)] = n;
    __atomic_add_fetch(&g->nviews, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g->lock);
}

/* Takes the sink views of view v (VK_NULL_HANDLE: every one of image bc) out of the register. */
static void sink_views_drop(bcsink *g, VkImageView v, VkImage bc)
{
    if (!g || !__atomic_load_n(&g->nviews, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&g->lock);
    for (uint32_t b = v ? SK_HASH(v) : 0; b < (v ? SK_HASH(v) + 1 : SK_BUCKETS); b++)
        for (sink_view **pp = &g->views[b]; *pp;) {
            sink_view *x = *pp;
            if (v ? x->view == v : x->bc == bc) {
                *pp = x->next;
                free(x);
                __atomic_sub_fetch(&g->nviews, 1, __ATOMIC_RELAXED);
            } else {
                pp = &x->next;
            }
        }
    pthread_mutex_unlock(&g->lock);
}

/* A copy of the sink view v's record (lock held); 0: v is none. */
static int sink_view_find(bcsink *g, VkImageView v, sink_view *out)
{
    for (sink_view *x = g->views[SK_HASH(v)]; x; x = x->next)
        if (x->view == v) {
            *out = *x;
            return 1;
        }
    return 0;
}

void vkb_emu_image_release(vkb_emu_device *e, vkb_emu_image *img)
{
    if (img->sinks) sink_views_drop(e->bcsink, VK_NULL_HANDLE, img->image);
    while (img->sinks) {
        bc_sink *k = img->sinks;
        img->sinks = k->next;
        sink_free(e, k);
    }
    if (img->bc_fallback) __atomic_sub_fetch(&g_fb_live, img->bc_fallback, __ATOMIC_RELAXED);
}

VkFormat vkb_emu_bcn_view_format(vkb_emu_device *e, VkImage image, VkFormat requested)
{
    if (!(e->flags & VKB_EMU_BCN)) return requested;
    vkb_emu_image *r = vkb_emu_find_image(e, image);
    int bc = r && vkb_is_bc(r->client_format);
    /* A 16-bit image has no other view formats (packed_format); a transcoded one only those of its
     * class (tx_format). */
    if (vkb_is_bc(requested) && bc && vkb_tx_block_bytes(r->format)) {
        const bcn_state *s = e->bcn;
        VkFormat t = s ? s->tx[BC_INDEX(requested)] : VK_FORMAT_UNDEFINED;
        return tx_class(t) == tx_class(r->format) ? t : r->format;
    }
    if (vkb_is_bc(requested)) return bc && vkb_bc_is_packed(r->format) ? r->format : vkb_bc_view_format(requested);
    if (bc) {
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: an uncompressed view (format %d) of a BC image shows decoded texels (logged once)", requested);
        return r->format;
    }
    return requested;
}

static uint32_t block_texel_bytes(VkFormat f);

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                         const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    VkImageViewCreateInfo *v = (VkImageViewCreateInfo *)ci;
    vkb_emu_device *e = vkb_emu_cur();
    vkb_emu_image *r = (e->flags & VKB_EMU_BCN) && !vkb_is_bc(ci->format) ? vkb_emu_find_image(e, ci->image) : NULL;
    if (r && vkb_is_bc(r->client_format)) {
        /* An uncompressed view of a BC image: what the view may be used as is the app's image usage, or its
         * VkImageViewUsageCreateInfo's. One the app may render to, of one level of a 2D image, in a format a block
         * is a texel of, is a view of the level's sink (see bc_sink); a transcoded image takes no other view it
         * could render to or store into either (the sink, as before, without the way back into the texture for
         * storage). A view that is only sampled stays a view of the image. */
        int txd = vkb_tx_block_bytes(r->format) != 0;
        VkImageViewUsageCreateInfo *vu = NULL;
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) vu = (VkImageViewUsageCreateInfo *)b;
        VkImageUsageFlags want = vu ? vu->usage : r->client_usage;
        uint32_t level = ci->subresourceRange.baseMipLevel, levels = ci->subresourceRange.levelCount;
        if (levels == VK_REMAINING_MIP_LEVELS) levels = r->mips > level ? r->mips - level : 0;
        int writes = (want & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || (txd && (want & VK_IMAGE_USAGE_STORAGE_BIT));
        if (writes && r->type == VK_IMAGE_TYPE_2D && levels == 1 && level < r->mips &&
            (ci->viewType == VK_IMAGE_VIEW_TYPE_2D || ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) &&
            block_texel_bytes(ci->format) == vkb_bc_block_bytes(r->client_format)) {
            bc_sink *k = sink_get(e, r, ci->format, level);
            if (k) {
                VkImageViewUsageCreateInfo use = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                                  (want & SINK_USAGE) ? (want & SINK_USAGE) : VK_IMAGE_USAGE_SAMPLED_BIT};
                VkImageViewCreateInfo sv = *ci;
                sv.pNext = &use;
                sv.image = k->image;
                sv.subresourceRange.baseMipLevel = 0;
                sv.subresourceRange.levelCount = 1;
                uint32_t layer0 = sv.subresourceRange.baseArrayLayer, layers = sv.subresourceRange.layerCount;
                if (layers == VK_REMAINING_ARRAY_LAYERS) layers = k->layers > layer0 ? k->layers - layer0 : 1;
                VkResult res = vkb_emu_real()->vkCreateImageView(device, &sv, pAllocator, pView);
                if (res == VK_SUCCESS) sink_view_add(e, *pView, r->image, k, layer0, layers);
                return res;
            }
        }
        if (txd) {
            /* a view only sampled (or no stand-in could be made): the usage the image has */
            if (vu) vu->usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (vu && !vu->usage) vu->usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        }
    }
    v->format = vkb_emu_bcn_view_format(e, ci->image, ci->format);
    return vkb_emu_real()->vkCreateImageView(device, v, pAllocator, pView);
}

void vkb_emu_bcn_buffer_usage(vkb_emu_device *e, VkBufferCreateInfo *ci)
{
    /* The decoder reads uploads as a storage buffer. */
    if ((e->flags & VKB_EMU_BCN) && (ci->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) ci->usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
}

/* ------------------------------------------------------------------ scratch */

static void chunk_free(vkb_emu_device *e, chunk *c)
{
    e->dev->real.vkDestroyBuffer(e->dev->device, c->buf, NULL);
    e->dev->real.vkFreeMemory(e->dev->device, c->mem, NULL);
}

static void chunk_destroy(vkb_emu_device *e, chunk *c)
{
    chunk_free(e, c);
    __atomic_sub_fetch(&g_live, c->size, __ATOMIC_RELAXED);
}

static int chunk_create(vkb_emu_device *e, VkDeviceSize size, chunk *out)
{
    bcn_state *s = e->bcn;
    const vkb_dispatch *r = &e->dev->real;
    VkDevice d = e->dev->device;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (r->vkCreateBuffer(d, &bci, NULL, &out->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    r->vkGetBufferMemoryRequirements(d, out->buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < s->mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (s->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    for (uint32_t i = 0; i < s->mp.memoryTypeCount && type == UINT32_MAX; i++)
        if (req.memoryTypeBits & (1u << i)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || r->vkAllocateMemory(d, &ai, NULL, &out->mem) != VK_SUCCESS) {
        r->vkDestroyBuffer(d, out->buf, NULL);
        return 0;
    }
    if (r->vkBindBufferMemory(d, out->buf, out->mem, 0) != VK_SUCCESS) {
        chunk_free(e, out);
        return 0;
    }
    out->size = size;
    __atomic_add_fetch(&g_live, size, __ATOMIC_RELAXED);
    return 1;
}

static VkDeviceSize chunk_size(VkDeviceSize want)
{
    VkDeviceSize size = CHUNK_MIN;
    while (size < want && size < CHUNK_MAX) size <<= 1;
    return size < want ? want : size;
}

/* A chunk for `need` bytes, sized for `want` when it may be: the smallest idle one that fits,
 * else a new one if live scratch stays under LIVE_MAX - or regardless when `force` (the command
 * buffer has none to reuse), then only `need` large. 1: got one; 0: out of memory; -1: at the cap,
 * the caller reuses what its command buffer holds. */
static int chunk_get(vkb_emu_device *e, VkDeviceSize need, VkDeviceSize want, int force, chunk *out)
{
    bcn_state *s = e->bcn;
    pthread_mutex_lock(&s->lock);
    uint32_t best = UINT32_MAX;
    for (int pass = 0; pass < 2 && best == UINT32_MAX; pass++) {
        VkDeviceSize min = pass ? need : want;
        for (uint32_t i = 0; i < s->nidle; i++)
            if (s->idle[i].size >= min && (best == UINT32_MAX || s->idle[i].size < s->idle[best].size)) best = i;
    }
    if (best != UINT32_MAX) {
        *out = s->idle[best];
        s->idle[best] = s->idle[--s->nidle];
        __atomic_sub_fetch(&g_idle, out->size, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&s->lock);
    if (best != UINT32_MAX) return 1;
    VkDeviceSize size = chunk_size(want), live = __atomic_load_n(&g_live, __ATOMIC_RELAXED);
    if (live + size > LIVE_MAX) {
        static int told;
        if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
            VKB_INFO("BC emulation: decode scratch at its %llu MiB cap (%llu KiB live, %llu KiB idle, %llu KiB more asked); command "
                     "buffers now reuse their own scratch (logged once)",
                     (unsigned long long)(LIVE_MAX >> 20), (unsigned long long)(live >> 10),
                     (unsigned long long)(__atomic_load_n(&g_idle, __ATOMIC_RELAXED) >> 10), (unsigned long long)(size >> 10));
        if (!force) return -1;
        size = chunk_size(need);
    }
    if (chunk_create(e, size, out)) return 1;
    VKB_ONCE(VKB_LOG_ERROR, "BC emulation: no memory for a %llu-byte decode buffer (logged once)", (unsigned long long)size);
    return 0;
}

/* Room in the process-wide idle budget for `size` bytes (taken if there is). */
static int idle_take(VkDeviceSize size)
{
    VkDeviceSize cur = __atomic_load_n(&g_idle, __ATOMIC_RELAXED);
    do {
        if (cur + size > IDLE_MAX) return 0;
    } while (!__atomic_compare_exchange_n(&g_idle, &cur, cur + size, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return 1;
}

/* A chunk a command buffer is done with: idle if the budget has room, else destroyed. */
static void chunk_release(vkb_emu_device *e, chunk *k)
{
    bcn_state *s = e->bcn;
    int keep = 0;
    if (s && idle_take(k->size)) {
        pthread_mutex_lock(&s->lock);
        if (s->nidle == s->capidle) {
            uint32_t cap = s->capidle ? s->capidle * 2 : 8;
            chunk *ni = realloc(s->idle, cap * sizeof(*ni));
            if (ni) {
                s->idle = ni;
                s->capidle = cap;
            }
        }
        if (s->nidle < s->capidle) {
            s->idle[s->nidle++] = *k;
            keep = 1;
        }
        pthread_mutex_unlock(&s->lock);
        if (!keep) __atomic_sub_fetch(&g_idle, k->size, __ATOMIC_RELAXED);
    }
    if (!keep) chunk_destroy(e, k);
}

void vkb_emu_bcn_cb_release(vkb_emu_device *e, void *cb_scratch)
{
    bcn_cb *c = cb_scratch;
    if (!c) return;
    for (uint32_t i = 0; i < c->n; i++) chunk_release(e, &c->held[i]);
    for (uint32_t i = 0; i < c->nstage; i++) chunk_release(e, &c->stage[i]);
    free(c->held);
    free(c->stage);
    memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------------ decoder */

static const uint32_t *const fam_code[FAM_COUNT] = {spv_bc1, spv_bc2, spv_bc3, spv_bc4, spv_bc5, spv_bc6h, spv_bc7, spv_bc1_16,
                                                    spv_bc1_etc2, spv_bc2_astc, spv_bc3_astc, spv_bc4_eac, spv_bc5_eac, spv_bc6h_astc,
                                                    spv_bc7_astc, spv_bc2_astc6, spv_bc3_astc6, spv_bc7_astc6, spv_bc2_astc8,
                                                    spv_bc3_astc8, spv_bc7_astc8, spv_bc1_astc, spv_raw_etc2, spv_raw_astc,
                                                    spv_raw_eac_r, spv_raw_eac_rg, spv_raw_astc_hdr, spv_raw_astc6, spv_raw_astc8};
static const size_t fam_size[FAM_COUNT] = {sizeof(spv_bc1), sizeof(spv_bc2), sizeof(spv_bc3), sizeof(spv_bc4), sizeof(spv_bc5),
                                           sizeof(spv_bc6h), sizeof(spv_bc7), sizeof(spv_bc1_16), sizeof(spv_bc1_etc2),
                                           sizeof(spv_bc2_astc), sizeof(spv_bc3_astc), sizeof(spv_bc4_eac), sizeof(spv_bc5_eac),
                                           sizeof(spv_bc6h_astc), sizeof(spv_bc7_astc), sizeof(spv_bc2_astc6), sizeof(spv_bc3_astc6),
                                           sizeof(spv_bc7_astc6), sizeof(spv_bc2_astc8), sizeof(spv_bc3_astc8), sizeof(spv_bc7_astc8), sizeof(spv_bc1_astc),
                                           sizeof(spv_raw_etc2), sizeof(spv_raw_astc), sizeof(spv_raw_eac_r), sizeof(spv_raw_eac_rg),
                                           sizeof(spv_raw_astc_hdr), sizeof(spv_raw_astc6), sizeof(spv_raw_astc8)};

/* Builds the decoder or transcoder for fam at quality q, through the device's persistent pipeline
 * cache (server_pcache.c): Mali takes hundreds of ms over some transcoders, and without the cache
 * every process paid that again. */
/* The transcoders' search, BC_QUALITY (specialization constant 0, default 1), written into a copy of
 * the code as that constant's default instead of passed as a specialization: Mali compilers have
 * taken specializations wrong (the spec_select probe: DXVK's guard folded at the defaults), and on
 * the Dimensity 9500's G1-Ultra (r54p1) every texture of 1024x1024 or more - the ones auto quality
 * gives the fast search, BC_QUALITY 0 - came out as blocks of noise while smaller ones were right
 * (Library of Ruina, Lobotomy Corporation; DroidDeck#218). With the value as the default the driver
 * has nothing to specialize. NULL: no constant 0 found (code returned unchanged then). */
static uint32_t *bake_quality(const uint32_t *code, size_t size, uint32_t q)
{
    size_t n = size / 4;
    uint32_t id = 0;
    for (size_t i = 5; i < n && !id;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len) return NULL;
        if (op == 71 /* OpDecorate */ && len >= 4 && code[i + 2] == 1 /* SpecId */ && code[i + 3] == 0) id = code[i + 1];
        i += len;
    }
    for (size_t i = 5; id && i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len) break;
        if (op == 50 /* OpSpecConstant */ && len == 4 && code[i + 2] == id) {
            uint32_t *w = malloc(size);
            if (!w) return NULL;
            memcpy(w, code, size);
            w[i + 3] = q;
            return w;
        }
        i += len;
    }
    return NULL;
}

static VkPipeline build(vkb_emu_device *e, int fam, uint32_t q, int bg)
{
    const vkb_dispatch *r = &e->dev->real;
    VkDevice d = e->dev->device;
    uint32_t *baked = fam >= TX_BC1 && q != 1 && fam_code[fam] ? bake_quality(fam_code[fam], fam_size[fam], q) : NULL;
    if (fam >= TX_BC1 && q != 1 && !baked) VKB_WARN("BC emulation: transcoder %d has no BC_QUALITY constant to set; built at its default", fam);
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, fam_size[fam], baked ? baked : fam_code[fam]};
    VkShaderModule m;
    VkResult mr = fam_code[fam] ? r->vkCreateShaderModule(d, &mci, NULL, &m) : VK_ERROR_INITIALIZATION_FAILED;
    free(baked);
    if (mr != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                  VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
    pci.layout = ((bcn_state *)e->bcn)->layout;
    VkPipeline p;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (vkb_pcache_create_compute(e->dev, &pci, &p) != VK_SUCCESS) {
        VKB_ERR("BC emulation: decoder pipeline %d (quality %u) failed to build", fam, q);
        p = VK_NULL_HANDLE;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    /* built on the first upload that needs it (or beside it, bg): a load-time cost worth seeing */
    if (p) VKB_INFO("BC emulation: %s pipeline %d%s built in %.1f ms%s", fam >= TX_BC1 ? "transcoder" : "decoder", fam,
                    fam < TX_BC1 ? "" : q == 0 ? " (fast)" : q == 2 ? " (high)" : " (default)",
                    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, bg ? " in the background" : "");
    r->vkDestroyShaderModule(d, m, NULL);
    return p;
}

static void *warm_main(void *arg);

/* What fam is built at: decoders have one variant; under auto a transcoder has two (fast, default). */
static uint32_t qualities(const bcn_state *s, int fam, uint32_t out[2])
{
    if (fam < TX_BC1) return out[0] = 1, 1;
    if (s->quality != QUALITY_AUTO) return out[0] = s->quality, 1;
    out[0] = 0, out[1] = 1;
    return 2;
}

/* Queues fam at q for the warm threads (started on first use). plock held. */
static void warm_queue(vkb_emu_device *e, int fam, uint32_t q, int urgent)
{
    bcn_state *s = e->bcn;
    if (s->pipes[q][fam] || s->building[q][fam] || s->failed[q][fam] || s->warm_stop) return;
    (urgent ? s->urgent : s->want)[q] |= 1u << fam;
    if (!s->nwarm) {
        for (int i = 0; i < WARM_THREADS; i++) {
            int r = pthread_create(&s->warm[s->nwarm], NULL, warm_main, e);
            if (r) VKB_WARN("BC emulation: cannot start a pipeline build thread (%d): pipelines are built at first use", r);
            else s->nwarm++;
        }
        if (s->nwarm) VKB_INFO("BC emulation: building the transcoders ahead on %d threads", s->nwarm);
        else s->warm_stop = 1; /* nothing will take the queue */
    }
    pthread_cond_broadcast(&s->pcond);
}

static void queue_family(vkb_emu_device *e, int fam, int urgent)
{
    uint32_t q[2], n = qualities(e->bcn, fam, q);
    for (uint32_t i = 0; i < n; i++) warm_queue(e, fam, q[i], urgent);
}

/* A BC image is being made as fam: its pipelines first; with the first one, every transcoder this
 * device may use, so none is compiled in the middle of an upload (Mali takes 0.3-0.95 s over the
 * BC1, BC2, BC3, BC6H and BC7 ones on the G720, at each quality, and its pipeline cache does not
 * spare that in a new process). */
static void prebuild(vkb_emu_device *e, int fam)
{
    bcn_state *s = e->bcn;
    uint32_t q[2], n = qualities(s, fam, q);
    int ready = __atomic_load_n(&s->warm_all, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && ready; i++) ready = __atomic_load_n(&s->pipes[q[i]][fam], __ATOMIC_ACQUIRE) != VK_NULL_HANDLE;
    if (ready) return;
    pthread_mutex_lock(&s->plock);
    queue_family(e, fam, 1);
    if (!s->warm_all) {
        __atomic_store_n(&s->warm_all, 1, __ATOMIC_RELEASE);
        for (VkFormat f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; f++)
            if (s->tx[BC_INDEX(f)]) queue_family(e, tx_family(family(f), s->tx[BC_INDEX(f)]), 0);
    }
    pthread_mutex_unlock(&s->plock);
}

/* fam built by the warm threads when they have nothing more urgent (the re-encoders: only a copy
 * between a transcoded and a decoded image uses them). */
static void prebuild_later(vkb_emu_device *e, int fam)
{
    bcn_state *s = e->bcn;
    uint32_t q[2], n = qualities(s, fam, q);
    int ready = 1;
    for (uint32_t i = 0; i < n && ready; i++) ready = __atomic_load_n(&s->pipes[q[i]][fam], __ATOMIC_ACQUIRE) != VK_NULL_HANDLE;
    if (ready) return;
    pthread_mutex_lock(&s->plock);
    queue_family(e, fam, 0);
    pthread_mutex_unlock(&s->plock);
}

/* Stores what a build of fam at q gave. plock held. */
static void publish(bcn_state *s, int fam, uint32_t q, VkPipeline p)
{
    __atomic_store_n(&s->pipes[q][fam], p, __ATOMIC_RELEASE);
    s->building[q][fam] = 0;
    if (!p) s->failed[q][fam] = 1;
    pthread_cond_broadcast(&s->pcond);
}

/* The decoder or transcoder for an upload of fam at q (the transcoders' BC_QUALITY, 0-2; ignored by
 * decoders). Under auto a large level does not wait for the fast transcoder when the default one is
 * built: default encodes it (better, a little slower), and fast moves to the front of the warm
 * queue. Small levels are never encoded fast in its place - they stay as encoded, and they are what
 * distant surfaces and UI show. Otherwise it waits for a build under way, or builds q itself. NULL:
 * nothing builds. */
static VkPipeline pipeline(vkb_emu_device *e, int fam, uint32_t q)
{
    bcn_state *s = e->bcn;
    if (fam < TX_BC1) q = 1;
    VkPipeline p = __atomic_load_n(&s->pipes[q][fam], __ATOMIC_ACQUIRE);
    if (p) return p;
    int alt = fam >= TX_BC1 && s->quality == QUALITY_AUTO && q == 0;
    const uint32_t o = 1;
    pthread_mutex_lock(&s->plock);
    for (;;) {
        if ((p = s->pipes[q][fam])) break;
        if (alt && (p = s->pipes[o][fam])) {
            warm_queue(e, fam, q, 1);
            VKB_ONCE(VKB_LOG_INFO, "BC emulation: a large level encoded with the default transcoder %d while the fast one is "
                     "built (logged once)", fam);
            break;
        }
        if (s->failed[q][fam]) {
            if (!alt) break;
            q = o, alt = 0; /* default, or nothing */
            continue;
        }
        if (s->building[q][fam] || (alt && s->building[o][fam])) {
            pthread_cond_wait(&s->pcond, &s->plock);
            continue;
        }
        s->building[q][fam] = 1;
        pthread_mutex_unlock(&s->plock);
        p = build(e, fam, q, 0);
        pthread_mutex_lock(&s->plock);
        publish(s, fam, q, p);
        if (fam >= TX_BC1 && s->quality == QUALITY_AUTO && q < 2) warm_queue(e, fam, q ^ 1, 0);
        break;
    }
    pthread_mutex_unlock(&s->plock);
    return p;
}

/* A warm thread: builds what is queued, urgent first, until the device goes (vkb_emu_bcn_quiesce). */
static void *warm_main(void *arg)
{
    vkb_emu_device *e = arg;
    bcn_state *s = e->bcn;
    vkb_srv_set_current_table(e->dev); /* the layers beneath (hang, gpuprof) find the device by it */
    pthread_mutex_lock(&s->plock);
    for (;;) {
        int fam = -1;
        uint32_t q = 0;
        for (int u = 0; u < 2 && fam < 0; u++)
            for (uint32_t k = 0; k < 3 && fam < 0; k++) {
                uint32_t *m = u ? s->want : s->urgent;
                if (!m[k]) continue;
                q = k, fam = __builtin_ctz(m[k]);
                s->urgent[k] &= ~(1u << fam), s->want[k] &= ~(1u << fam);
            }
        if (s->warm_stop) break;
        if (fam < 0) {
            pthread_cond_wait(&s->pcond, &s->plock);
            continue;
        }
        if (s->pipes[q][fam] || s->building[q][fam] || s->failed[q][fam]) continue;
        s->building[q][fam] = 1;
        pthread_mutex_unlock(&s->plock);
        VkPipeline p = build(e, fam, q, 1);
        pthread_mutex_lock(&s->plock);
        publish(s, fam, q, p);
    }
    pthread_mutex_unlock(&s->plock);
    return NULL;
}

/* Before the device's layers go (vkDestroyDevice, or its process gone): the warm threads compile
 * through them and the pipeline cache. Builds under way finish first. */
void vkb_emu_bcn_quiesce(vkb_srv_table *dev)
{
    bcn_state *s = dev->emu ? dev->emu->bcn : NULL;
    if (!s) return;
    pthread_mutex_lock(&s->plock);
    s->warm_stop = 1;
    int n = s->nwarm;
    s->nwarm = 0;
    pthread_cond_broadcast(&s->pcond);
    pthread_mutex_unlock(&s->plock);
    for (int i = 0; i < n; i++) pthread_join(s->warm[i], NULL);
}

typedef struct region {
    VkDeviceSize offset;
    uint32_t row_length, image_height;
    VkImageSubresourceLayers sub;
    VkOffset3D ioff;
    VkExtent3D iext;
} region;

/* One emulated upload being recorded. */
typedef struct upload {
    vkb_emu_device *e;
    VkCommandBuffer cb;
    bcn_cb *sc;
    VkBuffer src;
    vkb_emu_image *img;
    VkImageLayout layout;
    uint32_t ob, block_bytes;  /* bytes of a block in the scratch (decoded or transcoded), in the source */
    uint32_t n_dim;          /* texels a side of an output block: 4, or 6 / 8 for ASTC 6x6 / 8x8 */
    uint32_t xflags;         /* more flags for the shader (a re-encoder's, bc_decode.comp SRC_RAW) */
    VkDeviceSize left;       /* scratch the rest of the upload needs (sizes new chunks) */
    struct bc_batch *pl;     /* decoded, waiting to be copied into their images */
} upload;

/* ------------------------------------------------------------------ uploads, counted and batched */

/* What the conversions of the app's buffer-to-image copies into BC images record, all devices: a
 * line every 60 s while it changes and at each device's end. GTA V (DXVK 3.1.1) streams
 * ~1700 textures of ~25 KB in a second: one copy per mip level each, back to back with no barrier
 * between them (DXVK records its layout transitions into a command buffer of their own), which
 * the conversion turned into two barriers, a dispatch, a copy and a restore of the app's compute
 * state per copy. */
enum { BU_COPIES, BU_JOINED, BU_BARRIERS, BU_DISPATCHES, BU_COPYCMDS, BU_RESTORES, BU_COUNT };
static uint64_t g_bu[BU_COUNT], g_bu_told[BU_COUNT], g_bu_ns;

static void uploads_line(int end)
{
    uint64_t v[BU_COUNT];
    int changed = 0;
    for (int i = 0; i < BU_COUNT; i++) {
        v[i] = __atomic_load_n(&g_bu[i], __ATOMIC_RELAXED);
        changed |= v[i] != __atomic_load_n(&g_bu_told[i], __ATOMIC_RELAXED);
    }
    if (!changed && !(end && v[BU_COPIES])) return;
    for (int i = 0; i < BU_COUNT; i++) __atomic_store_n(&g_bu_told[i], v[i], __ATOMIC_RELAXED);
    VKB_INFO("BC uploads%s: %llu copies into BC images by the app (%llu right after another one, sharing its barriers); "
             "the conversions recorded %llu barriers, %llu dispatches, %llu copy commands, %llu restores of the app's compute state",
             end ? " so far" : " (running totals)", (unsigned long long)v[BU_COPIES], (unsigned long long)v[BU_JOINED],
             (unsigned long long)v[BU_BARRIERS], (unsigned long long)v[BU_DISPATCHES], (unsigned long long)v[BU_COPYCMDS],
             (unsigned long long)v[BU_RESTORES]);
}

static inline void bu_count(int what, uint64_t n) { __atomic_add_fetch(&g_bu[what], n, __ATOMIC_RELAXED); }

static void bu_tick(void)
{
    uint64_t now = vkb_hitch_now(), last = __atomic_load_n(&g_bu_ns, __ATOMIC_RELAXED);
    if ((!last || now - last >= 60000000000ull) && __atomic_compare_exchange_n(&g_bu_ns, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        uploads_line(0);
}

/* Converted blocks waiting to be copied into their images: an upload's own (flushed at its end), or
 * the thread's batch (tls_batch). A batch: in a replayed command stream (vkEndCommandBuffer, one
 * thread, one command buffer), a BC buffer-to-image copy right after another one (nothing but
 * other buffer-to-image copies between them) adds its dispatches after the earlier ones' and its
 * copies to theirs, under the one to_compute barrier, and the batch's copies, one flush barrier and
 * one restore of the app's compute state come before the stream's next other command
 * (vkb_emu_bcn_batch_flush, server_conn.c) or the driver's End. Nothing else ran between those
 * copies, the dispatches only read the app's buffers and write scratch, and copies with no command
 * between them are not ordered against each other in Vulkan, so each command after the batch sees
 * what it saw with a barrier pair per copy. VKBRIDGE_BC_BATCH=0: a pair per copy. */
typedef struct pend {
    VkBuffer buf;
    VkImage image;
    VkImageLayout layout;
} pend;

typedef struct bc_batch {
    vkb_emu_device *e;
    VkCommandBuffer cb;      /* the batch's (VK_NULL_HANDLE: none open) */
    VkPipeline bound;        /* the converter its last dispatch used */
    pend *p;                 /* per copy: from where into what */
    VkBufferImageCopy *c;    /* ... and its region */
    uint32_t n, cap;
} bc_batch;

static __thread bc_batch tls_batch;
__thread int vkb_emu_bc_batch_open;
static __thread int tls_b2i; /* the conversion in hand is of an app's buffer-to-image copy */

/* Earlier transfers (the app's writes to the source, earlier copies out of this scratch) before the
 * decoder reads the source and writes the scratch. */
static void to_compute(upload *u)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    u->e->dev->real.vkCmdPipelineBarrier(u->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL,
                                         0, NULL);
    bu_count(BU_BARRIERS, 1);
}

/* Copies what is decoded into the images, in the layouts the app recorded its copies with: one copy
 * command per run of the same scratch buffer, image and layout. */
static void pend_flush(vkb_emu_device *e, VkCommandBuffer cb, bc_batch *b)
{
    if (!b->n) return;
    const vkb_dispatch *r = &e->dev->real;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    r->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    uint64_t cmds = 0;
    for (uint32_t i = 0; i < b->n;) {
        uint32_t k = i + 1;
        while (k < b->n && b->p[k].buf == b->p[i].buf && b->p[k].image == b->p[i].image && b->p[k].layout == b->p[i].layout) k++;
        r->vkCmdCopyBufferToImage(cb, b->p[i].buf, b->p[i].image, b->p[i].layout, k - i, &b->c[i]);
        cmds++;
        i = k;
    }
    b->n = 0;
    bu_count(BU_BARRIERS, 1);
    bu_count(BU_COPYCMDS, cmds);
}

static void flush(upload *u) { pend_flush(u->e, u->cb, u->pl); }

void vkb_emu_bcn_batch_flush(void)
{
    bc_batch *b = &tls_batch;
    vkb_emu_bc_batch_open = 0;
    if (!b->cb) return;
    /* a crash line names these copies, not the command they come before */
    extern __thread const char *vkb_srv_current_cmd;
    const char *saved_cmd = vkb_srv_current_cmd;
    vkb_srv_current_cmd = "vkCmdCopyBufferToImage (BC uploads recorded back to back, their copies)";
    pend_flush(b->e, b->cb, b);
    vkb_emu_restore_compute(b->e, b->cb);
    vkb_srv_current_cmd = saved_cmd;
    bu_count(BU_RESTORES, 1);
    b->e = NULL, b->cb = VK_NULL_HANDLE, b->bound = VK_NULL_HANDLE;
    bu_tick();
}

/* A stream cut short: its command buffer is not ended (nor submitted), so nothing more goes into it. */
void vkb_emu_bcn_batch_drop(void)
{
    bc_batch *b = &tls_batch;
    vkb_emu_bc_batch_open = 0;
    b->n = 0;
    b->e = NULL, b->cb = VK_NULL_HANDLE, b->bound = VK_NULL_HANDLE;
}

/* Room for `bytes` of decoded texels in the command buffer's scratch. */
static int slot(upload *u, VkDeviceSize bytes, VkBuffer *buf, VkDeviceSize *off)
{
    bcn_cb *c = u->sc;
    int wrapped = 0;
    bytes = (bytes + SLOT_ALIGN - 1) & ~(VkDeviceSize)(SLOT_ALIGN - 1);
    for (;;) {
        if (c->cur < c->n) {
            chunk *k = &c->held[c->cur];
            if (c->used + bytes <= k->size) {
                *buf = k->buf;
                *off = c->used;
                c->used += bytes;
                return 1;
            }
            if (c->cur + 1 < c->n) {
                c->cur++;
                c->used = 0;
                continue;
            }
        }
        VkDeviceSize total = 0;
        for (uint32_t i = 0; i < c->n; i++) total += c->held[i].size;
        if (total < CB_MAX || wrapped) {
            if (c->n == c->cap) {
                uint32_t cap = c->cap ? c->cap * 2 : 4;
                chunk *nh = realloc(c->held, cap * sizeof(*nh));
                if (!nh) return 0;
                c->held = nh;
                c->cap = cap;
            }
            VkDeviceSize want = u->left < CHUNK_MAX ? u->left : CHUNK_MAX;
            int got = chunk_get(u->e, bytes, want > bytes ? want : bytes, wrapped || !c->n, &c->held[c->n]);
            if (!got) return 0;
            if (got > 0) {
                c->cur = c->n++;
                c->used = 0;
                continue;
            }
        } else {
            VKB_ONCE(VKB_LOG_INFO, "BC emulation: uploads past %llu MiB in one command buffer reuse its decode scratch (logged once)",
                     (unsigned long long)(CB_MAX >> 20));
        }
        /* The command buffer holds its share (or all scratch is taken): copy out what is decoded,
         * then decode over it. */
        flush(u);
        to_compute(u);
        c->cur = 0;
        c->used = 0;
        wrapped = 1;
    }
}

static void queue_copy(upload *u, VkBuffer buf, const VkBufferImageCopy *c);

/* VKBRIDGE_TEST_BC_SLOW=<n>[,<m>] (host tests, tests/bcburst_check.c): every transcoder dispatch
 * recorded n times and every decoder dispatch m times (default n), so a desktop GPU spends on a
 * conversion what a Mali GPU does (the G720 transcodes 20-30x slower than an RX 7900 XTX). The
 * repeats write the same blocks. Read once, from the server's environment only. */
static uint32_t test_slow(int transcode)
{
    static int n = -1, m = -1;
    if (n < 0) {
        const char *v = getenv("VKBRIDGE_TEST_BC_SLOW");
        char *end = NULL;
        int a = v && *v ? (int)strtol(v, &end, 10) : 0, b = end && *end == ',' ? atoi(end + 1) : a;
        if (a > 1 || b > 1) VKB_WARN("VKBRIDGE_TEST_BC_SLOW: transcoder dispatches recorded %d times, decoder dispatches %d times, on purpose", a, b);
        m = b > 1 ? b : 1;
        __atomic_store_n(&n, a > 1 ? a : 1, __ATOMIC_RELEASE);
    }
    return (uint32_t)(transcode ? __atomic_load_n(&n, __ATOMIC_ACQUIRE) : m);
}

static void dispatch(upload *u, uint32_t x, uint32_t y, uint32_t z)
{
    const vkb_dispatch *r = &u->e->dev->real;
    uint32_t rep = test_slow(vkb_tx_block_bytes(u->img->format) != 0);
    for (uint32_t i = 0; i < rep; i++) {
        if (i) {
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT};
            r->vkCmdPipelineBarrier(u->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        r->vkCmdDispatch(u->cb, x, y, z);
    }
    bu_count(BU_DISPATCHES, rep);
    if (rep > 1) bu_count(BU_BARRIERS, rep - 1);
}

/* Decodes slices [slice0, slice0 + nslices) x block rows [row0, row0 + nrows) of a region into
 * scratch and queues its copy into the image. */
static void piece(upload *u, const region *g, uint32_t slice0, uint32_t nslices, uint32_t row0, uint32_t nrows)
{
    bcn_state *s = u->e->bcn;
    const vkb_dispatch *r = &u->e->dev->real;
    uint32_t w_tex = g->row_length ? g->row_length : g->iext.width;
    uint32_t h_tex = g->image_height ? g->image_height : g->iext.height;
    uint32_t row_blocks = (w_tex + 3) / 4, layer_blocks = row_blocks * ((h_tex + 3) / 4);
    uint32_t bw = (g->iext.width + 3) / 4;
    VkDeviceSize bytes = (VkDeviceSize)bw * nrows * u->ob * nslices;
    VkBuffer buf;
    VkDeviceSize off;
    if (!slot(u, bytes, &buf, &off)) return;
    u->left = u->left > bytes ? u->left - bytes : 0;

    VkDeviceSize first = g->offset + ((VkDeviceSize)slice0 * layer_blocks + (VkDeviceSize)row0 * row_blocks) * u->block_bytes;
    VkDeviceSize aligned = first & ~(s->ssbo_align - 1);
    VkDeviceSize range = first - aligned +
                         ((VkDeviceSize)(nslices - 1) * layer_blocks + (VkDeviceSize)(nrows - 1) * row_blocks + bw) * u->block_bytes;
    VkDescriptorBufferInfo sbi = {u->src, aligned, range}, dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &sbi, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL}};
    if (s->push) {
        r->vkCmdPushDescriptorSet(u->cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 2, w);
    } else {
        VkDescriptorSet set = vkb_emu_cb_scratch_set(u->e, u->cb, s->dsl);
        if (!set) {
            VKB_ONCE(VKB_LOG_ERROR, "BC emulation: no descriptor set for the decoder (logged once)");
            return;
        }
        w[0].dstSet = w[1].dstSet = set;
        r->vkUpdateDescriptorSets(u->e->dev->device, 2, w, 0, NULL);
        r->vkCmdBindDescriptorSets(u->cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1, &set, 0, NULL);
    }
    uint32_t y0 = row0 * 4, h = g->iext.height - y0 < nrows * 4 ? g->iext.height - y0 : nrows * 4;
    pc_t pc = {(uint32_t)((first - aligned) / 4), row_blocks, layer_blocks, family_flags(u->img) | u->xflags,
               (uint32_t)(off / 4), 0, {g->iext.width, h}};
    r->vkCmdPushConstants(u->cb, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dispatch(u, (bw + 7) / 8, (nrows + 7) / 8, nslices);

    VkBufferImageCopy c = {off, bw * 4, nrows * 4, g->sub, {g->ioff.x, g->ioff.y + (int32_t)y0, g->ioff.z}, {g->iext.width, h, 1}};
    if (u->img->type == VK_IMAGE_TYPE_3D) {
        c.imageOffset.z += (int32_t)slice0;
        c.imageExtent.depth = nslices;
    } else {
        c.imageSubresource.baseArrayLayer += slice0;
        c.imageSubresource.layerCount = nslices;
    }
    queue_copy(u, buf, &c);
}

/* Queues a copy of decoded (transcoded) blocks into the image, cut to the mip level (a region may
 * name whole blocks past a small mip's edge). */
static void queue_copy(upload *u, VkBuffer buf, const VkBufferImageCopy *cin)
{
    VkBufferImageCopy c = *cin;
    uint32_t mw = u->img->extent.width >> c.imageSubresource.mipLevel, mh = u->img->extent.height >> c.imageSubresource.mipLevel;
    if (!mw) mw = 1;
    if (!mh) mh = 1;
    if ((uint32_t)c.imageOffset.x >= mw || (uint32_t)c.imageOffset.y >= mh) return;
    if (c.imageOffset.x + c.imageExtent.width > mw) c.imageExtent.width = mw - (uint32_t)c.imageOffset.x;
    if (c.imageOffset.y + c.imageExtent.height > mh) c.imageExtent.height = mh - (uint32_t)c.imageOffset.y;
    bc_batch *b = u->pl;
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 16;
        pend *np = realloc(b->p, cap * sizeof(*np));
        if (np) b->p = np;
        VkBufferImageCopy *nc = realloc(b->c, cap * sizeof(*nc));
        if (nc) b->c = nc;
        if (!np || !nc) return;
        b->cap = cap;
    }
    b->p[b->n] = (pend){buf, u->img->image, u->layout};
    b->c[b->n++] = c;
}

/* ASTC 6x6 / 8x8 output: a region is encoded in whole ASTC blocks from the block-aligned corner at
 * or before its start (shift: how far before), so its blocks per row and rows of blocks. */
static void n_blocks(const upload *u, const region *g, uint32_t *bw, uint32_t *bh, uint32_t *sx, uint32_t *sy)
{
    uint32_t n = u->n_dim;
    *sx = (uint32_t)g->ioff.x % n, *sy = (uint32_t)g->ioff.y % n;
    *bw = (*sx + g->iext.width + n - 1) / n, *bh = (*sy + g->iext.height + n - 1) / n;
}

/* ASTC 6x6 / 8x8: rows [row0, row0 + nrows) of a region's ASTC blocks (n_blocks) in slices
 * [slice0, slice0 + nslices), each encoded from the BC blocks under it (bc_decode.comp ENC_ASTCN),
 * and the copy of them queued. A region that does not start on an ASTC block's edge (a sub-region
 * update) is encoded from the corner before it, with its own texels repeated over what lies outside
 * it: the texels around it that share its edge blocks are re-encoded from its edge (logged once). */
static void piece_n(upload *u, const region *g, uint32_t slice0, uint32_t nslices, uint32_t row0, uint32_t nrows)
{
    bcn_state *s = u->e->bcn;
    const vkb_dispatch *r = &u->e->dev->real;
    uint32_t n = u->n_dim, bw, bh, sx, sy;
    n_blocks(u, g, &bw, &bh, &sx, &sy);
    uint32_t w_tex = g->row_length ? g->row_length : g->iext.width;
    uint32_t h_tex = g->image_height ? g->image_height : g->iext.height;
    uint32_t row_blocks = (w_tex + 3) / 4, layer_blocks = row_blocks * ((h_tex + 3) / 4), bcw = (g->iext.width + 3) / 4;
    uint32_t y0 = row0 * n, h = sy + g->iext.height - y0 < nrows * n ? sy + g->iext.height - y0 : nrows * n;
    /* the region's texel rows this piece reads, and their BC block rows */
    uint32_t v0 = y0 > sy ? y0 - sy : 0, v1 = y0 + h - 1 > sy ? y0 + h - 1 - sy : 0;
    if (v1 >= g->iext.height) v1 = g->iext.height - 1;
    uint32_t r0 = v0 / 4, r1 = v1 / 4;
    VkDeviceSize bytes = (VkDeviceSize)bw * nrows * u->ob * nslices;
    VkBuffer buf;
    VkDeviceSize off;
    if (!slot(u, bytes, &buf, &off)) return;
    u->left = u->left > bytes ? u->left - bytes : 0;
    VkDeviceSize first = g->offset + ((VkDeviceSize)slice0 * layer_blocks + (VkDeviceSize)r0 * row_blocks) * u->block_bytes;
    VkDeviceSize aligned = first & ~(s->ssbo_align - 1);
    VkDeviceSize range = first - aligned +
                         ((VkDeviceSize)(nslices - 1) * layer_blocks + (VkDeviceSize)(r1 - r0) * row_blocks + bcw) * u->block_bytes;
    VkDescriptorBufferInfo sbi = {u->src, aligned, range}, dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &sbi, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL}};
    if (s->push) {
        r->vkCmdPushDescriptorSet(u->cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 2, w);
    } else {
        VkDescriptorSet set = vkb_emu_cb_scratch_set(u->e, u->cb, s->dsl);
        if (!set) {
            VKB_ONCE(VKB_LOG_ERROR, "BC emulation: no descriptor set for the decoder (logged once)");
            return;
        }
        w[0].dstSet = w[1].dstSet = set;
        r->vkUpdateDescriptorSets(u->e->dev->device, 2, w, 0, NULL);
        r->vkCmdBindDescriptorSets(u->cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1, &set, 0, NULL);
    }
    pc_t pc = {(uint32_t)((first - aligned) / 4), row_blocks, layer_blocks, family_flags(u->img) | u->xflags, (uint32_t)(off / 4), 0,
               {sx + g->iext.width, h}, {sx | (sy << 4) | (r0 << 8), y0, g->iext.width, g->iext.height}};
    r->vkCmdPushConstants(u->cb, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dispatch(u, (bw + 7) / 8, (nrows + 7) / 8, nslices);
    if (sx || sy || ((g->ioff.x + g->iext.width) % n && g->ioff.x + g->iext.width < (u->img->extent.width >> g->sub.mipLevel)) ||
        ((g->ioff.y + g->iext.height) % n && g->ioff.y + g->iext.height < (u->img->extent.height >> g->sub.mipLevel)))
        VKB_ONCE(VKB_LOG_INFO, "BC emulation: an upload not on ASTC %ux%u block edges (at %d,%d, %ux%u): the texels around it in its edge "
                 "blocks are re-encoded from its own (logged once)", n, n, g->ioff.x, g->ioff.y, g->iext.width, g->iext.height);
    VkBufferImageCopy c = {off, bw * n, nrows * n, g->sub, {g->ioff.x - (int32_t)sx, g->ioff.y - (int32_t)sy + (int32_t)y0, g->ioff.z},
                           {bw * n, nrows * n, 1}};
    if (u->img->type == VK_IMAGE_TYPE_3D) {
        c.imageOffset.z += (int32_t)slice0;
        c.imageExtent.depth = nslices;
    } else {
        c.imageSubresource.baseArrayLayer += slice0;
        c.imageSubresource.layerCount = nslices;
    }
    queue_copy(u, buf, &c);
}

static VkDeviceSize region_bytes(const upload *u, const region *g, uint32_t slices)
{
    if (u->n_dim != 4) {
        uint32_t bw, bh, sx, sy;
        n_blocks(u, g, &bw, &bh, &sx, &sy);
        return (VkDeviceSize)bw * bh * u->ob * slices;
    }
    return (VkDeviceSize)((g->iext.width + 3) / 4) * ((g->iext.height + 3) / 4) * u->ob * slices;
}

/* VKBRIDGE_TEST_HITCH_DELAY_MS=<ms> (host tests, tests/hitch_check.c): each BC upload recorded and
 * each allocation of 64 MB or more (server_memory.c) takes that much longer on purpose, so that it
 * makes a hitch of its own. Read once, from the server's environment only. */
int vkb_test_hitch_delay_ms(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("VKBRIDGE_TEST_HITCH_DELAY_MS");
        v = s && *s ? atoi(s) : 0;
        if (v > 0) VKB_WARN("VKBRIDGE_TEST_HITCH_DELAY_MS: BC uploads and allocations of 64 MB or more take %d ms longer on purpose", v);
    }
    return v;
}

static void decode_now(vkb_emu_device *e, VkCommandBuffer cb, VkBuffer src, vkb_emu_image *img, VkImageLayout layout, uint32_t n,
                       const region *regs, const vkb_emu_image *raw);

/* The most GPU time a conversion of these regions can take on this device, in ns, for the hitch
 * line: server_hitch.c puts a wait down to BC conversions only when this reaches a fifth of it.
 * Calibration: the G720 MC12's warm BC7 -> ASTC 4x4 2048x2048 level at the fast search, 4.2 Mtex in
 * 29 ms (bcn_check, FEAT-bc-budget.md), so GPU_EST_NS_TEXEL a texel; scaled by budget_auto's rate for
 * this GPU (its cores x a core's speed, 32 = the G720 MC12; not a Mali: as a G720 MC12), times the
 * family's weight (tx_weight; decoders 1), the search's cost at the top of what was measured
 * (default GPU_EST_DEFAULT x the fast one, high GPU_EST_HIGH) and the host tests' dispatch repeats
 * (VKBRIDGE_TEST_BC_SLOW). An upper estimate on purpose: a wait is called bc only when the
 * conversions could have filled a fifth of it. The fixed cost of each conversion (its barriers and
 * small dispatches) is not in it; bc_n in the hitch line counts them. */
#define GPU_EST_NS_TEXEL (29e6 / 4.2e6)
#define GPU_EST_DEFAULT 4.0
#define GPU_EST_HIGH 6.0
static uint64_t gpu_est_ns(const bcn_state *s, const vkb_emu_image *img, uint32_t n, const region *regs, const vkb_emu_image *raw)
{
    int fam = family(img->client_format);
    int transcode = vkb_tx_block_bytes(img->format) != 0;
    fam = raw ? raw_family(img->format) : transcode ? tx_family(fam, img->format) : fam;
    double scale = s && s->auto_rate > 0 ? s->auto_rate / 32.0 : 1.0;
    double w = fam >= TX_BC1 ? tx_weight(fam) : 1.0, ns = 0;
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)regs[i].iext.width * regs[i].iext.height *
                   (img->type == VK_IMAGE_TYPE_3D ? regs[i].iext.depth : regs[i].sub.layerCount);
        uint32_t q = fam >= TX_BC1 ? level_quality(s, img, regs[i].sub.mipLevel) : 0;
        ns += t * (q == 0 ? 1.0 : q == 2 ? GPU_EST_HIGH : GPU_EST_DEFAULT);
    }
    return (uint64_t)(ns * w * GPU_EST_NS_TEXEL / scale * test_slow(transcode));
}

/* A conversion recorded: timed and counted for the hitch line when the process has it on
 * (VKBRIDGE_HITCH_MS, server_hitch.c); its GPU time shows as the waits that follow it. */
/* raw: the source holds texels of that decoded image (copy_mixed), not BC blocks. */
static void decode(vkb_emu_device *e, VkCommandBuffer cb, VkBuffer src, vkb_emu_image *img, VkImageLayout layout,
                   uint32_t n, const region *regs, const vkb_emu_image *raw)
{
    vkb_srv_proc *p = e->dev ? e->dev->proc : NULL;
    int delay = vkb_test_hitch_delay_ms();
    if (!vkb_hitch_on(p) && delay <= 0) {
        decode_now(e, cb, src, img, layout, n, regs, raw);
        return;
    }
    uint64_t t0 = vkb_hitch_now(), texels = 0;
    if (delay > 0) {
        struct timespec ts = {delay / 1000, (long)(delay % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    decode_now(e, cb, src, img, layout, n, regs, raw);
    for (uint32_t i = 0; i < n; i++)
        texels += (uint64_t)regs[i].iext.width * regs[i].iext.height *
                  (img->type == VK_IMAGE_TYPE_3D ? regs[i].iext.depth : regs[i].sub.layerCount);
    vkb_hitch_note_bc(p, t0, vkb_hitch_now(), texels, gpu_est_ns(e->bcn, img, n, regs, raw));
}

static void decode_now(vkb_emu_device *e, VkCommandBuffer cb, VkBuffer src, vkb_emu_image *img, VkImageLayout layout, uint32_t n,
                       const region *regs, const vkb_emu_image *raw)
{
    bcn_state *s = e->bcn;
    /* an app's buffer-to-image copy in a replayed stream joins the batch open on it (bc_batch);
     * anything else flushes a batch first */
    bc_batch own = {0}, *b = &own;
    int batched = tls_b2i && !raw && s->batch && vkb_srv_replay_cb == cb;
    if (vkb_emu_bc_batch_open && (!batched || tls_batch.cb != cb || tls_batch.e != e)) vkb_emu_bcn_batch_flush();
    if (batched) b = &tls_batch;
    int joined = b->cb == cb;
    if (tls_b2i) bu_count(BU_COPIES, 1);
    int fam = family(img->client_format);
    fam = vkb_tx_block_bytes(img->format) ? tx_family(fam, img->format) : vkb_bc_is_packed(img->format) ? FAM_BC1_16 : fam;
    uint32_t xflags = 0, in_bytes = vkb_bc_block_bytes(img->client_format);
    if (raw) {
        /* texels: a 4x4 tile of them counts as the source's block */
        fam = raw_family(img->format);
        in_bytes = 16 * raw_texel(fam, raw->format, &xflags);
        if (family(img->client_format) == FAM_BC7) xflags |= 32;
    }
    /* in auto per region (level_quality): the first region's, before anything is recorded (or the
     * other quality while this one is still being built) */
    uint32_t q0 = fam >= TX_BC1 ? level_quality(s, img, n ? regs[0].sub.mipLevel : 0) : 1;
    VkPipeline pipe = pipeline(e, fam, q0), bound = joined ? b->bound : VK_NULL_HANDLE;
    if (!pipe) return;
    upload u = {e, cb, vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb)), src, img, layout, out_block_bytes(img), in_bytes,
                vkb_tx_block_dim(img->format), .xflags = xflags, .pl = b};
    if (!u.sc) return;
    int is3d = img->type == VK_IMAGE_TYPE_3D;
    for (uint32_t i = 0; i < n; i++) u.left += region_bytes(&u, &regs[i], is3d ? regs[i].iext.depth : regs[i].sub.layerCount);
    /* Pieces are cut to fit a chunk. Near the cap, to fit what the command buffer holds (at least
     * CHUNK_MIN), so it decodes through that rather than taking 16 MiB more. */
    VkDeviceSize lim = CHUNK_MAX;
    if (__atomic_load_n(&g_live, __ATOMIC_RELAXED) + CHUNK_MAX > LIVE_MAX) {
        lim = CHUNK_MIN;
        for (uint32_t i = 0; i < u.sc->n; i++)
            if (u.sc->held[i].size > lim && u.sc->held[i].size <= CHUNK_MAX) lim = u.sc->held[i].size;
    }
    if (joined) {
        bu_count(BU_JOINED, 1);
    } else {
        to_compute(&u);
        if (batched) {
            b->e = e, b->cb = cb;
            vkb_emu_bc_batch_open = 1;
        }
    }
    /* VKBRIDGE_BC_LOG=<texels> (a diagnostic): uploads of that many texels or more logged with
     * their BC format, target and size, to tell which transcoders a stall in the frame comes from. */
    const char *lv = vkb_srv_env("VKBRIDGE_BC_LOG");
    long log_min = lv ? atol(lv) : 0;
    if (log_min > 0) {
        uint64_t tex = 0;
        for (uint32_t i = 0; i < n; i++)
            tex += (uint64_t)regs[i].iext.width * regs[i].iext.height * (is3d ? regs[i].iext.depth : regs[i].sub.layerCount);
        if (tex >= (uint64_t)log_min)
            VKB_INFO("BC upload: format %d -> %d (family %d), %llu texels in %u region(s), first %ux%u mip %u", img->client_format, img->format, fam,
                     (unsigned long long)tex, n, n ? regs[0].iext.width : 0, n ? regs[0].iext.height : 0, n ? regs[0].sub.mipLevel : 0);
    }
    for (uint32_t i = 0; i < n; i++) {
        const region *g = &regs[i];
        uint32_t slices = is3d ? g->iext.depth : g->sub.layerCount;
        uint32_t bh = (g->iext.height + 3) / 4;
        if (!g->iext.width || !g->iext.height || !slices) continue;
        if (fam >= TX_BC1 && s->quality == QUALITY_AUTO) {
            VkPipeline p = pipeline(e, fam, level_quality(s, img, g->sub.mipLevel));
            pipe = p ? p : pipe; /* a variant that failed to build: the one there is */
        }
        if (pipe != bound) {
            e->dev->real.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            bound = pipe;
        }
        if (u.n_dim != 4) {
            /* ASTC 6x6 / 8x8: whole slices, or bands of ASTC block rows */
            uint32_t nbw, nbh, sx, sy;
            n_blocks(&u, g, &nbw, &nbh, &sx, &sy);
            VkDeviceSize sb = (VkDeviceSize)nbw * nbh * u.ob, rb = (VkDeviceSize)nbw * u.ob;
            if (sb <= lim) {
                uint32_t per = (uint32_t)(lim / sb);
                for (uint32_t z = 0; z < slices; z += per) piece_n(&u, g, z, slices - z < per ? slices - z : per, 0, nbh);
            } else {
                uint32_t per = rb <= lim ? (uint32_t)(lim / rb) : 1;
                for (uint32_t z = 0; z < slices; z++)
                    for (uint32_t y = 0; y < nbh; y += per) piece_n(&u, g, z, 1, y, nbh - y < per ? nbh - y : per);
            }
            continue;
        }
        VkDeviceSize slice_bytes = region_bytes(&u, g, 1), row_bytes = slice_bytes / bh;
        if (slice_bytes <= lim) {
            /* whole slices, as many as a chunk holds */
            uint32_t per = (uint32_t)(lim / slice_bytes);
            for (uint32_t z = 0; z < slices; z += per) piece(&u, g, z, slices - z < per ? slices - z : per, 0, bh);
        } else {
            /* bands of block rows (a 4096x4096 BC7 level: 64 MiB decoded) */
            uint32_t per = row_bytes <= lim ? (uint32_t)(lim / row_bytes) : 1;
            for (uint32_t z = 0; z < slices; z++)
                for (uint32_t y = 0; y < bh; y += per) piece(&u, g, z, 1, y, bh - y < per ? bh - y : per);
        }
    }
    if (batched) {
        b->bound = bound; /* its copies, barrier and restore: at the batch's flush */
        return;
    }
    flush(&u);
    free(own.p);
    free(own.c);
    vkb_emu_restore_compute(e, cb);
    bu_count(BU_RESTORES, 1);
    bu_tick();
}

static vkb_emu_image *bc_image(vkb_emu_device *e, VkImage img)
{
    if (!(e->flags & VKB_EMU_BCN)) return NULL;
    vkb_emu_image *r = vkb_emu_find_image(e, img);
    return r && vkb_is_bc(r->client_format) ? r : NULL;
}

int vkb_emu_copy_buffer_to_image(vkb_emu_device *e, VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                 const VkBufferImageCopy *regions)
{
    vkb_emu_image *img = bc_image(e, dst);
    if (!img) return 0;
    region *g = malloc((n ? n : 1) * sizeof(*g));
    for (uint32_t i = 0; i < n; i++)
        g[i] = (region){regions[i].bufferOffset, regions[i].bufferRowLength, regions[i].bufferImageHeight, regions[i].imageSubresource,
                        regions[i].imageOffset, regions[i].imageExtent};
    tls_b2i = 1;
    decode(e, cb, src, img, dl, n, g, NULL);
    tls_b2i = 0;
    free(g);
    return 1;
}

int vkb_emu_copy_buffer_to_image2(vkb_emu_device *e, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info)
{
    vkb_emu_image *img = bc_image(e, info->dstImage);
    if (!img) return 0;
    region *g = malloc((info->regionCount ? info->regionCount : 1) * sizeof(*g));
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2 *r = &info->pRegions[i];
        g[i] = (region){r->bufferOffset, r->bufferRowLength, r->bufferImageHeight, r->imageSubresource, r->imageOffset, r->imageExtent};
    }
    tls_b2i = 1;
    decode(e, cb, info->srcBuffer, img, info->dstImageLayout, info->regionCount, g, NULL);
    tls_b2i = 0;
    free(g);
    return 1;
}

/* Copies between BC images copy decoded texels (or transcoded blocks); a region may run past a
 * small mip's edge (whole blocks), which the decoded image does not have and which a compressed
 * one takes only as a region ending at the edge. */
static void clamp_region(const vkb_emu_image *s, const vkb_emu_image *d, const VkImageSubresourceLayers *ss,
                         const VkImageSubresourceLayers *ds, const VkOffset3D *so, const VkOffset3D *dof, VkExtent3D *ext)
{
    uint32_t sw = s->extent.width >> ss->mipLevel, sh = s->extent.height >> ss->mipLevel;
    uint32_t dw = d->extent.width >> ds->mipLevel, dh = d->extent.height >> ds->mipLevel;
    if (!sw) sw = 1;
    if (!sh) sh = 1;
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    if ((uint32_t)so->x + ext->width > sw) ext->width = sw > (uint32_t)so->x ? sw - (uint32_t)so->x : 0;
    if ((uint32_t)so->y + ext->height > sh) ext->height = sh > (uint32_t)so->y ? sh - (uint32_t)so->y : 0;
    if ((uint32_t)dof->x + ext->width > dw) ext->width = dw > (uint32_t)dof->x ? dw - (uint32_t)dof->x : 0;
    if ((uint32_t)dof->y + ext->height > dh) ext->height = dh > (uint32_t)dof->y ? dh - (uint32_t)dof->y : 0;
}

/* How a copy between BC images goes: 0 as a copy (the decoded formats are size-compatible, or both
 * transcoded to blocks of one size), 1 as an unscaled blit (one side decoded to 16 bits, the other
 * to RGBA8 UNORM or the other 16-bit format: the same texels, converted), 2 from a transcoded into
 * a decoded image and 3 the other way (copy_mixed), -1 not at all (a BC and an uncompressed image,
 * a 16-bit and an sRGB image, which a blit would convert out of sRGB), -2 not at all (transcoded
 * images of different block sizes). */
static int copy_kind(const vkb_emu_image *s, const vkb_emu_image *d)
{
    if (!s || !d) return -1;
    uint32_t st = vkb_tx_block_bytes(s->format), dt = vkb_tx_block_bytes(d->format);
    if (st && !dt) return 2;
    if (dt && !st) return 3;
    if (st || dt) {
        if (st == dt && vkb_tx_block_dim(s->format) == vkb_tx_block_dim(d->format)) return 0;
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy between BC images transcoded to formats %d and %d (other block sizes) is skipped "
                 "(logged once, counted in the BC copies line)", s->format, d->format);
        return -2;
    }
    int sp = vkb_bc_is_packed(s->format), dp = vkb_bc_is_packed(d->format);
    if (!sp && !dp) return vkb_bc_storage_format(s->client_format) == vkb_bc_storage_format(d->client_format) ? 0 : -1;
    if (s->format == d->format) return 0;
    if ((sp || s->format == VK_FORMAT_R8G8B8A8_UNORM) && (dp || d->format == VK_FORMAT_R8G8B8A8_UNORM)) return 1;
    return -1;
}

/* A copy between two ASTC 6x6 / 8x8 images must start on block edges and end on one or at the
 * level's edge; the app's regions are on BC's 4-texel edges, so one that is not on these is
 * skipped (logged once). Empty regions (clamp_region) are skipped too. */
static int on_blocks(const vkb_emu_image *s, const vkb_emu_image *d, const VkImageSubresourceLayers *ss, const VkImageSubresourceLayers *ds,
                     const VkOffset3D *so, const VkOffset3D *dof, const VkExtent3D *ext)
{
    if (!ext->width || !ext->height || !ext->depth) return 0;
    uint32_t n = vkb_tx_block_dim(s->format);
    if (n == 4) return 1;
    uint32_t sw = s->extent.width >> ss->mipLevel, sh = s->extent.height >> ss->mipLevel;
    uint32_t dw = d->extent.width >> ds->mipLevel, dh = d->extent.height >> ds->mipLevel;
    int ok = !(so->x % n) && !(so->y % n) && !(dof->x % n) && !(dof->y % n) &&
             (!(ext->width % n) || (so->x + ext->width >= (sw ? sw : 1) && dof->x + ext->width >= (dw ? dw : 1))) &&
             (!(ext->height % n) || (so->y + ext->height >= (sh ? sh : 1) && dof->y + ext->height >= (dh ? dh : 1)));
    if (!ok)
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy between images held as ASTC %ux%u off its block edges (%d,%d -> %d,%d, %ux%u) is "
                 "skipped (logged once)", n, n, so->x, so->y, dof->x, dof->y, ext->width, ext->height);
    return ok;
}

static int blit_region(const vkb_emu_image *s, const VkImageSubresourceLayers *ss, const VkImageSubresourceLayers *ds,
                       const VkOffset3D *so, const VkOffset3D *dof, const VkExtent3D *ext, VkImageBlit *out)
{
    if (!ext->width || !ext->height || !ext->depth) return 0;
    uint32_t depth = s->type == VK_IMAGE_TYPE_3D ? ext->depth : 1;
    *out = (VkImageBlit){*ss, {*so, {so->x + (int32_t)ext->width, so->y + (int32_t)ext->height, so->z + (int32_t)depth}},
                         *ds, {*dof, {dof->x + (int32_t)ext->width, dof->y + (int32_t)ext->height, dof->z + (int32_t)depth}}};
    return 1;
}

static void skipped_copy(void)
{
    copy_count(CP_SKIP_OTHER);
    VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy from a BC image into an uncompressed one (it would need an encoder) or into an "
                           "incompatible BC format is skipped (logged once, counted in the BC copies line)");
}

/* Bytes of one texel of the uncompressed formats a BC block can be copied from (64 and 128 bits),
 * 0 for any other. */
static uint32_t block_texel_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_USCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R64_UINT: case VK_FORMAT_R64_SINT: case VK_FORMAT_R64_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R64G64_UINT: case VK_FORMAT_R64G64_SINT: case VK_FORMAT_R64G64_SFLOAT: return 16;
    default: return 0;
    }
}

/* Staging for `need` bytes of blocks, kept by the command buffer until it is begun, reset or freed.
 * One it holds already is reused once the decoder has read what an earlier copy left there. */
static VkBuffer stage_buffer(vkb_emu_device *e, VkCommandBuffer cb, bcn_cb *c, VkDeviceSize need)
{
    for (uint32_t i = 0; i < c->nstage; i++)
        if (c->stage[i].size >= need) {
            e->dev->real.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                                              0, NULL);
            return c->stage[i].buf;
        }
    if (c->nstage == c->capstage) {
        uint32_t cap = c->capstage ? c->capstage * 2 : 2;
        chunk *ns = realloc(c->stage, cap * sizeof(*ns));
        if (!ns) return VK_NULL_HANDLE;
        c->stage = ns;
        c->capstage = cap;
    }
    if (chunk_get(e, need, need, 1, &c->stage[c->nstage]) <= 0) return VK_NULL_HANDLE;
    return c->stage[c->nstage++].buf;
}

/* A copy from an uncompressed image into a BC one: a texel of the source is a block of the
 * destination, and the extent counts source texels (blocks). The blocks go to staging, packed, and
 * are decoded from there as an upload of them would be. */
static void copy_blocks_in(vkb_emu_device *e, VkCommandBuffer cb, VkImage src, VkImageLayout sl, vkb_emu_image *d, VkImageLayout dl,
                           uint32_t n, const VkImageCopy *regions)
{
    vkb_emu_image *s = vkb_emu_find_image(e, src);
    uint32_t bb = vkb_bc_block_bytes(d->client_format);
    if (!s || block_texel_bytes(s->format) != bb) {
        copy_count(CP_SKIP_IN);
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy into a BC image from format %d (not %u bytes a texel) is skipped (logged once)",
                 s ? s->format : VK_FORMAT_UNDEFINED, bb);
        return;
    }
    bcn_cb *c = vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb));
    VkBufferImageCopy *out = malloc((n ? n : 1) * sizeof(*out));
    region *g = malloc((n ? n : 1) * sizeof(*g));
    if (!c || !out || !g) goto done;
    int s3d = s->type == VK_IMAGE_TYPE_3D, d3d = d->type == VK_IMAGE_TYPE_3D;
    VkDeviceSize total = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const VkImageCopy *r = &regions[i];
        uint32_t slices = s3d ? r->extent.depth : r->srcSubresource.layerCount;
        if (!r->extent.width || !r->extent.height || !slices) continue;
        out[k] = (VkBufferImageCopy){total, 0, 0, r->srcSubresource, r->srcOffset, {r->extent.width, r->extent.height, s3d ? slices : 1}};
        VkImageSubresourceLayers ds = r->dstSubresource;
        if (!d3d) ds.layerCount = slices;
        g[k++] = (region){total, r->extent.width * 4, r->extent.height * 4, ds, r->dstOffset,
                          {r->extent.width * 4, r->extent.height * 4, d3d ? slices : 1}};
        total += (VkDeviceSize)r->extent.width * r->extent.height * slices * bb;
        total = (total + SLOT_ALIGN - 1) & ~(VkDeviceSize)(SLOT_ALIGN - 1);
    }
    if (!k) goto done;
    VkBuffer buf = stage_buffer(e, cb, c, total);
    if (!buf) {
        VKB_ONCE(VKB_LOG_ERROR, "BC emulation: no memory to stage a %llu-byte copy into a BC image (logged once)", (unsigned long long)total);
        goto done;
    }
    e->dev->real.vkCmdCopyImageToBuffer(cb, src, sl, buf, k, out);
    decode(e, cb, buf, d, dl, k, g, NULL);
    static int told;
    if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
        VKB_INFO("BC emulation: copy into a BC image (format %d) from format %d decoded through staging (logged once)", d->client_format,
                 s->format);
done:
    free(out);
    free(g);
}

static int is_srgb(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return 1;
    default: return 0;
    }
}

static void mixed_skip(const char *why, const vkb_emu_image *s, const vkb_emu_image *d)
{
    copy_count(CP_SKIP_MIXED);
    VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy between BC images held as formats %d and %d is skipped: %s (logged once, counted in "
             "the BC copies line)", s->format, d->format, why);
}

/* A copy between a transcoded and a decoded image of one BC format (copy_kind 2 and 3), regions
 * already cut to the levels (clamp_region). The budget decides per image, so the two images UE4
 * streams a texture's levels between (the old one, the new one) can be held differently; skipping
 * the copy left the new image with whatever its memory held (Stray's sky and water, 2026-10-26).
 * From the transcoded one: an unscaled blit, the GPU decoding its blocks into the decoded image's
 * texels (what the source showed). From the decoded one: its texels go to staging, and the
 * transcoder's encoder (bc_decode.comp SRC_RAW) makes the blocks an upload of those colours would
 * have; its texels count against the budget like a transcode's. */
static void copy_mixed(vkb_emu_device *e, VkCommandBuffer cb, vkb_emu_image *s, VkImageLayout sl, vkb_emu_image *d,
                       VkImageLayout dl, int kind, uint32_t n, const VkImageCopy *c)
{
    bcn_state *st = e->bcn;
    const vkb_dispatch *r = &e->dev->real;
    const char *why = !st ? "no BC state" : s->type != d->type ? "images of different types" : NULL;
    if (!why && kind == 2) {
        VkFormatProperties fs, fd;
        r->vkGetPhysicalDeviceFormatProperties(e->dev->physical, s->format, &fs);
        r->vkGetPhysicalDeviceFormatProperties(e->dev->physical, d->format, &fd);
        VkImageBlit *b = NULL;
        if (is_srgb(s->format) != is_srgb(d->format)) why = "one is sRGB, the other not (a blit would convert)";
        else if (!(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            why = "the GPU cannot blit between the two formats";
        else if (!(b = malloc((n ? n : 1) * sizeof(*b)))) why = "out of memory";
        if (why) {
            mixed_skip(why, s, d);
            return;
        }
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++)
            k += blit_region(s, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent, &b[k]);
        if (k) r->vkCmdBlitImage(cb, s->image, sl, d->image, dl, k, b, VK_FILTER_NEAREST);
        free(b);
        copy_count(CP_BLIT);
        VKB_ONCE(VKB_LOG_INFO, "BC emulation: a copy from a transcoded BC image (format %d) into a decoded one (format %d) done as a blit "
                 "(logged once, counted in the BC copies line)", s->format, d->format);
        return;
    }
    int raw = raw_family(d->format);
    uint32_t xf, tb = raw >= 0 ? raw_texel(raw, s->format, &xf) : 0;
    bcn_cb *bc = NULL;
    VkBufferImageCopy *out = NULL;
    region *g = NULL;
    if (!why && !tb) why = "no re-encoder from the decoded format into the transcoded one";
    if (!why && !pipeline(e, raw, level_quality(st, d, c[0].dstSubresource.mipLevel))) why = "the re-encoder did not build";
    if (!why && (!(bc = vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb))) || !(out = malloc((n ? n : 1) * sizeof(*out))) ||
                 !(g = malloc((n ? n : 1) * sizeof(*g)))))
        why = "out of memory";
    if (why) {
        free(out);
        free(g);
        mixed_skip(why, s, d);
        return;
    }
    int is3d = s->type == VK_IMAGE_TYPE_3D;
    VkDeviceSize total = 0;
    uint64_t texels = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = c[i].extent.width, h = c[i].extent.height, slices = is3d ? c[i].extent.depth : c[i].srcSubresource.layerCount;
        if (!w || !h || !slices) continue;
        uint32_t rl = (w + 3) & ~3u, ih = (h + 3) & ~3u;
        out[k] = (VkBufferImageCopy){total, rl, ih, c[i].srcSubresource, c[i].srcOffset, {w, h, is3d ? slices : 1}};
        VkImageSubresourceLayers ds = c[i].dstSubresource;
        if (!is3d) ds.layerCount = slices;
        g[k++] = (region){total, rl, ih, ds, c[i].dstOffset, {w, h, is3d ? slices : 1}};
        total += (VkDeviceSize)rl * ih * slices * tb;
        total = (total + SLOT_ALIGN - 1) & ~(VkDeviceSize)(SLOT_ALIGN - 1);
        texels += (uint64_t)w * h * slices;
    }
    VkBuffer buf = k ? stage_buffer(e, cb, bc, total) : VK_NULL_HANDLE;
    if (k && !buf) {
        free(out);
        free(g);
        mixed_skip("no memory to stage the texels", s, d);
        return;
    }
    if (k) {
        r->vkCmdCopyImageToBuffer(cb, s->image, sl, buf, k, out);
        decode(e, cb, buf, d, dl, k, g, s);
        budget_charge(st, tx_family(family(d->client_format), d->format), texels);
    }
    free(out);
    free(g);
    copy_count(CP_ENC);
    VKB_ONCE(VKB_LOG_INFO, "BC emulation: a copy from a decoded BC image (format %d) into a transcoded one (format %d) re-encoded "
             "(logged once, counted in the BC copies line)", s->format, d->format);
}

/* Regions cut to the levels and, between ASTC 6x6 / 8x8 images, to block edges: how many are left. */
static uint32_t cut_regions(const vkb_emu_image *s, const vkb_emu_image *d, int kind, VkImageCopy *c, uint32_t n)
{
    uint32_t k = 0, dropped = 0;
    for (uint32_t i = 0; i < n; i++) {
        clamp_region(s, d, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent);
        if (!c[i].extent.width || !c[i].extent.height || !c[i].extent.depth) continue;
        /* a re-encoded copy encodes what it covers, like an upload; a blit has no block edges */
        if (kind >= 2 || on_blocks(s, d, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent)) c[k++] = c[i];
        else dropped++;
    }
    if (dropped) copy_count(CP_SKIP_EDGE);
    return k;
}

int vkb_emu_copy_image(vkb_emu_device *e, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                       const VkImageCopy *regions)
{
    vkb_emu_image *s = bc_image(e, src), *d = bc_image(e, dst);
    if (!s && !d) return 0;
    if (!s) {
        copy_blocks_in(e, cb, src, sl, d, dl, n, regions);
        return 1;
    }
    int kind = copy_kind(s, d);
    if (kind < 0) {
        if (kind == -1) skipped_copy();
        else copy_count(CP_SKIP_BLOCKS);
        return 1;
    }
    VkImageCopy *c = (VkImageCopy *)regions;
    n = cut_regions(s, d, kind, c, n);
    if (!n) return 1;
    if (kind >= 2) {
        copy_mixed(e, cb, s, sl, d, dl, kind, n, c);
        return 1;
    }
    if (!kind) {
        e->dev->real.vkCmdCopyImage(cb, src, sl, dst, dl, n, regions);
        return 1;
    }
    VkImageBlit *b = malloc((n ? n : 1) * sizeof(*b));
    uint32_t k = 0;
    for (uint32_t i = 0; b && i < n; i++)
        k += blit_region(s, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent, &b[k]);
    if (k) e->dev->real.vkCmdBlitImage(cb, src, sl, dst, dl, k, b, VK_FILTER_NEAREST);
    free(b);
    return 1;
}

int vkb_emu_copy_image2(vkb_emu_device *e, VkCommandBuffer cb, const VkCopyImageInfo2 *info)
{
    vkb_emu_image *s = bc_image(e, info->srcImage), *d = bc_image(e, info->dstImage);
    if (!s && !d) return 0;
    VkImageCopy *r = malloc((info->regionCount ? info->regionCount : 1) * sizeof(*r));
    if (!r) return 1;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkImageCopy2 *q = &info->pRegions[i];
        r[i] = (VkImageCopy){q->srcSubresource, q->srcOffset, q->dstSubresource, q->dstOffset, q->extent};
    }
    int kind = s ? copy_kind(s, d) : -3;
    if (kind == 0) {
        /* as recorded (the regions' pNext kept), cut to the levels and block edges */
        VkImageCopy2 *c = (VkImageCopy2 *)info->pRegions;
        uint32_t k = 0, dropped = 0;
        for (uint32_t i = 0; i < info->regionCount; i++) {
            clamp_region(s, d, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent);
            if (!c[i].extent.width || !c[i].extent.height || !c[i].extent.depth) continue;
            if (on_blocks(s, d, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent)) c[k++] = c[i];
            else dropped++;
        }
        if (dropped) copy_count(CP_SKIP_EDGE);
        ((VkCopyImageInfo2 *)info)->regionCount = k;
        if (k) e->dev->real.vkCmdCopyImage2(cb, info);
    } else {
        vkb_emu_copy_image(e, cb, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout, info->regionCount, r);
    }
    free(r);
    return 1;
}

int vkb_emu_bcn_blocks_readback(vkb_emu_device *e, VkImage img)
{
    if (!bc_image(e, img)) return 0;
    copy_count(CP_SKIP_READBACK);
    VKB_ONCE(VKB_LOG_WARN, "BC emulation: reading a BC image back to a buffer is not supported (logged once)");
    return 1;
}

/* ------------------------------------------------------------------ blocks drawn into uncompressed views */

static bcsink *sk(void)
{
    vkb_emu_device *e = vkb_emu_cur();
    return e ? e->bcsink : NULL;
}

static int sk_live(const bcsink *g) { return g && __atomic_load_n(&g->nviews, __ATOMIC_RELAXED) != 0; }

static VkRect2D rect_union(VkRect2D a, VkRect2D b)
{
    if (!a.extent.width || !a.extent.height) return b;
    if (!b.extent.width || !b.extent.height) return a;
    int32_t x0 = a.offset.x < b.offset.x ? a.offset.x : b.offset.x, y0 = a.offset.y < b.offset.y ? a.offset.y : b.offset.y;
    int64_t ax1 = (int64_t)a.offset.x + a.extent.width, bx1 = (int64_t)b.offset.x + b.extent.width;
    int64_t ay1 = (int64_t)a.offset.y + a.extent.height, by1 = (int64_t)b.offset.y + b.extent.height;
    int64_t x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
    return (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

static VkRect2D rect_meet(VkRect2D a, VkRect2D b)
{
    int64_t x0 = a.offset.x > b.offset.x ? a.offset.x : b.offset.x, y0 = a.offset.y > b.offset.y ? a.offset.y : b.offset.y;
    int64_t ax1 = (int64_t)a.offset.x + a.extent.width, bx1 = (int64_t)b.offset.x + b.extent.width;
    int64_t ay1 = (int64_t)a.offset.y + a.extent.height, by1 = (int64_t)b.offset.y + b.extent.height;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

/* The framebuffer texels viewports can reach (a negative height flips; a fraction reaches the texel it is in). */
static VkRect2D viewports_rect(uint32_t n, const VkViewport *v)
{
    VkRect2D r = {{0, 0}, {0, 0}};
    for (uint32_t i = 0; v && i < n; i++) {
        float x0 = v[i].x, x1 = v[i].x + v[i].width, y0 = v[i].y, y1 = v[i].y + v[i].height;
        if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 <= x0 || y1 <= y0 || x0 > 65536.0f || y0 > 65536.0f) continue;
        int32_t ix0 = (int32_t)x0, iy0 = (int32_t)y0;
        float cx = x1 > 65536.0f ? 65536.0f : x1, cy = y1 > 65536.0f ? 65536.0f : y1;
        int32_t ix1 = (int32_t)cx, iy1 = (int32_t)cy;
        if ((float)ix1 < cx) ix1++;
        if ((float)iy1 < cy) iy1++;
        r = rect_union(r, (VkRect2D){{ix0, iy0}, {(uint32_t)(ix1 - ix0), (uint32_t)(iy1 - iy0)}});
    }
    return r;
}

static int rect_inside(VkRect2D a, VkRect2D b) /* a within b */
{
    return a.offset.x >= b.offset.x && a.offset.y >= b.offset.y &&
           (int64_t)a.offset.x + a.extent.width <= (int64_t)b.offset.x + b.extent.width &&
           (int64_t)a.offset.y + a.extent.height <= (int64_t)b.offset.y + b.extent.height;
}

/* A rect the pass may draw in: kept apart from the others (a bounding box would take in what lies between them,
 * which the sink does not hold), up to 8; past that, one bounding box. */
static void note_drawn(bcn_cb *c, VkRect2D r)
{
    r = rect_meet(r, c->area);
    if (!r.extent.width) return;
    for (uint32_t i = 0; i < c->ndrawn; i++)
        if (rect_inside(r, c->drawn[i])) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < c->ndrawn; i++)
        if (!rect_inside(c->drawn[i], r)) c->drawn[k++] = c->drawn[i];
    c->ndrawn = k;
    if (c->ndrawn < 8) {
        c->drawn[c->ndrawn++] = r;
        return;
    }
    VkRect2D u = r;
    for (uint32_t i = 0; i < c->ndrawn; i++) u = rect_union(u, c->drawn[i]);
    c->drawn[0] = u;
    c->ndrawn = 1;
}

static void note_viewports(VkCommandBuffer cb, uint32_t n, const VkViewport *v)
{
    vkb_emu_device *e = vkb_emu_cur();
    bcn_cb *c = vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb));
    if (!c) return;
    c->vp = viewports_rect(n, v);
    c->vp_known = 1;
    if (c->npass) {
        note_drawn(c, c->vp);
        c->vp_in = 1;
    }
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    bcsink *g = sk();
    if (sk_live(g)) note_viewports(cb, n, v);
    g->set_vp(cb, first, n, v);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v)
{
    bcsink *g = sk();
    if (sk_live(g)) note_viewports(cb, n, v);
    g->set_vp_count(cb, n, v);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment *a, uint32_t nr,
                                                        const VkClearRect *rects)
{
    bcsink *g = sk();
    if (sk_live(g)) {
        bcn_cb *c = vkb_emu_cb_bcn(vkb_emu_cur(), cb, sizeof(bcn_cb));
        for (uint32_t i = 0; c && c->npass && i < nr; i++) {
            note_drawn(c, rects[i].rect);
            c->clear_in = 1;
        }
    }
    g->clear_att(cb, n, a, nr, rects);
}

/* One attachment of the render pass instance about to begin: a sink view (copied out, lock held by the caller's
 * caller) with what the pass does with it. */
typedef struct pass_att {
    sink_view v;
    rp_att a;
} pass_att;

/* Before the render pass instance begins: what it draws into sinks is noted, and each such sink is put in the
 * layout the pass starts it in (its content before the pass is not put back anywhere, so it is discarded). */
static void pass_begin(VkCommandBuffer cb, const pass_att *at, uint32_t n, VkRect2D area, int whole)
{
    vkb_emu_device *e = vkb_emu_cur();
    bcn_cb *c = n ? vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb)) : NULL;
    if (!c) return;
    c->npass = 0;
    c->area = area;
    c->whole = whole;
    c->ndrawn = 0;
    c->vp_in = c->clear_in = 0;
    c->pre = c->vp;
    c->pre_known = c->vp_known;
    VkImageMemoryBarrier b[4];
    uint32_t nb = 0;
    for (uint32_t i = 0; i < n && c->npass < 4; i++) {
        c->pass[c->npass++] = (struct pass_sink){at[i].v.bc, at[i].v.sink, at[i].v.layer0, at[i].v.layers, at[i].a.final};
        if (at[i].a.load == VK_ATTACHMENT_LOAD_OP_CLEAR) c->whole = 1;
        if (at[i].a.initial != VK_IMAGE_LAYOUT_UNDEFINED)
            b[nb++] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                                             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                             at[i].a.initial, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, at[i].v.sink->image,
                                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, at[i].v.layer0, at[i].v.layers}};
    }
    if (n > 4) VKB_ONCE(VKB_LOG_WARN, "BC emulation: a render pass draws into more than 4 uncompressed views of BC images; the first 4 go "
                                      "into their textures (logged once)");
    if (nb)
        e->dev->real.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0,
                                          NULL, nb, b);
}

/* What was drawn into the sink of p (region: blocks of its level) transcoded (decoded) into the texture, as an upload
 * of those blocks would be. Recorded after the render pass instance, the texture and the sink left in the layout
 * the pass left the attachment in. */
static void sink_writeback(vkb_emu_device *e, VkCommandBuffer cb, const struct pass_sink *p, VkRect2D rg)
{
    bc_sink *k = p->sink;
    vkb_emu_image *img = e->bcn ? vkb_emu_find_image(e, p->bc) : NULL;
    if (!img || !k) return;
    rg = rect_meet(rg, (VkRect2D){{0, 0}, {k->bw, k->bh}});
    if (!rg.extent.width || !p->layers) return;
    uint32_t bb = vkb_bc_block_bytes(img->client_format);
    if (block_texel_bytes(k->format) != bb) return;
    bcn_cb *c = vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb));
    if (!c) return;
    VkDeviceSize total = (VkDeviceSize)rg.extent.width * rg.extent.height * p->layers * bb;
    total = (total + SLOT_ALIGN - 1) & ~(VkDeviceSize)(SLOT_ALIGN - 1);
    VkBuffer buf = stage_buffer(e, cb, c, total);
    if (!buf) {
        VKB_ONCE(VKB_LOG_ERROR, "BC emulation: no memory to stage %llu bytes of blocks drawn into an uncompressed view (logged once)",
                 (unsigned long long)total);
        return;
    }
    const vkb_dispatch *r = &e->dev->real;
    VkImageMemoryBarrier b[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, p->fin,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, k->image,
         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, p->layer0, p->layers}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, p->fin,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img->image,
         {VK_IMAGE_ASPECT_COLOR_BIT, k->level, 1, p->layer0, p->layers}}};
    r->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
    VkBufferImageCopy out = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, p->layer0, p->layers}, {rg.offset.x, rg.offset.y, 0},
                             {rg.extent.width, rg.extent.height, 1}};
    r->vkCmdCopyImageToBuffer(cb, k->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &out);
    region g = {0, rg.extent.width * 4, rg.extent.height * 4, {VK_IMAGE_ASPECT_COLOR_BIT, k->level, p->layer0, p->layers},
                {rg.offset.x * 4, rg.offset.y * 4, 0}, {rg.extent.width * 4, rg.extent.height * 4, 1}};
    decode(e, cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &g, NULL);
    b[0] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->fin, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, k->image,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, p->layer0, p->layers}};
    b[1] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, p->fin,
                                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img->image,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, k->level, 1, p->layer0, p->layers}};
    r->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, b);
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
        VKB_INFO("BC emulation: blocks drawn into an uncompressed view (format %d) of a BC image (format %d, held as %d) put into the "
                 "texture: %ux%u blocks at %d,%d of level %u, %u layer(s) (logged for the first few)", k->format, img->client_format,
                 img->format, rg.extent.width, rg.extent.height, rg.offset.x, rg.offset.y, k->level, p->layers);
}

/* After the render pass instance: its draws into sinks put into the textures. */
static void pass_end(VkCommandBuffer cb)
{
    vkb_emu_device *e = vkb_emu_cur();
    bcn_cb *c = vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb));
    if (!c || !c->npass) return;
    /* What it drew: the renderArea where a clear at load or secondary command buffers write it all; else the viewports
     * set in it and the rects cleared in it; with neither, the viewport its draws had from before it began (Zink sets
     * one in the pass when it changed, so the one before is the one its draws use), or the renderArea if none is
     * known. A pass that clears rects and draws with a viewport from before it gets only the rects (no draw is followed
     * one by one: what is put back is never more than what was drawn there). */
    if (!c->whole && !c->vp_in && !c->clear_in) {
        if (c->pre_known) note_drawn(c, c->pre);
        else c->whole = 1;
    }
    int all = c->whole; /* else the rects (none: the viewport from before lies outside the renderArea, nothing drawn) */
    for (uint32_t i = 0; i < c->npass; i++)
        for (uint32_t j = 0; j < (all ? 1 : c->ndrawn); j++) sink_writeback(e, cb, &c->pass[i], all ? c->area : c->drawn[j]);
    c->npass = 0;
}

static const rp_info *rp_find(bcsink *g, VkRenderPass rp)
{
    for (rp_info *x = g->rps[SK_HASH(rp)]; x; x = x->next)
        if (x->rp == rp) return x;
    return NULL;
}

/* The sink views among a render pass instance's attachments, with what the pass does with them; how many. */
static uint32_t rp_sink_atts(bcsink *g, const VkRenderPassBeginInfo *bi, pass_att *out, uint32_t max)
{
    const VkRenderPassAttachmentBeginInfo *ab = NULL;
    for (const VkBaseInStructure *b = bi->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) ab = (const VkRenderPassAttachmentBeginInfo *)b;
    uint32_t n = 0;
    pthread_mutex_lock(&g->lock);
    const VkImageView *views = NULL;
    uint32_t nv = 0;
    if (ab) {
        views = ab->pAttachments;
        nv = ab->attachmentCount;
    } else if (__atomic_load_n(&g->nfbs, __ATOMIC_RELAXED)) {
        for (fb_info *x = g->fbs[SK_HASH(bi->framebuffer)]; x; x = x->next)
            if (x->fb == bi->framebuffer) {
                views = x->views;
                nv = x->n;
                break;
            }
    }
    const rp_info *rp = views ? rp_find(g, bi->renderPass) : NULL;
    for (uint32_t i = 0; rp && i < nv && i < rp->n && n < max; i++)
        if (sink_view_find(g, views[i], &out[n].v)) {
            out[n].a = rp->att[i];
            if (out[n].a.store == VK_ATTACHMENT_STORE_OP_STORE) n++;
        }
    pthread_mutex_unlock(&g->lock);
    return n;
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents contents)
{
    bcsink *g = sk();
    if (sk_live(g)) {
        pass_att at[8];
        uint32_t n = rp_sink_atts(g, bi, at, 8);
        if (n) pass_begin(cb, at, n, bi->renderArea, contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    }
    g->begin_rp(cb, bi, contents);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *si)
{
    bcsink *g = sk();
    if (sk_live(g)) {
        pass_att at[8];
        uint32_t n = rp_sink_atts(g, bi, at, 8);
        if (n) pass_begin(cb, at, n, bi->renderArea, si && si->contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    }
    g->begin_rp2(cb, bi, si);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdEndRenderPass(VkCommandBuffer cb)
{
    bcsink *g = sk();
    g->end_rp(cb);
    if (sk_live(g)) pass_end(cb);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *ei)
{
    bcsink *g = sk();
    g->end_rp2(cb, ei);
    if (sk_live(g)) pass_end(cb);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    bcsink *g = sk();
    if (sk_live(g)) {
        pass_att at[8];
        uint32_t n = 0;
        pthread_mutex_lock(&g->lock);
        for (uint32_t i = 0; i < ri->colorAttachmentCount && n < 8; i++) {
            const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
            if (a->imageView && a->storeOp == VK_ATTACHMENT_STORE_OP_STORE && sink_view_find(g, a->imageView, &at[n].v)) {
                at[n].a = (rp_att){a->loadOp, a->storeOp, a->imageLayout, a->imageLayout};
                n++;
            }
        }
        pthread_mutex_unlock(&g->lock);
        if (n && (ri->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT))) {
            VKB_ONCE(VKB_LOG_WARN, "BC emulation: a suspended or resumed rendering draws into an uncompressed view of a BC image; not put "
                                   "into the texture (logged once)");
            n = 0;
        }
        if (n) pass_begin(cb, at, n, ri->renderArea, (ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) != 0);
    }
    g->begin_r(cb, ri);
}

static VKAPI_ATTR void VKAPI_CALL sk_CmdEndRendering(VkCommandBuffer cb)
{
    bcsink *g = sk();
    g->end_r(cb);
    if (sk_live(g)) pass_end(cb);
}

static void rp_add(bcsink *g, VkRenderPass rp, uint32_t n, const rp_att *att)
{
    rp_info *x = calloc(1, sizeof(*x));
    rp_att *a = calloc(n ? n : 1, sizeof(*a));
    if (!x || !a) {
        free(x);
        free(a);
        return;
    }
    memcpy(a, att, n * sizeof(*a));
    *x = (rp_info){rp, n, a, NULL};
    pthread_mutex_lock(&g->lock);
    x->next = g->rps[SK_HASH(rp)];
    g->rps[SK_HASH(rp)] = x;
    pthread_mutex_unlock(&g->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL sk_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkRenderPass *rp)
{
    bcsink *g = sk();
    VkResult r = g->create_rp(device, ci, a, rp);
    if (r == VK_SUCCESS && ci->attachmentCount <= 64) {
        rp_att at[64];
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            at[i] = (rp_att){ci->pAttachments[i].loadOp, ci->pAttachments[i].storeOp, ci->pAttachments[i].initialLayout,
                             ci->pAttachments[i].finalLayout};
        rp_add(g, *rp, ci->attachmentCount, at);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL sk_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a,
                                                           VkRenderPass *rp)
{
    bcsink *g = sk();
    VkResult r = g->create_rp2(device, ci, a, rp);
    if (r == VK_SUCCESS && ci->attachmentCount <= 64) {
        rp_att at[64];
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            at[i] = (rp_att){ci->pAttachments[i].loadOp, ci->pAttachments[i].storeOp, ci->pAttachments[i].initialLayout,
                             ci->pAttachments[i].finalLayout};
        rp_add(g, *rp, ci->attachmentCount, at);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL sk_DestroyRenderPass(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks *a)
{
    bcsink *g = sk();
    if (rp) {
        pthread_mutex_lock(&g->lock);
        for (rp_info **pp = &g->rps[SK_HASH(rp)]; *pp; pp = &(*pp)->next)
            if ((*pp)->rp == rp) {
                rp_info *x = *pp;
                *pp = x->next;
                free(x->att);
                free(x);
                break;
            }
        pthread_mutex_unlock(&g->lock);
    }
    g->destroy_rp(device, rp, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL sk_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a,
                                                           VkFramebuffer *fb)
{
    bcsink *g = sk();
    VkResult r = g->create_fb(device, ci, a, fb);
    if (r != VK_SUCCESS || !sk_live(g) || (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) || !ci->attachmentCount) return r;
    int any = 0;
    sink_view tmp;
    pthread_mutex_lock(&g->lock);
    for (uint32_t i = 0; i < ci->attachmentCount && !any; i++) any = sink_view_find(g, ci->pAttachments[i], &tmp);
    pthread_mutex_unlock(&g->lock);
    if (!any) return r;
    fb_info *x = calloc(1, sizeof(*x));
    VkImageView *v = calloc(ci->attachmentCount, sizeof(*v));
    if (!x || !v) {
        free(x);
        free(v);
        return r;
    }
    memcpy(v, ci->pAttachments, ci->attachmentCount * sizeof(*v));
    *x = (fb_info){*fb, ci->attachmentCount, v, NULL};
    pthread_mutex_lock(&g->lock);
    x->next = g->fbs[SK_HASH(*fb)];
    g->fbs[SK_HASH(*fb)] = x;
    __atomic_add_fetch(&g->nfbs, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL sk_DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *a)
{
    bcsink *g = sk();
    if (fb && __atomic_load_n(&g->nfbs, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&g->lock);
        for (fb_info **pp = &g->fbs[SK_HASH(fb)]; *pp; pp = &(*pp)->next)
            if ((*pp)->fb == fb) {
                fb_info *x = *pp;
                *pp = x->next;
                free(x->views);
                free(x);
                __atomic_sub_fetch(&g->nfbs, 1, __ATOMIC_RELAXED);
                break;
            }
        pthread_mutex_unlock(&g->lock);
    }
    g->destroy_fb(device, fb, a);
}

static VKAPI_ATTR void VKAPI_CALL sk_DestroyImageView(VkDevice device, VkImageView v, const VkAllocationCallbacks *a)
{
    bcsink *g = sk();
    if (v) sink_views_drop(g, v, VK_NULL_HANDLE);
    g->destroy_view(device, v, a);
}

/* The hooks, over what the device's table has (the errata fixes beneath, the emulations installed later over them:
 * the dynamic rendering emulation makes its render passes and framebuffers through these). */
static void sink_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    vkb_dispatch *dt = &dev->dt;
    if (e->bcsink || !dt->vkCreateRenderPass || !dt->vkDestroyRenderPass || !dt->vkCreateFramebuffer || !dt->vkDestroyFramebuffer ||
        !dt->vkDestroyImageView || !dt->vkCmdBeginRenderPass || !dt->vkCmdEndRenderPass || !dt->vkCmdSetViewport || !dt->vkCmdClearAttachments)
        return;
    bcsink *g = calloc(1, sizeof(*g));
    if (!g) return;
    pthread_mutex_init(&g->lock, NULL);
    g->create_rp = dt->vkCreateRenderPass;
    g->destroy_rp = dt->vkDestroyRenderPass;
    g->create_fb = dt->vkCreateFramebuffer;
    g->destroy_fb = dt->vkDestroyFramebuffer;
    g->destroy_view = dt->vkDestroyImageView;
    g->begin_rp = dt->vkCmdBeginRenderPass;
    g->end_rp = dt->vkCmdEndRenderPass;
    g->set_vp = dt->vkCmdSetViewport;
    g->clear_att = dt->vkCmdClearAttachments;
    g->create_rp2 = dt->vkCreateRenderPass2;
    g->begin_rp2 = dt->vkCmdBeginRenderPass2;
    g->end_rp2 = dt->vkCmdEndRenderPass2;
    g->begin_r = dt->vkCmdBeginRendering;
    g->end_r = dt->vkCmdEndRendering;
    g->set_vp_count = dt->vkCmdSetViewportWithCount;
    e->bcsink = g;
    dt->vkCreateRenderPass = sk_CreateRenderPass;
    dt->vkDestroyRenderPass = sk_DestroyRenderPass;
    dt->vkCreateFramebuffer = sk_CreateFramebuffer;
    dt->vkDestroyFramebuffer = sk_DestroyFramebuffer;
    dt->vkDestroyImageView = sk_DestroyImageView;
    dt->vkCmdBeginRenderPass = sk_CmdBeginRenderPass;
    dt->vkCmdEndRenderPass = sk_CmdEndRenderPass;
    dt->vkCmdSetViewport = sk_CmdSetViewport;
    dt->vkCmdClearAttachments = sk_CmdClearAttachments;
    if (g->create_rp2) dt->vkCreateRenderPass2 = sk_CreateRenderPass2;
    if (g->begin_rp2 && g->end_rp2) {
        dt->vkCmdBeginRenderPass2 = sk_CmdBeginRenderPass2;
        dt->vkCmdEndRenderPass2 = sk_CmdEndRenderPass2;
    }
    if (g->begin_r && g->end_r) {
        dt->vkCmdBeginRendering = sk_CmdBeginRendering;
        dt->vkCmdEndRendering = sk_CmdEndRendering;
    }
    if (g->set_vp_count) dt->vkCmdSetViewportWithCount = sk_CmdSetViewportWithCount;
}

void vkb_emu_bcsink_free(vkb_emu_device *e)
{
    bcsink *g = e ? e->bcsink : NULL;
    if (!g) return;
    e->bcsink = NULL;
    for (uint32_t b = 0; b < SK_BUCKETS; b++) {
        while (g->views[b]) {
            sink_view *x = g->views[b];
            g->views[b] = x->next;
            free(x);
        }
        while (g->rps[b]) {
            rp_info *x = g->rps[b];
            g->rps[b] = x->next;
            free(x->att);
            free(x);
        }
        while (g->fbs[b]) {
            fb_info *x = g->fbs[b];
            g->fbs[b] = x->next;
            free(x->views);
            free(x);
        }
    }
    pthread_mutex_destroy(&g->lock);
    free(g);
}

/* ------------------------------------------------------------------ install */

void vkb_emu_bcn_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    if (!(e->flags & VKB_EMU_BCN)) return;
    bcn_state *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->plock, NULL);
    pthread_cond_init(&s->pcond, NULL);
    VkPhysicalDeviceProperties p;
    dev->real.vkGetPhysicalDeviceProperties(dev->physical, &p);
    s->ssbo_align = p.limits.minStorageBufferOffsetAlignment ? p.limits.minStorageBufferOffsetAlignment : 4;
    dev->real.vkGetPhysicalDeviceMemoryProperties(dev->physical, &s->mp);
    /* What BC1 UNORM images do with their texels; VKBRIDGE_FAKE_MISSING=...,bc16 keeps RGBA8 (host tests). */
    static const VkFormat packed[2] = {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16};
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    for (int i = 0; i < 2; i++) {
        VkFormatProperties fp;
        dev->real.vkGetPhysicalDeviceFormatProperties(dev->physical, packed[i], &fp);
        s->packed_ok[i] = (fp.optimalTilingFeatures & need) == need && !vkb_emu_faked("bc16");
    }
    VKB_INFO("BC emulation: BC1 UNORM decodes to %s / %s", s->packed_ok[0] ? "R5G6B5" : "RGBA8 (no R5G6B5)",
             s->packed_ok[1] ? "A1R5G5B5" : "RGBA8 (no A1R5G5B5)");
    /* Transcoding: the device was created with ETC2 / ASTC LDR / ASTC HDR on where the GPU has them
     * (vkb_emu_device_create_info), and the target must be sampled, filtered and copied. */
    VkPhysicalDeviceFeatures pf;
    dev->real.vkGetPhysicalDeviceFeatures(dev->physical, &pf);
    int hdr = vkb_emu_bcn_astc_hdr(&dev->real, dev->physical, dev->api_version) != 0;
    s->astc_n = astc_block();
    const VkFormatFeatureFlags txneed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    char msg[256], off[96];
    int len = 0, olen = 0;
    msg[0] = off[0] = 0;
    for (VkFormat f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; f++) {
        VkFormat t = tx_target(f, s->astc_n);
        if (!t || !vkb_emu_bcn_transcode_wanted()) continue;
        int cls = tx_class(t);
        if (cls == 6 ? !hdr : cls == 3 || cls >= 7 ? !pf.textureCompressionASTC_LDR : !pf.textureCompressionETC2) continue;
        /* the startup probe could not build this target's transcoder on the driver (server_probe.c) */
        uint32_t blocked = cls <= 2 ? VKB_PROBE_FLAG_TX_ETC2 : cls == 3 || cls >= 7 ? VKB_PROBE_FLAG_TX_ASTC
                                                         : cls == 6 ? VKB_PROBE_FLAG_TX_ASTC_HDR : VKB_PROBE_FLAG_TX_EAC;
        if (family(f) == FAM_BC7) blocked = VKB_PROBE_FLAG_TX_BC7;
        if (vkb_probe_flag(blocked)) {
            if (olen < (int)sizeof(off) - 8) olen += snprintf(off + olen, sizeof(off) - (size_t)olen, " %d", f);
            continue;
        }
        VkFormatProperties fp;
        dev->real.vkGetPhysicalDeviceFormatProperties(dev->physical, t, &fp);
        if ((fp.optimalTilingFeatures & txneed) != txneed) continue;
        s->tx[BC_INDEX(f)] = t;
        /* not needed for textures, but the client reports BC as blittable (client/emu.c) */
        int blit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
        if (len < (int)sizeof(msg) - 24) len += snprintf(msg + len, sizeof(msg) - (size_t)len, " %d->%d%s", f, t, blit ? "" : " (no blit)");
    }
    if (olen)
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: formats%s decoded, not transcoded: the startup probe could not build their transcoder "
                 "(its line in the device profile says why; VKBRIDGE_PROBE=force runs it again)", off);
    s->quality = transcode_quality();
    {
        VkPhysicalDeviceProperties pp;
        dev->real.vkGetPhysicalDeviceProperties(dev->physical, &pp);
        s->auto_rate = budget_auto(auto_name(pp.deviceName), &s->auto_fast, s->auto_why, sizeof(s->auto_why));
        s->auto_burst = s->auto_rate / 8;
    }
    static const char *const qname[4] = {", fast", "", ", high quality", ", auto quality (fast for levels of 1024x1024 texels or more)"};
    if (len) VKB_INFO("BC emulation: transcoding (BC format -> format):%s%s%s", msg, qname[s->quality],
                      s->astc_n == 6 ? ", ASTC 6x6 (VKBRIDGE_BC_ASTC_BLOCK)" : s->astc_n == 8 ? ", ASTC 8x8 (VKBRIDGE_BC_ASTC_BLOCK)" : "");
    else VKB_INFO("BC emulation: no transcoding (%s)", vkb_emu_bcn_transcode_wanted() ? "the GPU samples neither ETC2 nor ASTC" : "VKBRIDGE_BC_TRANSCODE=0");
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    s->push = has_push(dev);
    VKB_INFO("BC emulation: decoder binds with %s", s->push ? "push descriptors" : "descriptor sets (no VK_KHR_push_descriptor)");
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
                                           s->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT : 0, 2, b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc_t)};
    if (dev->real.vkCreateDescriptorSetLayout(dev->device, &dci, NULL, &s->dsl) != VK_SUCCESS) {
        VKB_ERR("BC emulation: cannot create the decoder's descriptor layout");
        pthread_cond_destroy(&s->pcond);
        pthread_mutex_destroy(&s->plock);
        pthread_mutex_destroy(&s->lock);
        free(s);
        e->flags &= ~VKB_EMU_BCN;
        return;
    }
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &s->dsl, 1, &pr};
    dev->real.vkCreatePipelineLayout(dev->device, &plci, NULL, &s->layout);
    {
        const char *bv = vkb_srv_env("VKBRIDGE_BC_BATCH");
        s->batch = !(bv && bv[0] == '0' && !bv[1]);
        VKB_INFO("BC emulation: %s", s->batch ? "uploads recorded back to back share one barrier pair (VKBRIDGE_BC_BATCH=0: a pair each)"
                                               : "a barrier pair per upload (VKBRIDGE_BC_BATCH=0)");
    }
    e->bcn = s;
    dev->dt.vkCreateImageView = emu_CreateImageView;
    sink_install(dev);
}

void vkb_emu_bcn_uninstall(vkb_srv_table *dev)
{
    bcn_state *s = dev->emu->bcn;
    if (!s) return;
    budget_totals();
    copies_line(1);
    uploads_line(1);
    vkb_emu_bcn_quiesce(dev);
    for (int q = 0; q < 3; q++)
        for (int i = 0; i < FAM_COUNT; i++)
            if (s->pipes[q][i]) dev->real.vkDestroyPipeline(dev->device, s->pipes[q][i], NULL);
    dev->real.vkDestroyPipelineLayout(dev->device, s->layout, NULL);
    dev->real.vkDestroyDescriptorSetLayout(dev->device, s->dsl, NULL);
    /* Scratch that command buffers still hold goes with their state (server_emu_cmdstate.c). */
    dev->emu->bcn = NULL;
    for (uint32_t i = 0; i < s->nidle; i++) {
        __atomic_sub_fetch(&g_idle, s->idle[i].size, __ATOMIC_RELAXED);
        chunk_destroy(dev->emu, &s->idle[i]);
    }
    free(s->idle);
    free(s->pin);
    pthread_cond_destroy(&s->pcond);
    pthread_mutex_destroy(&s->plock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

/* ------------------------------------------------------------------ the startup probe */

/* The transcoders (server_probe.c builds each on the driver, under the compile watchdog). */
static const struct {
    int fam;
    const char *name;
    uint32_t flag;
} probe_tx[] = {{TX_BC1, "BC1->ETC2", VKB_PROBE_FLAG_TX_ETC2},    {TX_BC2, "BC2->ASTC", VKB_PROBE_FLAG_TX_ASTC},
                {TX_BC3, "BC3->ASTC", VKB_PROBE_FLAG_TX_ASTC},    {TX_BC4, "BC4->EAC", VKB_PROBE_FLAG_TX_EAC},
                {TX_BC5, "BC5->EAC", VKB_PROBE_FLAG_TX_EAC},      {TX_BC6H, "BC6H->ASTC-HDR", VKB_PROBE_FLAG_TX_ASTC_HDR},
                {TX_BC7, "BC7->ASTC", VKB_PROBE_FLAG_TX_BC7}};

int vkb_emu_bcn_transcoders(void) { return (int)(sizeof(probe_tx) / sizeof(probe_tx[0])); }

const char *vkb_emu_bcn_transcoder_name(int i, uint32_t *flag)
{
    *flag = probe_tx[i].flag;
    return probe_tx[i].name;
}

/* Transcoder i built (at the default quality) and destroyed again, on a device of the probe's,
 * through cache (the probe's own persistent one: a re-probe then does not compile them again). */
VkResult vkb_emu_bcn_build_transcoder(const vkb_dispatch *r, VkDevice d, VkPipelineCache cache, int i, double *ms)
{
    int fam = probe_tx[i].fam;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc_t)};
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkShaderModule m = VK_NULL_HANDLE;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult res = r->vkCreateDescriptorSetLayout(d, &dci, NULL, &dsl);
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pr};
    if (res == VK_SUCCESS) res = r->vkCreatePipelineLayout(d, &plci, NULL, &pl);
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, fam_size[fam], fam_code[fam]};
    if (res == VK_SUCCESS) res = r->vkCreateShaderModule(d, &mci, NULL, &m);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (res == VK_SUCCESS) {
        VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                      m, "main", NULL};
        pci.layout = pl;
        res = r->vkCreateComputePipelines(d, cache, 1, &pci, NULL, &p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (p) r->vkDestroyPipeline(d, p, NULL);
    if (m) r->vkDestroyShaderModule(d, m, NULL);
    if (pl) r->vkDestroyPipelineLayout(d, pl, NULL);
    if (dsl) r->vkDestroyDescriptorSetLayout(d, dsl, NULL);
    return res;
}
