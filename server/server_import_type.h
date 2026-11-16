/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The memory type a dma-buf import goes in when the type asked for is not one the dma-buf takes
 * (vkGetMemoryFdPropertiesKHR's memoryTypeBits leave it out). The importer chose its type from the
 * resource's requirements alone (gamescope, Zink and Xwayland all do, or pick by flags): on
 * PowerVR and on Mali the driver's answer for a dma-buf is a different set, and an import in a
 * type outside it fails (VK_ERROR_INVALID_EXTERNAL_HANDLE). A pure function of the memory table
 * and the masks, so a host test covers it without a driver.
 *
 * vkb_import_type_pick: the type to use instead of `asked`, or -1 when `asked` already is one of
 * fd_bits (nothing to change) or no type is better than failing. Candidates are types in fd_bits and
 * in req_bits (the resource's own mask; UINT32_MAX when the import has no dedicated resource to ask)
 * that are ordinary (not lazily allocated, not protected; those only when the asked type is one
 * itself). The closest to the asked flags wins: a type that keeps the asked HOST_VISIBLE and
 * HOST_COHERENT beats any that lost them (the lost ones are still taken when nothing listed keeps
 * them: the server only knows the type it was handed, which for a client's device-local type may be
 * a host-visible stand-in, and an import that fails is no better), then the same DEVICE_LOCAL-ness,
 * the most flags in common, the fewest extra; the lowest index on a tie. *why (may be NULL) says what decided, for the log.
 */
#ifndef VKB_SERVER_IMPORT_TYPE_H
#define VKB_SERVER_IMPORT_TYPE_H

#include <stdint.h>
#include <vulkan/vulkan.h>

int vkb_import_type_pick(const VkPhysicalDeviceMemoryProperties *mp, uint32_t asked, uint32_t fd_bits, uint32_t req_bits, const char **why);

#endif
