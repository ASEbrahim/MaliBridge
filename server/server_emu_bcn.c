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
 * Not emulated (logged once): reading a BC image back to a buffer, and copies between BC and
 * uncompressed images (they would need an encoder), or between a transcoded and a decoded image.
 */
#define _GNU_SOURCE
#include "server_emu.h"
#include "vkb_emu_formats.h"
#include "bc_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FAM_BC1_16: BC1 UNORM decoded to 16 bits (vkb_bc_packed_format), a decoder variant, not a family;
 * TX_*: the transcoders (TX_BC1 + family). */
enum { FAM_BC1, FAM_BC2, FAM_BC3, FAM_BC4, FAM_BC5, FAM_BC6H, FAM_BC7, FAM_BC1_16, TX_BC1, TX_BC2, TX_BC3, TX_BC4, TX_BC5,
       TX_BC6H, TX_BC7, FAM_COUNT };
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
} bcn_cb;

typedef struct bcn_state {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipes[FAM_COUNT];
    VkDeviceSize ssbo_align;
    int push;                /* VK_KHR_push_descriptor; else per-command-buffer sets */
    VkPhysicalDeviceMemoryProperties mp;
    int packed_ok[2];        /* R5G6B5, A1R5G5B5 can hold BC1 UNORM */
    VkFormat tx[16];         /* per BC format (BC_INDEX): what it is transcoded to, UNDEFINED: decoded */
    pthread_mutex_t lock;
    chunk *idle;             /* scratch no pending command buffer uses (bytes in g_idle) */
    uint32_t nidle, capidle;
} bcn_state;

typedef struct pc_t {
    uint32_t src_word, row_blocks, layer_blocks, flags;
    uint32_t dst_word, pad;
    uint32_t extent[2];
} pc_t;

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
static VkFormat tx_target(VkFormat f)
{
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
    default: return 0;
    }
}

int vkb_emu_bcn_transcode_wanted(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_BC_TRANSCODE"); /* <cache dir>/env too, read at device creation */
    return !v || strcmp(v, "0");
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
    const char *fake = getenv("VKBRIDGE_FAKE_MISSING");
    if (fake && strstr(fake, "push")) return 0;
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
static VkFormat tx_format(vkb_emu_device *e, const VkImageCreateInfo *ci, const VkImageFormatListCreateInfo *fl)
{
    const bcn_state *s = e->bcn;
    if (!s) return VK_FORMAT_UNDEFINED;
    VkFormat t = s->tx[BC_INDEX(ci->format)];
    const VkImageUsageFlags texture = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!t || (ci->usage & ~texture)) return VK_FORMAT_UNDEFINED;
    if ((ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && fl)
        for (uint32_t i = 0; i < fl->viewFormatCount; i++) {
            VkFormat v = fl->pViewFormats[i];
            if (!vkb_is_bc(v) || tx_class(s->tx[BC_INDEX(v)]) != tx_class(t)) return VK_FORMAT_UNDEFINED;
        }
    const vkb_dispatch *r = &e->dev->real;
    VkImageFormatProperties ip;
    VkImageCreateFlags flags = ci->flags & ~(VkImageCreateFlags)(VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
    if (!r->vkGetPhysicalDeviceImageFormatProperties ||
        r->vkGetPhysicalDeviceImageFormatProperties(e->dev->physical, t, ci->imageType, ci->tiling, ci->usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                                    flags, &ip) != VK_SUCCESS ||
        ci->extent.width > ip.maxExtent.width || ci->extent.height > ip.maxExtent.height || ci->extent.depth > ip.maxExtent.depth ||
        ci->mipLevels > ip.maxMipLevels || ci->arrayLayers > ip.maxArrayLayers) {
        VKB_ONCE(VKB_LOG_INFO, "BC emulation: the GPU takes no %ux%ux%u image of type %d in format %d; BC images like it are decoded "
                 "(logged once)", ci->extent.width, ci->extent.height, ci->extent.depth, ci->imageType, t);
        return VK_FORMAT_UNDEFINED;
    }
    return t;
}

void vkb_emu_image_create_info(vkb_emu_device *e, VkImageCreateInfo *ci)
{
    if (!(e->flags & VKB_EMU_BCN) || !vkb_is_bc(ci->format)) return;
    /* The image keeps the meaning of the app's format (sRGB, SNORM) so blits and copies convert as
     * they would, and nothing but the app's usage plus TRANSFER_DST for the decoded uploads. A
     * mutable image is made mutable only among the decoded formats its views can map to (a format
     * list, which lets Mali keep AFBC where blanket MUTABLE_FORMAT may not); one format, not mutable. */
    static __thread VkFormat list[8];
    static __thread VkImageFormatListCreateInfo own;
    VkImageFormatListCreateInfo *fl = NULL;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) fl = (VkImageFormatListCreateInfo *)b;
    VkFormat tx = tx_format(e, ci, fl);
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
    ci->format = view;
    ci->flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
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

void vkb_emu_image_release(vkb_emu_device *e, vkb_emu_image *img)
{
    (void)e;
    (void)img; /* nothing kept per image */
}

VkFormat vkb_emu_bcn_view_format(vkb_emu_device *e, VkImage image, VkFormat requested)
{
    if (!(e->flags & VKB_EMU_BCN)) return requested;
    vkb_emu_image *r = vkb_emu_find_image(e, image);
    int bc = r && vkb_is_bc(r->client_format);
    /* A 16-bit image has no other view formats (packed_format); a transcoded one only those of its
     * class (tx_format). */
    if (vkb_is_bc(requested) && bc && vkb_tx_block_bytes(r->format)) {
        VkFormat t = tx_target(requested);
        return tx_class(t) == tx_class(r->format) ? t : r->format;
    }
    if (vkb_is_bc(requested)) return bc && vkb_bc_is_packed(r->format) ? r->format : vkb_bc_view_format(requested);
    if (bc) {
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: an uncompressed view (format %d) of a BC image shows decoded texels (logged once)", requested);
        return r->format;
    }
    return requested;
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                         const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    VkImageViewCreateInfo *v = (VkImageViewCreateInfo *)ci;
    v->format = vkb_emu_bcn_view_format(vkb_emu_cur(), ci->image, ci->format);
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
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
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

void vkb_emu_bcn_cb_release(vkb_emu_device *e, void *cb_scratch)
{
    bcn_cb *c = cb_scratch;
    if (!c) return;
    bcn_state *s = e->bcn;
    for (uint32_t i = 0; i < c->n; i++) {
        int keep = 0;
        if (s && idle_take(c->held[i].size)) {
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
                s->idle[s->nidle++] = c->held[i];
                keep = 1;
            }
            pthread_mutex_unlock(&s->lock);
            if (!keep) __atomic_sub_fetch(&g_idle, c->held[i].size, __ATOMIC_RELAXED);
        }
        if (!keep) chunk_destroy(e, &c->held[i]);
    }
    free(c->held);
    memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------------ decoder */

static VkPipeline pipeline(vkb_emu_device *e, int fam)
{
    bcn_state *s = e->bcn;
    if (s->pipes[fam]) return s->pipes[fam];
    pthread_mutex_lock(&s->lock);
    if (!s->pipes[fam]) {
        static const uint32_t *const code[FAM_COUNT] = {spv_bc1, spv_bc2, spv_bc3, spv_bc4, spv_bc5, spv_bc6h, spv_bc7, spv_bc1_16,
                                                        spv_bc1_etc2, spv_bc2_astc, spv_bc3_astc, spv_bc4_eac, spv_bc5_eac, spv_bc6h_astc,
                                                        spv_bc7_astc};
        static const size_t size[FAM_COUNT] = {sizeof(spv_bc1), sizeof(spv_bc2), sizeof(spv_bc3), sizeof(spv_bc4), sizeof(spv_bc5),
                                               sizeof(spv_bc6h), sizeof(spv_bc7), sizeof(spv_bc1_16), sizeof(spv_bc1_etc2),
                                               sizeof(spv_bc2_astc), sizeof(spv_bc3_astc), sizeof(spv_bc4_eac), sizeof(spv_bc5_eac),
                                               sizeof(spv_bc6h_astc), sizeof(spv_bc7_astc)};
        const vkb_dispatch *r = &e->dev->real;
        VkDevice d = e->dev->device;
        VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size[fam], code[fam]};
        VkShaderModule m;
        if (code[fam] && r->vkCreateShaderModule(d, &mci, NULL, &m) == VK_SUCCESS) {
            VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                          VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
            pci.layout = s->layout;
            if (r->vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pci, NULL, &s->pipes[fam]) != VK_SUCCESS) {
                VKB_ERR("BC emulation: decoder pipeline %d failed to build", fam);
                s->pipes[fam] = VK_NULL_HANDLE;
            }
            r->vkDestroyShaderModule(d, m, NULL);
        }
    }
    pthread_mutex_unlock(&s->lock);
    return s->pipes[fam];
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
    VkDeviceSize left;       /* scratch the rest of the upload needs (sizes new chunks) */
    VkBuffer *bufs;          /* decoded, waiting to be copied into the image */
    VkBufferImageCopy *copies;
    uint32_t n, cap;
} upload;

/* Earlier transfers (the app's writes to the source, earlier copies out of this scratch) before the
 * decoder reads the source and writes the scratch. */
static void to_compute(upload *u)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    u->e->dev->real.vkCmdPipelineBarrier(u->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL,
                                         0, NULL);
}

/* Copies what is decoded into the image, in the layout the app recorded its copy with. */
static void flush(upload *u)
{
    if (!u->n) return;
    const vkb_dispatch *r = &u->e->dev->real;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    r->vkCmdPipelineBarrier(u->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    for (uint32_t i = 0; i < u->n;) {
        uint32_t k = i + 1;
        while (k < u->n && u->bufs[k] == u->bufs[i]) k++;
        r->vkCmdCopyBufferToImage(u->cb, u->bufs[i], u->img->image, u->layout, k - i, &u->copies[i]);
        i = k;
    }
    u->n = 0;
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
    pc_t pc = {(uint32_t)((first - aligned) / 4), row_blocks, layer_blocks, family_flags(u->img),
               (uint32_t)(off / 4), 0, {g->iext.width, h}};
    r->vkCmdPushConstants(u->cb, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    r->vkCmdDispatch(u->cb, (bw + 7) / 8, (nrows + 7) / 8, nslices);

    /* The copy, within the mip level (a region may name whole blocks past a small mip's edge). */
    const vkb_emu_image *img = u->img;
    uint32_t mip = g->sub.mipLevel;
    uint32_t mw = img->extent.width >> mip, mh = img->extent.height >> mip;
    if (!mw) mw = 1;
    if (!mh) mh = 1;
    VkBufferImageCopy c = {off, bw * 4, nrows * 4, g->sub, {g->ioff.x, g->ioff.y + (int32_t)y0, g->ioff.z}, {g->iext.width, h, 1}};
    if (img->type == VK_IMAGE_TYPE_3D) {
        c.imageOffset.z += (int32_t)slice0;
        c.imageExtent.depth = nslices;
    } else {
        c.imageSubresource.baseArrayLayer += slice0;
        c.imageSubresource.layerCount = nslices;
    }
    if ((uint32_t)c.imageOffset.x >= mw || (uint32_t)c.imageOffset.y >= mh) return;
    if (c.imageOffset.x + c.imageExtent.width > mw) c.imageExtent.width = mw - (uint32_t)c.imageOffset.x;
    if (c.imageOffset.y + c.imageExtent.height > mh) c.imageExtent.height = mh - (uint32_t)c.imageOffset.y;
    if (u->n == u->cap) {
        uint32_t cap = u->cap ? u->cap * 2 : 16;
        VkBuffer *nb = realloc(u->bufs, cap * sizeof(*nb));
        if (nb) u->bufs = nb;
        VkBufferImageCopy *nc = realloc(u->copies, cap * sizeof(*nc));
        if (nc) u->copies = nc;
        if (!nb || !nc) return;
        u->cap = cap;
    }
    u->bufs[u->n] = buf;
    u->copies[u->n++] = c;
}

static VkDeviceSize region_bytes(const upload *u, const region *g, uint32_t slices)
{
    return (VkDeviceSize)((g->iext.width + 3) / 4) * ((g->iext.height + 3) / 4) * u->ob * slices;
}

static void decode(vkb_emu_device *e, VkCommandBuffer cb, VkBuffer src, vkb_emu_image *img, VkImageLayout layout,
                   uint32_t n, const region *regs)
{
    int fam = family(img->client_format);
    VkPipeline pipe = pipeline(e, vkb_tx_block_bytes(img->format) ? TX_BC1 + fam : vkb_bc_is_packed(img->format) ? FAM_BC1_16 : fam);
    if (!pipe) return;
    upload u = {e, cb, vkb_emu_cb_bcn(e, cb, sizeof(bcn_cb)), src, img, layout, out_block_bytes(img),
                vkb_bc_block_bytes(img->client_format)};
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
    to_compute(&u);
    e->dev->real.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    for (uint32_t i = 0; i < n; i++) {
        const region *g = &regs[i];
        uint32_t slices = is3d ? g->iext.depth : g->sub.layerCount;
        uint32_t bh = (g->iext.height + 3) / 4;
        if (!g->iext.width || !g->iext.height || !slices) continue;
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
    flush(&u);
    free(u.bufs);
    free(u.copies);
    vkb_emu_restore_compute(e, cb);
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
    decode(e, cb, src, img, dl, n, g);
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
    decode(e, cb, info->srcBuffer, img, info->dstImageLayout, info->regionCount, g);
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
 * to RGBA8 UNORM or the other 16-bit format: the same texels, converted), -1 not at all (a BC and
 * an uncompressed image, a 16-bit and an sRGB image, which a blit would convert out of sRGB, or a
 * transcoded and a decoded image, or transcoded ones of different block sizes: re-encoding a
 * copy is not done). */
static int copy_kind(const vkb_emu_image *s, const vkb_emu_image *d)
{
    if (!s || !d) return -1;
    uint32_t st = vkb_tx_block_bytes(s->format), dt = vkb_tx_block_bytes(d->format);
    if (st || dt) {
        if (st == dt) return 0;
        VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy between BC images held as formats %d and %d is skipped (logged once)", s->format, d->format);
        return -2;
    }
    int sp = vkb_bc_is_packed(s->format), dp = vkb_bc_is_packed(d->format);
    if (!sp && !dp) return vkb_bc_storage_format(s->client_format) == vkb_bc_storage_format(d->client_format) ? 0 : -1;
    if (s->format == d->format) return 0;
    if ((sp || s->format == VK_FORMAT_R8G8B8A8_UNORM) && (dp || d->format == VK_FORMAT_R8G8B8A8_UNORM)) return 1;
    return -1;
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
    VKB_ONCE(VKB_LOG_WARN, "BC emulation: a copy between a BC image and another format is skipped (logged once)");
}

int vkb_emu_copy_image(vkb_emu_device *e, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                       const VkImageCopy *regions)
{
    vkb_emu_image *s = bc_image(e, src), *d = bc_image(e, dst);
    if (!s && !d) return 0;
    int kind = copy_kind(s, d);
    if (kind < 0) {
        if (kind == -1) skipped_copy();
        return 1;
    }
    VkImageCopy *c = (VkImageCopy *)regions;
    for (uint32_t i = 0; i < n; i++)
        clamp_region(s, d, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent);
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
    int kind = copy_kind(s, d);
    if (kind < 0) {
        if (kind == -1) skipped_copy();
        return 1;
    }
    VkImageCopy2 *c = (VkImageCopy2 *)info->pRegions;
    for (uint32_t i = 0; i < info->regionCount; i++)
        clamp_region(s, d, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent);
    if (!kind) {
        e->dev->real.vkCmdCopyImage2(cb, info);
        return 1;
    }
    VkImageBlit *b = malloc((info->regionCount ? info->regionCount : 1) * sizeof(*b));
    uint32_t k = 0;
    for (uint32_t i = 0; b && i < info->regionCount; i++)
        k += blit_region(s, &c[i].srcSubresource, &c[i].dstSubresource, &c[i].srcOffset, &c[i].dstOffset, &c[i].extent, &b[k]);
    if (k) e->dev->real.vkCmdBlitImage(cb, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout, k, b, VK_FILTER_NEAREST);
    free(b);
    return 1;
}

int vkb_emu_bcn_blocks_readback(vkb_emu_device *e, VkImage img)
{
    if (!bc_image(e, img)) return 0;
    VKB_ONCE(VKB_LOG_WARN, "BC emulation: reading a BC image back to a buffer is not supported (logged once)");
    return 1;
}

/* ------------------------------------------------------------------ install */

void vkb_emu_bcn_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    if (!(e->flags & VKB_EMU_BCN)) return;
    bcn_state *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
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
    const VkFormatFeatureFlags txneed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    char msg[256];
    int len = 0;
    msg[0] = 0;
    for (VkFormat f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; f++) {
        VkFormat t = tx_target(f);
        if (!t || !vkb_emu_bcn_transcode_wanted()) continue;
        int cls = tx_class(t);
        if (cls == 6 ? !hdr : cls == 3 ? !pf.textureCompressionASTC_LDR : !pf.textureCompressionETC2) continue;
        VkFormatProperties fp;
        dev->real.vkGetPhysicalDeviceFormatProperties(dev->physical, t, &fp);
        if ((fp.optimalTilingFeatures & txneed) != txneed) continue;
        s->tx[BC_INDEX(f)] = t;
        /* not needed for textures, but the client reports BC as blittable (client/emu.c) */
        int blit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
        if (len < (int)sizeof(msg) - 24) len += snprintf(msg + len, sizeof(msg) - (size_t)len, " %d->%d%s", f, t, blit ? "" : " (no blit)");
    }
    if (len) VKB_INFO("BC emulation: transcoding (BC format -> format):%s", msg);
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
        free(s);
        e->flags &= ~VKB_EMU_BCN;
        return;
    }
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &s->dsl, 1, &pr};
    dev->real.vkCreatePipelineLayout(dev->device, &plci, NULL, &s->layout);
    e->bcn = s;
    dev->dt.vkCreateImageView = emu_CreateImageView;
}

void vkb_emu_bcn_uninstall(vkb_srv_table *dev)
{
    bcn_state *s = dev->emu->bcn;
    if (!s) return;
    for (int i = 0; i < FAM_COUNT; i++)
        if (s->pipes[i]) dev->real.vkDestroyPipeline(dev->device, s->pipes[i], NULL);
    dev->real.vkDestroyPipelineLayout(dev->device, s->layout, NULL);
    dev->real.vkDestroyDescriptorSetLayout(dev->device, s->dsl, NULL);
    /* Scratch that command buffers still hold goes with their state (server_emu_cmdstate.c). */
    dev->emu->bcn = NULL;
    for (uint32_t i = 0; i < s->nidle; i++) {
        __atomic_sub_fetch(&g_idle, s->idle[i].size, __ATOMIC_RELAXED);
        chunk_destroy(dev->emu, &s->idle[i]);
    }
    free(s->idle);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
