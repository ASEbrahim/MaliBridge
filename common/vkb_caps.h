/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VKB_CAPS_H
#define VKB_CAPS_H

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "vkb_proto.h"
#include "vkb_wire.h"

typedef void (*vkb_caps_get_fmt_fn)(VkPhysicalDevice pd, VkFormat format,
                                    VkFormatProperties *out, void *user);
typedef void (*vkb_caps_get_props2_fn)(VkPhysicalDevice pd,
                                       VkPhysicalDeviceProperties2 *out, void *user);
typedef int (*vkb_caps_ext_faked_fn)(const char *name, void *user);

typedef struct vkb_pd_caps {
    uint32_t emu;         /* VKB_EMU_* active bitmask */
    uint32_t missing;     /* VKB_EMU_* missing on hardware bitmask */
    uint32_t aliased;     /* bit i: vkb_ext_aliases[i] is offered */
    uint32_t spoof_gpu;   /* 0, VKB_SPOOF_NVIDIA, or VKB_SPOOF_AMD */
    uint32_t heap_max_mb; /* 0 = uncapped, >0 = cap device-local heaps in MB */
    VkPhysicalDeviceProperties real_props;
} vkb_pd_caps;

/* The GPU identity the app is shown: VKB_SPOOF_NVIDIA, VKB_SPOOF_AMD or 0 (the real one). The
 * value is VKBRIDGE_SPOOF_GPU's: "nvidia", "amd", "off" or "0". *ok is set to 0 when the value is
 * none of those. */
uint32_t vkb_caps_spoof_gpu(const char *value, int *ok);

/* The device-local heap cap in MB (0: uncapped, and also when the value is not a number). The
 * value is VKBRIDGE_HEAP_MAX_MB's. *ok is set to 0 when the value is not a number of MB. */
uint32_t vkb_caps_heap_max_mb(const char *value, int *ok);

/* Populates emulated extensions (up to 16) based on caps->emu */
const VkExtensionProperties *vkb_caps_emu_extensions(const vkb_pd_caps *caps, uint32_t *n);

/* Computes the aliased extension bitmask from the driver's raw VkExtensionProperties */
uint32_t vkb_caps_compute_aliased_mask(const VkExtensionProperties *avail, uint32_t avail_count);

/* Appends emulated and aliased extensions to an existing extension array, deduplicating entries */
uint32_t vkb_caps_append_device_extensions(
    const vkb_pd_caps *caps,
    VkExtensionProperties *exts, uint32_t count, uint32_t max_cap);

/* The extension names to pass to the driver:
 * 1. the ones the driver has,
 * 2. the app's EXT names translated to the driver's KHR names if necessary,
 * 3. the emulated and client-side ones dropped,
 * 4. and push descriptors if BC is enabled.
 * @param not_passed - dropped extensions
 * @out must have room for requested_count + 2. */
uint32_t vkb_caps_filter_device_extensions(
    uint32_t emu,
    const VkExtensionProperties *avail, uint32_t avail_count,
    const char *const *requested, uint32_t requested_count,
    const char **out, uint32_t max_out,
    vkb_caps_ext_faked_fn faked, void *user,
    char *not_passed, size_t not_passed_size);

/* Filters VkDeviceCreateInfo::ppEnabledExtensionNames into arena:
 * - retains extensions natively supported by the driver
 * - translates app EXT extension names to driver KHR names via vkb_ext_aliases
 * - drops purely emulated extensions so the driver does not reject vkCreateDevice
 * - adds VK_KHR_push_descriptor when VKB_EMU_BCN is active and supported by the driver */
void vkb_caps_filter_device_create_extensions(
    uint32_t emu,
    const VkExtensionProperties *avail, uint32_t avail_count,
    VkDeviceCreateInfo *ci, vkb_arena *arena,
    vkb_caps_ext_faked_fn faked, void *user);

/* Patches VkPhysicalDeviceProperties (API version cap, GPU spoof, and emulated limits) */
void vkb_caps_patch_properties(const vkb_pd_caps *caps, VkPhysicalDeviceProperties *p,
                               uint32_t max_api);

/* Patches VkPhysicalDeviceProperties2::pNext */
void vkb_caps_patch_properties_chain(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                                     VkBaseOutStructure *b,
                                     vkb_caps_get_props2_fn get_props2, void *user);

/* Patches VkPhysicalDeviceFeatures */
void vkb_caps_patch_features(const vkb_pd_caps *caps, VkPhysicalDeviceFeatures *f);

/* Patches VkPhysicalDeviceFeatures2::pNext */
void vkb_caps_patch_features_chain(const vkb_pd_caps *caps, VkBaseOutStructure *b,
                                   int is_remote_bridge, bool no_sharing);

/* Returns 1 if format is emulated and fills fp / fp2; returns 0 if native */
int vkb_caps_format_properties(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                               VkFormat format, VkFormatProperties *fp,
                               VkFormatProperties2 *fp2,
                               vkb_caps_get_fmt_fn get_fmt, void *user);

/* Takes VK_IMAGE_CREATE_EXTENDED_USAGE_BIT out of *flags for a multi-planar format whose own
 * features cover usage at tiling (the Mali-G925 refuses the flag on such formats); 1 when it did. */
int vkb_caps_planar_ext_usage(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                              VkFormat format, VkImageTiling tiling,
                              VkImageUsageFlags usage, VkImageCreateFlags *flags,
                              vkb_caps_get_fmt_fn get_fmt, void *user);

/* Applies VKBRIDGE_HEAP_MAX_MB cap to device-local heaps and VkPhysicalDeviceMemoryBudgetPropertiesEXT */
void vkb_caps_patch_memory_heaps(const vkb_pd_caps *caps,
                                 VkPhysicalDeviceMemoryProperties *mem,
                                 VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget);

#ifdef __cplusplus
}
#endif

#endif /* VKB_CAPS_H */
