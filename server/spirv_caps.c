/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What a SPIR-V module asks of the device (spirv_caps.h). A scan of the module's preamble, done once
 * per module when it is made (server_guard.c), never per draw.
 */
#include "spirv_caps.h"

#include <stdio.h>
#include <string.h>

static const struct {
    const char *name, *feature;
} cap_info[VKB_SPVCAP_COUNT] = {
    {"ClipDistance", "shaderClipDistance"},
    {"CullDistance", "shaderCullDistance"},
    {"Float64", "shaderFloat64"},
    {"Int64", "shaderInt64"},
    {"Int16", "shaderInt16"},
    {"Geometry", "geometryShader"},
    {"Tessellation", "tessellationShader"},
    {"ImageCubeArray", "imageCubeArray"},
    {"SampleRateShading", "sampleRateShading"},
    {"ImageGatherExtended", "shaderImageGatherExtended"},
    {"StorageImageMultisample", "shaderStorageImageMultisample"},
    {"StorageImageExtendedFormats", "shaderStorageImageExtendedFormats"},
    {"StorageImageReadWithoutFormat", "shaderStorageImageReadWithoutFormat"},
    {"StorageImageWriteWithoutFormat", "shaderStorageImageWriteWithoutFormat"},
};

const char *vkb_spv_cap_name(int i) { return i >= 0 && i < VKB_SPVCAP_COUNT ? cap_info[i].name : "?"; }
const char *vkb_spv_cap_feature(int i) { return i >= 0 && i < VKB_SPVCAP_COUNT ? cap_info[i].feature : "?"; }

void vkb_spv_caps_text(uint32_t mask, char *out, size_t cap)
{
    size_t n = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < VKB_SPVCAP_COUNT && n < cap; i++)
        if (mask & (1u << i)) {
            int k = snprintf(out + n, cap - n, "%s%s (%s)", n ? ", " : "", cap_info[i].name, cap_info[i].feature);
            if (k < 0) break;
            n += (size_t)k;
        }
}

/* SPIR-V capability numbers (the unified specification's Capability enum) */
static uint32_t cap_bit(uint32_t c)
{
    switch (c) {
    case 2: return VKB_SPVCAP_GEOM;
    case 3: return VKB_SPVCAP_TESS;
    case 10: return VKB_SPVCAP_F64;
    case 11: return VKB_SPVCAP_I64;
    case 22: return VKB_SPVCAP_I16;
    case 25: return VKB_SPVCAP_GATHER_EXT;
    case 27: return VKB_SPVCAP_MS_STORAGE;
    case 32: return VKB_SPVCAP_CLIP;
    case 33: return VKB_SPVCAP_CULL;
    case 34: case 45: return VKB_SPVCAP_CUBE_ARRAY;
    case 35: case 52: return VKB_SPVCAP_SAMPLE_RATE;
    case 49: return VKB_SPVCAP_EXT_FORMATS;
    case 55: return VKB_SPVCAP_READ_NOFMT;
    case 56: return VKB_SPVCAP_WRITE_NOFMT;
    default: return 0;
    }
}

uint32_t vkb_spv_caps(const uint32_t *code, size_t bytes)
{
    if (!code || bytes < 20 || (bytes & 3) || code[0] != 0x07230203u) return 0;
    size_t n = bytes / 4, i = 5;
    uint32_t need = 0;
    while (i < n) {
        uint32_t op = code[i] & 0xffffu, wc = code[i] >> 16;
        if (!wc || i + wc > n) break;
        if (op == 17 && wc >= 2) {
            need |= cap_bit(code[i + 1]); /* OpCapability */
        } else if (op == 71 && wc >= 4 && code[i + 2] == 11) {
            /* OpDecorate %x BuiltIn ClipDistance (3) / CullDistance (4) */
            if (code[i + 3] == 3) need |= VKB_SPVCAP_CLIP;
            if (code[i + 3] == 4) need |= VKB_SPVCAP_CULL;
        } else if (op == 72 && wc >= 5 && code[i + 3] == 11) {
            /* OpMemberDecorate %block m BuiltIn ClipDistance / CullDistance (gl_PerVertex) */
            if (code[i + 4] == 3) need |= VKB_SPVCAP_CLIP;
            if (code[i + 4] == 4) need |= VKB_SPVCAP_CULL;
        } else if ((op >= 19 && op <= 39) || op == 54) {
            break; /* the first type (or function): the annotations are behind */
        }
        i += wc;
    }
    return need;
}
