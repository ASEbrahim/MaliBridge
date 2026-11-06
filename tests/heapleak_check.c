/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * heapleak_check: what Detroit: Become Human (native Vulkan 1.1 on a G925) did to the
 * server's heap, for host/heapleak_test.sh (which reads the server's log and /proc).
 *   pools   FREE_DESCRIPTOR_SET pools of 45056 sets, still empty, asked for a set bigger than
 *           the pool (more image samplers than it has) 20 times each, six pools: refused every
 *           time (as before), and the server must not make overflow pools for it (it made 64 per
 *           pool, 384 in all, 8 MB each in Mali's heap). Then the overflow pools that are wanted:
 *           a FREE pool of 32 small sets gives 4 pools' worth, sets freed and allocated again.
 *   frames  N frames (default 10000) of a game that compiles into its own pipeline cache while
 *           it plays: each 4 ms frame (HEAPLEAK_FRAME_US) a few descriptor sets from a FREE pool
 *           (freed again), the oversized set asked for once more (every 100th frame), an empty
 *           submission waited for, and a compute
 *           pipeline (a new specialization of one module) compiled into the app's cache and
 *           destroyed (anything the server keeps past a pipeline's destroy shows); every 2000
 *           frames the app's
 *           cache goes and a new one is made. The server's per-program cache is set over its
 *           limit by the test, so it is never saved again: copies of the app's caches merged
 *           into it are dead weight. Prints the server's RssAnon (HEAPLEAK_SERVER_PID) at
 *           frame 2000 (after a pause for the server's saver) and at the end:
 *             "server anon at 2000: N KB", "server anon at end: N KB".
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smoke_spv.h"

static int failures, checks;

static void check(int ok, const char *what, long v)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
}

static int refused(VkResult r) { return r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL; }

/* The server's anonymous memory in KB (0: not known); HEAPLEAK_SERVER_PID=self: this process's
 * (the same run on the desktop driver without the bridge, for what the driver itself keeps). */
static long server_anon_kb(void)
{
    const char *pid = getenv("HEAPLEAK_SERVER_PID");
    if (!pid || !*pid) return 0;
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long kb = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "RssAnon:", 8)) kb = atol(line + 8);
    fclose(f);
    return kb;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "pools";
    int frames = argc > 2 ? atoi(argv[2]) : 10000;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    /* 1.3 for nullcache: the server counts cache hits with the driver's creation feedback (core in 1.3) */
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-heapleak-check", 1, "vkbridge", 1,
                             !strcmp(mode, "nullcache") ? VK_API_VERSION_1_3 : !strcmp(mode, "varcount") ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1};
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
    /* varcount: a bindless-style layout (a variable descriptor count) */
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.descriptorIndexing = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    if (!strcmp(mode, "varcount")) dci.pNext = &f12;
    VkDevice dev;
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define G(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    G(vkCreateDescriptorSetLayout); G(vkDestroyDescriptorSetLayout); G(vkCreateDescriptorPool); G(vkDestroyDescriptorPool);
    G(vkAllocateDescriptorSets); G(vkFreeDescriptorSets); G(vkResetDescriptorPool); G(vkDestroyDevice);
    G(vkCreatePipelineLayout); G(vkDestroyPipelineLayout); G(vkCreateShaderModule); G(vkDestroyShaderModule);
    G(vkCreateComputePipelines); G(vkDestroyPipeline); G(vkCreatePipelineCache); G(vkDestroyPipelineCache);
    G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkResetFences);
#undef G
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

    /* Detroit's layout: 8426 combined image samplers, 12800 storage buffers and a few dynamic
     * buffers in one set; its pools hold fewer image samplers than one set of it takes. */
    VkDescriptorSetLayoutBinding big_b[4] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8426, VK_SHADER_STAGE_ALL, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12800, VK_SHADER_STAGE_ALL, NULL},
                                             {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 5, VK_SHADER_STAGE_ALL, NULL},
                                             {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, big_b};
    VkDescriptorSetLayoutBinding small_b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &small_b};
    VkDescriptorSetLayout big = VK_NULL_HANDLE, small = VK_NULL_HANDLE;
    VkResult r1 = vkCreateDescriptorSetLayout(dev, &lci, NULL, &big), r2 = vkCreateDescriptorSetLayout(dev, &sci, NULL, &small);
    VkDescriptorPoolSize dps[4] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 64}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                       45056, 4, dps};
    if (r1 || r2) {
        printf("FAILED: setup: layouts %d %d\n", r1, r2);
        return 1;
    }
    VkDescriptorSet ds[8];
    VkResult r;

    if (!strcmp(mode, "pools")) {
        VkDescriptorPool pools[6];
        int refusals = 0, setup = 0;
        for (int i = 0; i < 6; i++) {
            if (vkCreateDescriptorPool(dev, &dpci, NULL, &pools[i])) {
                pools[i] = VK_NULL_HANDLE;
                continue;
            }
            setup++;
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pools[i], 1, &big};
            for (int k = 0; k < 20; k++) refusals += refused(vkAllocateDescriptorSets(dev, &ai, ds));
        }
        check(setup == 6, "six Detroit-shaped pools (FREE_DESCRIPTOR_SET, 45056 sets)", setup);
        check(refusals == 120, "the set bigger than the pool refused each time, on the empty pools", refusals);
        /* The pools still give the sets that fit. */
        int small_ok = 0;
        for (int i = 0; i < 6; i++) {
            if (!pools[i]) continue;
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pools[i], 1, &small};
            small_ok += vkAllocateDescriptorSets(dev, &ai, ds) == VK_SUCCESS;
        }
        check(small_ok == 6, "an ordinary set from each of them after that", small_ok);
        for (int i = 0; i < 6; i++)
            if (pools[i]) vkDestroyDescriptorPool(dev, pools[i], NULL);

        /* Overflow pools that are wanted: a FREE pool of 32 sets of 2 storage buffers, filled to
         * 4 pools' worth (the server's overflow pools), half freed, filled again, then a set too big. */
        VkDescriptorPoolSize fps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
        VkDescriptorPoolCreateInfo fpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                           32, 1, &fps};
        VkDescriptorPool fp;
        if (vkCreateDescriptorPool(dev, &fpci, NULL, &fp)) {
            printf("FAILED: the small FREE pool\n");
            return 1;
        }
        static VkDescriptorSet held[128];
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, fp, 1, &small};
        int got = 0;
        while (got < 128 && vkAllocateDescriptorSets(dev, &ai, &held[got]) == VK_SUCCESS) got++;
        check(got == 128, "a FREE pool of 32 sets gives 4 pools' worth (overflow pools as before)", got);
        r = vkFreeDescriptorSets(dev, fp, 64, held);
        int again = 0;
        while (again < 64 && vkAllocateDescriptorSets(dev, &ai, &held[again]) == VK_SUCCESS) again++;
        check(r == VK_SUCCESS && again == 64, "64 freed and 64 allocated again", again);
        ai.pSetLayouts = &big;
        r = vkAllocateDescriptorSets(dev, &ai, ds);
        check(refused(r), "a set too big for it refused, with every overflow pool full", r);
        vkResetDescriptorPool(dev, fp, 0);
        r = vkAllocateDescriptorSets(dev, &ai, ds);
        check(refused(r), "and after a reset, its overflow pools empty", r);
        ai.pSetLayouts = &small;
        got = 0;
        while (got < 128 && vkAllocateDescriptorSets(dev, &ai, &held[got]) == VK_SUCCESS) got++;
        check(got == 128, "4 pools' worth again after the reset", got);
        vkDestroyDescriptorPool(dev, fp, NULL);
    } else if (!strcmp(mode, "frames")) {
        VkDescriptorPoolSize fps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
        VkDescriptorPoolCreateInfo fpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                           32, 1, &fps};
        VkDescriptorPool fp, dp;
        VkDescriptorSetLayoutBinding pb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo plci_s = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, pb};
        VkDescriptorSetLayout pdsl;
        VkPipelineLayout pl;
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &pdsl, 1, &pcr};
        VkFence fence;
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
        VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, 0, NULL};
        VkPipelineCache cache;
        VkQueue q;
        if (vkCreateDescriptorPool(dev, &fpci, NULL, &fp) || vkCreateDescriptorPool(dev, &dpci, NULL, &dp) ||
            vkCreateDescriptorSetLayout(dev, &plci_s, NULL, &pdsl) || vkCreatePipelineLayout(dev, &plci, NULL, &pl) ||
            vkCreateFence(dev, &fci, NULL, &fence) || vkCreatePipelineCache(dev, &pcci, NULL, &cache)) {
            printf("FAILED: frames setup\n");
            return 1;
        }
        vkGetDeviceQueue(dev, 0, 0, &q);
        VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
        VkShaderModule mod;
        if (vkCreateShaderModule(dev, &mci, NULL, &mod)) {
            printf("FAILED: module\n");
            return 1;
        }
        const char *fu = getenv("HEAPLEAK_FRAME_US"); /* a frame's length (the server's saver runs every 10 s) */
        long frame_us = fu ? atol(fu) : 4000;
        /* HEAPLEAK_SKIP=sets,big,pipe,submit leaves parts out (to find which one the server keeps memory for) */
        const char *skip = getenv("HEAPLEAK_SKIP") ? getenv("HEAPLEAK_SKIP") : "";
        int do_sets = !strstr(skip, "sets"), do_big = !strstr(skip, "big"), do_pipe = !strstr(skip, "pipe"), do_submit = !strstr(skip, "submit");
        int made = 0, sets_ok = 0, big_refused = 0, submits_ok = 0, caches = 1;
        long at2000 = 0;
        for (int f = 0; f < frames; f++) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, fp, 1, &small};
            int k = 0;
            while (do_sets && k < 8 && vkAllocateDescriptorSets(dev, &ai, &ds[k]) == VK_SUCCESS) k++;
            sets_ok += k == 8 || !do_sets;
            if (k) vkFreeDescriptorSets(dev, fp, (uint32_t)k, ds);
            VkDescriptorSetAllocateInfo bi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &big};
            /* every 100th frame: a failed allocation costs the desktop driver memory of its own
             * (RADV: ~0.25 KB each, without the bridge too) */
            big_refused += !do_big || f % 100 || refused(vkAllocateDescriptorSets(dev, &bi, ds));
            /* One module, a new specialization each frame: a new pipeline (and cache entry) for
             * the driver, while the server's copies of module code stay one. */
            uint32_t spec_val = (uint32_t)f;
            VkSpecializationMapEntry me = {777, 0, 4};
            VkSpecializationInfo spec = {1, &me, 4, &spec_val};
            VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                         VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", &spec};
            ci.layout = pl;
            /* Destroyed at once (as engines that only warm their cache do): whatever the server
             * keeps of a pipeline past its destroy shows in its memory. */
            VkPipeline pipe;
            if (!do_pipe) made++;
            else if (vkCreateComputePipelines(dev, cache, 1, &ci, NULL, &pipe) == VK_SUCCESS) {
                made++;
                vkDestroyPipeline(dev, pipe, NULL);
            }
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            if (!do_submit || (vkQueueSubmit(q, 1, &si, fence) == VK_SUCCESS && vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS))
                submits_ok++;
            vkResetFences(dev, 1, &fence);
            if (frame_us) usleep((useconds_t)frame_us);
            if ((f + 1) % 2000 == 0 && f + 1 < frames) {
                /* The game is done with this cache (the pipelines stay); the next compiles go into a new one. */
                vkDestroyPipelineCache(dev, cache, NULL);
                vkCreatePipelineCache(dev, &pcci, NULL, &cache);
                caches++;
                if (f + 1 == 2000) {
                    sleep(12); /* the server's saver (every 10 s) finds its own cache over the limit */
                    at2000 = server_anon_kb();
                    printf("server anon at 2000: %ld KB\n", at2000);
                }
            }
        }
        vkDestroyPipelineCache(dev, cache, NULL);
        sleep(12); /* a save after the last merges */
        long end = server_anon_kb();
        printf("server anon at end: %ld KB\n", end);
        check(made == frames, "a new pipeline every frame", made);
        check(sets_ok == frames, "8 sets every frame from the FREE pool", sets_ok);
        check(big_refused == frames, "the oversized set refused each time", big_refused);
        check(submits_ok == frames, "a submission waited for every frame", submits_ok);
        check(caches == (frames + 1999) / 2000, "the app's caches made and destroyed", caches);
        vkDestroyShaderModule(dev, mod, NULL);
        vkDestroyFence(dev, fence, NULL);
        vkDestroyPipelineLayout(dev, pl, NULL);
        vkDestroyDescriptorSetLayout(dev, pdsl, NULL);
        vkDestroyDescriptorPool(dev, fp, NULL);
        vkDestroyDescriptorPool(dev, dp, NULL);
    } else if (!strcmp(mode, "nullcache")) {
        /* A game that passes no pipeline cache (DXVK, vkd3d-proton): every pipeline goes into the
         * server's own. "seed": NULLCACHE_SEEDS pipelines, saved to the file when the device goes.
         * "play" (the server's limit set just over that file by the test): a new pipeline every frame,
         * destroyed; every 50th made again at once (this session's repeat); every 500th frame 20 of
         * the seeds again (earlier sessions' pipelines, in the file). The server's cache is past its
         * limit from the first save on: it must stay bounded, and the file's pipelines and the
         * repeats must still be found in it (the server's creation-feedback line counts them). */
        const char *phase = argc > 3 ? argv[3] : "play";
        int seeds = getenv("NULLCACHE_SEEDS") ? atoi(getenv("NULLCACHE_SEEDS")) : 400;
        const char *fu = getenv("HEAPLEAK_FRAME_US");
        long frame_us = fu ? atol(fu) : 4000;
        VkDescriptorSetLayoutBinding pb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo plci_s = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, pb};
        VkDescriptorSetLayout pdsl;
        VkPipelineLayout pl;
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &pdsl, 1, &pcr};
        VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
        VkShaderModule mod;
        if (vkCreateDescriptorSetLayout(dev, &plci_s, NULL, &pdsl) || vkCreatePipelineLayout(dev, &plci, NULL, &pl) ||
            vkCreateShaderModule(dev, &mci, NULL, &mod)) {
            printf("FAILED: nullcache setup\n");
            return 1;
        }
        int asked = 0, made = 0, seeds_asked = 0, repeats = 0;
        long at2000 = 0;
#define MAKE(val)                                                                                                     \
    do {                                                                                                              \
        uint32_t spec_val = (uint32_t)(val);                                                                          \
        VkSpecializationMapEntry me = {777, 0, 4};                                                                    \
        VkSpecializationInfo spec = {1, &me, 4, &spec_val};                                                           \
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};                            \
        ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,    \
                                                     VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", &spec};                \
        ci.layout = pl;                                                                                               \
        VkPipeline pipe;                                                                                              \
        asked++;                                                                                                      \
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe) == VK_SUCCESS) {                       \
            made++;                                                                                                   \
            vkDestroyPipeline(dev, pipe, NULL);                                                                       \
        }                                                                                                             \
    } while (0)
        if (!strcmp(phase, "seed")) {
            for (int i = 0; i < seeds; i++) MAKE(1000000 + i);
            check(made == seeds, "the seed pipelines made", made);
        } else {
            for (int f = 0; f < frames; f++) {
                MAKE(f);
                if (f % 50 == 49) {
                    MAKE(f);
                    repeats++;
                }
                if (f % 500 == 499) {
                    for (int i = 0; i < 20; i++) MAKE(1000000 + (f / 500 * 20 + i) % seeds);
                    seeds_asked += 20;
                }
                if (frame_us) usleep((useconds_t)frame_us);
                if (f + 1 == 2000 && f + 1 < frames) {
                    sleep(12);
                    at2000 = server_anon_kb();
                    printf("server anon at 2000: %ld KB\n", at2000);
                }
            }
            sleep(12); /* the saver's turn after the last ones */
            printf("server anon at end: %ld KB\n", server_anon_kb());
            printf("seeds asked again: %d\nrepeats: %d\n", seeds_asked, repeats);
            check(made == asked, "every pipeline made", made);
        }
#undef MAKE
        vkDestroyShaderModule(dev, mod, NULL);
        vkDestroyPipelineLayout(dev, pl, NULL);
        vkDestroyDescriptorSetLayout(dev, pdsl, NULL);
    } else if (!strcmp(mode, "varcount")) {
        /* Risk T10 #14: the refusal memo was per layout. A layout whose last binding has a variable
         * descriptor count: one set of it asked with 8000 (more than a pool like the app's holds) is
         * refused, as the driver refuses it; then, with the app's pool full, a set of the same layout
         * with 16 must still get an overflow pool (the old memo refused it after the app's pool). */
        VkDescriptorSetLayoutBinding vb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8426, VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorBindingFlags vf = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &vf};
        VkDescriptorSetLayoutCreateInfo vci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi, 0, 1, &vb};
        VkDescriptorSetLayout var = VK_NULL_HANDLE;
        VkDescriptorPoolSize vps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
        VkDescriptorPoolCreateInfo vpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                           64, 1, &vps};
        VkDescriptorPool vp = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(dev, &vci, NULL, &var) || vkCreateDescriptorPool(dev, &vpci, NULL, &vp)) {
            printf("FAILED: varcount setup (descriptorBindingVariableDescriptorCount)\n");
            return 1;
        }
        uint32_t want = 8000;
        VkDescriptorSetVariableDescriptorCountAllocateInfo vai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, NULL, 1, &want};
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vai, vp, 1, &var};
        int big_refused = 0;
        for (int k = 0; k < 5; k++) big_refused += refused(vkAllocateDescriptorSets(dev, &ai, ds));
        check(big_refused == 5, "a set of 8000 variable descriptors refused by the empty pool of 256 (as the driver does)", big_refused);
        want = 16;
        static VkDescriptorSet vheld[64];
        int got = 0;
        while (got < 16 && vkAllocateDescriptorSets(dev, &ai, &vheld[got]) == VK_SUCCESS) got++;
        check(got == 16, "16 sets of 16 fill the app's pool", got);
        r = vkAllocateDescriptorSets(dev, &ai, &vheld[got]);
        check(r == VK_SUCCESS, "with the app's pool full, a set of 16 of the same layout gets an overflow pool", r);
        if (r == VK_SUCCESS) got++;
        int more = 0;
        while (more < 8 && vkAllocateDescriptorSets(dev, &ai, &vheld[got]) == VK_SUCCESS) got++, more++;
        check(more == 8, "and 8 more of them", more);
        want = 8000;
        r = vkAllocateDescriptorSets(dev, &ai, ds);
        check(refused(r), "8000 again: still refused", r);
        want = 9000;
        r = vkAllocateDescriptorSets(dev, &ai, ds);
        check(refused(r), "9000 (bigger than one refused): refused", r);
        vkDestroyDescriptorPool(dev, vp, NULL);
        vkDestroyDescriptorSetLayout(dev, var, NULL);
    } else {
        printf("FAILED: mode %s\n", mode);
        return 2;
    }
    vkDestroyDescriptorSetLayout(dev, big, NULL);
    vkDestroyDescriptorSetLayout(dev, small, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
