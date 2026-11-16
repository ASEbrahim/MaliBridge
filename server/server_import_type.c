/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * See server_import_type.h.
 */
#include "server_import_type.h"

#define KEEP (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
#define SPECIAL (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)

static int popcount(uint32_t v) { return __builtin_popcount(v); }

/* How near a candidate's flags are to the asked ones: higher is nearer. Host-visible and coherent
 * lost weigh more than anything else: a type that keeps them always beats one that does not. */
static int score(VkMemoryPropertyFlags asked, VkMemoryPropertyFlags cand)
{
    int s = -64 * popcount((asked & KEEP) & ~cand);
    if ((asked ^ cand) & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) s -= 16;
    s += 4 * popcount(asked & cand);
    s -= popcount(cand & ~asked);
    return s;
}

int vkb_import_type_pick(const VkPhysicalDeviceMemoryProperties *mp, uint32_t asked, uint32_t fd_bits, uint32_t req_bits, const char **why)
{
    if (why) *why = "";
    if (!mp || asked >= 32 || asked >= mp->memoryTypeCount) return -1;
    if (!fd_bits || (fd_bits & (1u << asked))) return -1;
    VkMemoryPropertyFlags want = mp->memoryTypes[asked].propertyFlags;
    uint32_t pool = fd_bits & req_bits;
    int best = -1, best_score = 0;
    for (uint32_t i = 0; i < mp->memoryTypeCount && i < 32; i++) {
        if (!(pool >> i & 1)) continue;
        VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        if ((f & SPECIAL) & ~want) continue; /* lazily allocated / protected only when the asked type is one */
        int s = score(want, f);
        if (best < 0 || s > best_score) best = (int)i, best_score = s;
    }
    if (why)
        *why = best < 0 ? (pool ? "only lazily allocated or protected types are listed" : "no type is in both the dma-buf's and the resource's masks")
                        : ((want & KEEP) & ~mp->memoryTypes[best].propertyFlags) ? "closest flags; none listed keeps the asked host access"
                                                                                  : "closest flags";
    return best;
}
