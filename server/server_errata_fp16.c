/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fp16_hide (MK-20): the Pixel 8's Mali-G715 with drivers r54p1 and r54p3 gets 16-bit float
 * arithmetic wrong (ncnn#7054: right with r47p0, and in fp32). DXVK compiles its shaders with
 * 16-bit arithmetic whenever shaderFloat16 is offered; vkd3d-proton's native 16-bit types want it
 * too.
 *
 * The fix is what the clients are shown: with the erratum on for a physical device (its decision
 * in server_errata.c: the fp16_math probe, the driver range in safe mode, VKBRIDGE_NO_FP16), the
 * feature queries answer shaderFloat16 off (VkPhysicalDeviceVulkan12Features,
 * VkPhysicalDeviceShaderFloat16Int8Features) and the property queries the 16-bit float controls
 * (denormals, rounding, signed zero/inf/nan) off, so DXVK and vkd3d-proton take their 32-bit
 * paths. An app that enables shaderFloat16 anyway has it taken off its vkCreateDevice (logged).
 *
 * The 16-bit storage features (storageBuffer16BitAccess, uniformAndStorageBuffer16BitAccess,
 * storagePushConstant16, storageInputOutput16) stay as the driver reports them: DXVK 3 requires
 * storageBuffer16BitAccess (dxvk_device_info.cpp) and rejects a GPU without it, so hiding it would
 * stop every D3D game. They are no way into the fp16 maths either: without the Float16 capability
 * (shaderFloat16) SPIR-V may only load, store and convert 16-bit floats (the StorageBuffer16BitAccess
 * family of capabilities); ncnn#7054's wrong results are in the arithmetic, and fp16_math checks
 * the conversions on their own (its convert sub-case). shaderInt16 and shaderInt8 stay. The
 * extensions stay offered: VK_KHR_shader_float16_int8 also carries shaderInt8, and an extension
 * whose features are all off is valid.
 *
 * The decision is made from the same inputs at each query and at vkCreateDevice; a probe that
 * finishes in the background between them (it is a light one, at start in practice) changes it
 * from the next start.
 *
 * Imitation (host, VKBRIDGE_ERRATA_IMITATE=fp16_hide): beneath the fix, shader modules get their
 * 16-bit float multiplies turned into adds and their 16-bit fmas into mixes - a driver that
 * computes 16-bit floats wrong. The fp16_math probe then fails (server_probe.c).
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ what the clients are shown */

#define OFF(field, name)                                                                                 \
    do {                                                                                                 \
        if (s->field) {                                                                                  \
            s->field = VK_FALSE;                                                                         \
            n++;                                                                                         \
            if (what) {                                                                                  \
                size_t l = strlen(what);                                                                 \
                if (!strstr(what, name)) snprintf(what + l, cap - l, "%s%s", l ? ", " : "", name);       \
            }                                                                                            \
        }                                                                                                \
    } while (0)

/* Turns shaderFloat16 off in a features chain (the 16-bit storage features stay: see the top);
 * returns how many were on (what, when not NULL, gets their names) */
static int features_off(void *chain, char *what, size_t cap)
{
    int n = 0;
    for (VkBaseOutStructure *b = chain; b; b = b->pNext) switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            VkPhysicalDeviceVulkan12Features *s = (void *)b;
            OFF(shaderFloat16, "shaderFloat16");
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
            VkPhysicalDeviceShaderFloat16Int8Features *s = (void *)b;
            OFF(shaderFloat16, "shaderFloat16");
            break;
        }
        }
    return n;
}

static void properties_off(void *chain)
{
    for (VkBaseOutStructure *b = chain; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            VkPhysicalDeviceVulkan12Properties *p = (void *)b;
            p->shaderSignedZeroInfNanPreserveFloat16 = p->shaderDenormPreserveFloat16 = p->shaderDenormFlushToZeroFloat16 = VK_FALSE;
            p->shaderRoundingModeRTEFloat16 = p->shaderRoundingModeRTZFloat16 = VK_FALSE;
        } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES) {
            VkPhysicalDeviceFloatControlsProperties *p = (void *)b;
            p->shaderSignedZeroInfNanPreserveFloat16 = p->shaderDenormPreserveFloat16 = p->shaderDenormFlushToZeroFloat16 = VK_FALSE;
            p->shaderRoundingModeRTEFloat16 = p->shaderRoundingModeRTZFloat16 = VK_FALSE;
        }
    }
}

static int hidden(vkb_srv_table *t, VkPhysicalDevice pd)
{
    char why[320];
    if (!vkb_errata_fp16_hidden(t, pd, why, sizeof(why))) return 0;
    vkb_errata_info_once("errata: fp16_hide: shaderFloat16 and the 16-bit float controls hidden from clients, 16-bit storage kept (%s) - MK-20", why);
    return 1;
}

static VKAPI_ATTR void VKAPI_CALL fp16_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (!t) return;
    t->real.vkGetPhysicalDeviceFeatures2(pd, f);
    if (hidden(t, pd)) features_off(f->pNext, NULL, 0);
}

static VKAPI_ATTR void VKAPI_CALL fp16_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (!t) return;
    t->real.vkGetPhysicalDeviceProperties2(pd, p);
    if (p->pNext && hidden(t, pd)) properties_off(p->pNext);
}

void vkb_errata_fp16_instance_install(vkb_srv_table *t)
{
    if (t->real.vkGetPhysicalDeviceFeatures2 && t->dt.vkGetPhysicalDeviceFeatures2 == t->real.vkGetPhysicalDeviceFeatures2)
        t->dt.vkGetPhysicalDeviceFeatures2 = fp16_GetPhysicalDeviceFeatures2;
    if (t->real.vkGetPhysicalDeviceProperties2 && t->dt.vkGetPhysicalDeviceProperties2 == t->real.vkGetPhysicalDeviceProperties2)
        t->dt.vkGetPhysicalDeviceProperties2 = fp16_GetPhysicalDeviceProperties2;
}

void vkb_errata_fp16_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci)
{
    char why[320];
    if (!vkb_errata_fp16_hidden(inst, pd, why, sizeof(why))) return;
    char what[200] = "";
    /* the chain lives in the request's arena: ours to change */
    if (features_off((void *)ci->pNext, what, sizeof(what)))
        VKB_INFO("errata: fp16_hide: the app enabled %s anyway; taken off its device (%s) - MK-20", what, why);
}

void vkb_errata_fp16_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)t, (void)e;
    vkb_errata_info_once("errata: fp16_hide: shaderFloat16 and the 16-bit float controls not offered to this device's clients and "
                         "shaderFloat16 taken off its creation (16-bit storage kept); DXVK and vkd3d-proton compute in 32 bits");
}

/* ------------------------------------------------------------------ the imitation (host tests) */

enum {
    OP_DECORATE = 71, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12,
    OP_FADD = 129, OP_FMUL = 133, GLSL_FMIX = 46, GLSL_FMA = 50
};

/* 16-bit float multiplies become adds, 16-bit fmas mixes; returns how many were changed */
static int miscompile(uint32_t *w, size_t n)
{
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    uint8_t *half = calloc(bound ? bound : 1, 1);
    if (!half) return 0;
    uint32_t glsl = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        if (op == OP_TYPE_FLOAT && wc >= 3 && w[i + 2] == 16 && w[i + 1] < bound) half[w[i + 1]] = 1;
        if (op == OP_TYPE_VECTOR && wc >= 4 && w[i + 1] < bound && w[i + 2] < bound && half[w[i + 2]]) half[w[i + 1]] = 1;
        if (op == OP_EXT_INST_IMPORT && wc >= 3 && !strncmp((const char *)&w[i + 2], "GLSL.std.450", (wc - 2) * 4)) glsl = w[i + 1];
        i += wc;
    }
    int changed = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        int is_half = wc >= 3 && w[i + 1] < bound && half[w[i + 1]];
        if (op == OP_FMUL && wc == 5 && is_half) w[i] = (wc << 16) | OP_FADD, changed++;
        if (op == OP_EXT_INST && wc == 8 && is_half && glsl && w[i + 3] == glsl && w[i + 4] == GLSL_FMA) w[i + 4] = GLSL_FMIX, changed++;
        i += wc;
    }
    free(half);
    return changed;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    PFN_vkCreateShaderModule next = e && e->fp16_imi_next ? e->fp16_imi_next : vkb_errata_next(1)->vkCreateShaderModule;
    size_t n = ci->codeSize / 4;
    uint32_t *code = n ? malloc(n * 4) : NULL;
    if (!code) return next(d, ci, a, out);
    memcpy(code, ci->pCode, n * 4);
    VkResult r;
    if (miscompile(code, n)) {
        VkShaderModuleCreateInfo c2 = *ci;
        c2.pCode = code;
        r = next(d, &c2, a, out);
    } else
        r = next(d, ci, a, out);
    free(code);
    return r;
}

void vkb_errata_fp16_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    e->fp16_imi_next = t->real.vkCreateShaderModule;
    ERRATA_HOOK(t, vkCreateShaderModule, imi_CreateShaderModule);
}
