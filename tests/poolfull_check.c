/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * poolfull_check: a descriptor pool used the way DXVK uses one (sets until the pool says
 * VK_ERROR_OUT_OF_POOL_MEMORY, then a reset, three rounds), and sets no pool like it can hold.
 * The pool: 256 sets, 256 uniform buffers. The client's books grow it up to 8 pools' worth
 * (client/descriptor.c ds_allocate), so each round gives exactly 2048 sets; that end is the
 * normal "pool full" and is not warned about (host/poolfull_test.sh reads the client's log).
 * Refused, with a warning, whatever the pool's growth so far:
 *   - a set of 1024 uniform buffers (more than the pool holds), on a fresh pool and on one grown twice;
 *   - a set of 300 (more than one pool, less than the grown books: the server's overflow pools
 *     are each the original size, so no one pool could hold it).
 * And a pool shaped like DXVK's on the XRing O3 (MaliBridge #16: maxSets 1024, 512 sampled images,
 * 2048 uniform buffers and others), with a set that takes 4 sampled images and 4 uniform buffers
 * and one that takes a uniform buffer only, in turns. The sampled images run out first: after
 * 8 pools' worth of them (1024 of the big sets) the pool says full while thousands of sets remain,
 * as a driver says full when any one type runs out. That is the normal "pool full", not warned
 * about; the small sets still come from the same pool, a second pool like it gives big sets at
 * once (DXVK moving on), and after a reset the first gives 1024 again.
 *
 * `poolfull_check lowmem` (host/lowmem_test.sh, a server told memory is low): the pool filled once
 * (8 pools' worth, 7 overflow pools on the server), then for 12 s a few sets and a reset every
 * 50 ms - the resets after the server notices the low memory free the idle overflow pools - then
 * filled again: still 2048 sets (the overflow pools made again).
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures, checks;

static void check(int ok, const char *what, int v)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%d)\n", ok ? "PASS" : "FAIL", what, v);
}

static int refused(VkResult r) { return r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL; }

int main(int argc, char **argv)
{
    int lowmem = argc > 1 && !strcmp(argv[1], "lowmem");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-poolfull-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define G(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    G(vkCreateDescriptorSetLayout); G(vkDestroyDescriptorSetLayout); G(vkCreateDescriptorPool); G(vkDestroyDescriptorPool);
    G(vkAllocateDescriptorSets); G(vkResetDescriptorPool); G(vkDestroyDevice);
#undef G
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    VkDescriptorSetLayout one = VK_NULL_HANDLE, big = VK_NULL_HANDLE, mid = VK_NULL_HANDLE;
    VkResult r1 = vkCreateDescriptorSetLayout(dev, &lci, NULL, &one);
    b.descriptorCount = 1024;
    VkResult r2 = vkCreateDescriptorSetLayout(dev, &lci, NULL, &big);
    b.descriptorCount = 300;
    VkResult r3 = vkCreateDescriptorSetLayout(dev, &lci, NULL, &mid);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 256, 1, &ps};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult r4 = vkCreateDescriptorPool(dev, &pci, NULL, &pool);
    if (r1 || r2 || r3 || r4) {
        printf("FAILED: setup: layouts %d %d %d, pool %d\n", r1, r2, r3, r4);
        return 1;
    }

    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &big};
    VkResult r;
    if (lowmem) {
        ai.pSetLayouts = &one;
        for (int round = 0; round < 2; round++) {
            int got = 0;
            while (got < 8192 && vkAllocateDescriptorSets(dev, &ai, &ds) == VK_SUCCESS) got++;
            check(got == 8 * 256, round ? "filled again after the low-memory resets: 8 pools' worth" : "filled: 8 pools' worth", got);
            vkResetDescriptorPool(dev, pool, 0);
            if (round) break;
            struct timespec t0, t, nap = {0, 50 * 1000 * 1000};
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int resets = 0, short_rounds = 0;
            do {
                int k = 0;
                while (k < 10 && vkAllocateDescriptorSets(dev, &ai, &ds) == VK_SUCCESS) k++;
                short_rounds += k == 10;
                vkResetDescriptorPool(dev, pool, 0);
                resets++;
                nanosleep(&nap, NULL);
                clock_gettime(CLOCK_MONOTONIC, &t);
            } while (t.tv_sec - t0.tv_sec < 12);
            check(short_rounds == resets, "10 sets between resets for 12 s", resets);
        }
        goto done;
    }
    r = vkAllocateDescriptorSets(dev, &ai, &ds);
    check(refused(r), "a set bigger than the pool, fresh pool: refused", r);

    for (int round = 0; round < 3; round++) {
        int got = 0;
        ai.pSetLayouts = &one;
        while (got < 8192 && (r = vkAllocateDescriptorSets(dev, &ai, &ds)) == VK_SUCCESS) got++;
        check(refused(r), "the pool fills and says so", r);
        check(got == 8 * 256, "8 pools' worth of sets a round", got);
        vkResetDescriptorPool(dev, pool, 0);
    }

    /* Grown twice (past 512 sets), then the sets no single pool holds. */
    ai.pSetLayouts = &one;
    int got = 0;
    while (got < 600 && vkAllocateDescriptorSets(dev, &ai, &ds) == VK_SUCCESS) got++;
    check(got == 600, "600 sets (the pool grown twice)", got);
    ai.pSetLayouts = &big;
    r = vkAllocateDescriptorSets(dev, &ai, &ds);
    check(refused(r), "a set bigger than the pool, grown pool: refused", r);
    ai.pSetLayouts = &mid;
    r = vkAllocateDescriptorSets(dev, &ai, &ds);
    check(refused(r), "a set bigger than one pool but within the grown books: refused", r);
    ai.pSetLayouts = &one;
    r = vkAllocateDescriptorSets(dev, &ai, &ds);
    check(r == VK_SUCCESS, "an ordinary set after those", r);

    /* MaliBridge #16: one type runs out while the pool has sets left. */
    VkDescriptorSetLayoutBinding hb[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo hci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, hb};
    VkDescriptorSetLayout heavy = VK_NULL_HANDLE;
    VkResult r5 = vkCreateDescriptorSetLayout(dev, &hci, NULL, &heavy);
    VkDescriptorPoolSize dx[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
                                 {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256},
                                 {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048},      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
                                 {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 16}};
    VkDescriptorPoolCreateInfo dci2 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1024, sizeof(dx) / sizeof(dx[0]), dx};
    VkDescriptorPool dxp = VK_NULL_HANDLE, dxp2 = VK_NULL_HANDLE;
    VkResult r6 = vkCreateDescriptorPool(dev, &dci2, NULL, &dxp);
    VkResult r7 = vkCreateDescriptorPool(dev, &dci2, NULL, &dxp2);
    check(!r5 && !r6 && !r7, "#16: the layout and two pools shaped like DXVK's", r5 ? r5 : r6 ? r6 : r7);
    if (!r5 && !r6 && !r7) {
        VkDescriptorSetAllocateInfo hai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dxp, 1, &heavy};
        VkDescriptorSetAllocateInfo uai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dxp, 1, &one};
        for (int round = 0; round < 2; round++) {
            int big_sets = 0, small_sets = 0;
            while (big_sets < 4096 && (r = vkAllocateDescriptorSets(dev, &hai, &ds)) == VK_SUCCESS) {
                big_sets++;
                if (vkAllocateDescriptorSets(dev, &uai, &ds) == VK_SUCCESS) small_sets++;
            }
            check(refused(r), "#16: the pool says full when its sampled images run out", r);
            check(big_sets == 1024, "#16: 8 pools' worth of sampled images, 1024 big sets", big_sets);
            check(small_sets == big_sets, "#16: every small set alongside them", small_sets);
            int more = 0;
            while (more < 100 && vkAllocateDescriptorSets(dev, &uai, &ds) == VK_SUCCESS) more++;
            check(more == 100, "#16: small sets still come from the full pool (other types left)", more);
            if (!round) {
                hai.descriptorPool = dxp2;
                r = vkAllocateDescriptorSets(dev, &hai, &ds);
                check(r == VK_SUCCESS, "#16: a second pool like it gives a big set at once", r);
                hai.descriptorPool = dxp;
            }
            vkResetDescriptorPool(dev, dxp, 0);
        }
    }
    if (dxp) vkDestroyDescriptorPool(dev, dxp, NULL);
    if (dxp2) vkDestroyDescriptorPool(dev, dxp2, NULL);
    if (heavy) vkDestroyDescriptorSetLayout(dev, heavy, NULL);

done:
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkDestroyDescriptorSetLayout(dev, one, NULL);
    vkDestroyDescriptorSetLayout(dev, big, NULL);
    vkDestroyDescriptorSetLayout(dev, mid, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
