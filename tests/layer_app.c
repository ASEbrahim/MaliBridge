/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A minimal Vulkan app: an instance, the physical devices, their identity. Nothing else, so that
 * the layer's own path is the only thing that can make it work (a plain app, no extension, no
 * application info).
 *
 *   tests/layer_app.c -> out/layer-app
 *
 * It is the case that caught the layer's exported vkGetInstanceProcAddr being bound to the
 * loader's own copy: the loader then took the layer for itself, and the app hung in the loader's
 * instance creation. With the layer in the path, the identity is the layer's (the GPU spoof, when
 * VKBRIDGE_SPOOF_GPU asks for one).
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance inst;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) {
        printf("no instance\n");
        return 1;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (!n) {
        printf("no device\n");
        return 1;
    }
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        printf("device %u: vendorID 0x%04x '%s'\n", i, p.vendorID, p.deviceName);
    }
    free(pds);
    vkDestroyInstance(inst, NULL);
    return 0;
}
