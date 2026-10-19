/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * exitclean_check: processes that exit without destroying anything while another keeps importing.
 * On a Tab S10+ (v0.5.1 rc1) two steamwebhelper processes exited without destroying their devices
 * when a game started, and the server crashed in libc at address 0 in the second one's cleanup:
 * its null-descriptor stand-in buffer was destroyed through the dedicated_import erratum's wrapper
 * on a thread without a call's table, which took the state of the last device made - Steam's GPU
 * query's, destroyed a minute before - and locked its NULL mutex (server_errata_mem.c).
 * Here, in a few rounds, two victim processes make what a Zink/CEF process holds - mapped memory,
 * exported dma-bufs imported back with and without the dedicated struct (images and buffers),
 * texel buffer views, a pipeline cache, a compute pipeline, descriptor sets, a null descriptor
 * (robustness2, emulated on Mali), a submission - then, every other round, a bystander process
 * makes a device and destroys it as it should, and the victims _exit(). Meanwhile a survivor
 * process exports and imports dma-bufs (dedicated and not), makes and destroys buffer views and
 * images, and at the end destroys its device; it must finish every round with VK_SUCCESS (a dead
 * server fails it). The suite runs it with Mali's errata (VKBRIDGE_FAKE_DRIVER_VERSION=r44p1) and
 * checks the server log for "crashed with signal" and every victim's "cleaning up".
 *   exitclean_check [rounds]   (default 6)
 * Needs VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf (SKIP otherwise).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "smoke_spv.h"

#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkEnumerateDeviceExtensionProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyDevice); I(vkDestroyInstance);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkFreeMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdFillBuffer); I(vkCreateImage); I(vkDestroyImage);
I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkGetMemoryFdKHR); I(vkGetMemoryFdPropertiesKHR); I(vkCreateBufferView);
I(vkDestroyBufferView); I(vkCreatePipelineCache); I(vkCreateShaderModule); I(vkCreateDescriptorSetLayout); I(vkCreatePipelineLayout);
I(vkCreateComputePipelines); I(vkCreateDescriptorPool); I(vkAllocateDescriptorSets); I(vkUpdateDescriptorSets);
I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets); I(vkCmdDispatch);
#undef I

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* 0: the device is up; 1: no dma-buf extensions (SKIP); -1: failed */
static int null_desc; /* robustness2's nullDescriptor enabled (the bridge emulates it on Mali) */

static int setup(const char *name, int want_null)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return -1;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, name, 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) return -1;
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkEnumerateDeviceExtensionProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkDestroyInstance);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return -1;
    const char *want[3] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_robustness2"};
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int have = 0, r2 = 0;
    for (uint32_t i = 0; i < ne; i++) {
        for (int k = 0; k < 2; k++) have += !strcmp(ext[i].extensionName, want[k]);
        r2 |= !strcmp(ext[i].extensionName, want[2]);
    }
    free(ext);
    if (have < 2) return 1;
    null_desc = want_null && r2;
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceRobustness2FeaturesEXT er2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    er2.nullDescriptor = VK_TRUE;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &er2};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, null_desc ? &ef : NULL, 0, 1, &qci, 0, NULL, null_desc ? 3 : 2, want, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return -1;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkDestroyBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkFreeMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCmdFillBuffer); D(vkCreateImage); D(vkDestroyImage);
    D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkGetMemoryFdKHR); D(vkGetMemoryFdPropertiesKHR); D(vkCreateBufferView);
    D(vkDestroyBufferView); D(vkCreatePipelineCache); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout);
    D(vkCreateComputePipelines); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets);
    D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets); D(vkCmdDispatch);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    return 0;
}

static VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, int external)
{
    VkExternalMemoryBufferCreateInfo x = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, DMABUF};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, external ? &x : NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE;
    return vkCreateBuffer(dev, &bci, NULL, &b) ? VK_NULL_HANDLE : b;
}

static VkResult bound(VkBuffer b, VkMemoryPropertyFlags want, const void *pNext, VkDeviceMemory *out)
{
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, pNext, mr.size, find_type(mr.memoryTypeBits, want)};
    if (ai.memoryTypeIndex == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, out);
    return r ? r : vkBindBufferMemory(dev, b, *out, 0);
}

/* One exported dma-buf (a buffer's memory) and its imports: an image with the dedicated struct, an
 * image without it, a buffer with it. Everything made is returned in the arrays when kept, else
 * destroyed. */
typedef struct {
    VkBuffer bufs[4];
    VkImage imgs[2];
    VkDeviceMemory mems[4];
    int nb, ni, nm;
} objs;

static VkResult share_round(objs *o, uint32_t w, uint32_t h)
{
    VkDeviceSize size = 4u << 20;
    VkBuffer src = buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    if (!src) return VK_ERROR_OUT_OF_HOST_MEMORY;
    o->bufs[o->nb++] = src;
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, DMABUF};
    VkDeviceMemory smem;
    VkResult r = bound(src, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &exp, &smem);
    if (r) return r;
    o->mems[o->nm++] = smem;
    int fd = -1;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, smem, DMABUF};
    if ((r = vkGetMemoryFdKHR(dev, &gi, &fd))) return r;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if ((r = vkGetMemoryFdPropertiesKHR(dev, DMABUF, fd, &fp))) {
        close(fd);
        return r;
    }
    for (int k = 0; k < 2; k++) {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        if ((r = vkCreateImage(dev, &ic, NULL, &img))) break;
        o->imgs[o->ni++] = img;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, img, &mr);
        uint32_t bits = mr.memoryTypeBits & fp.memoryTypeBits;
        uint32_t t = find_type(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (t == UINT32_MAX) t = find_type(bits, 0);
        if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0); /* as the client offers it: the server fits the type */
        if (t == UINT32_MAX || mr.size > size) continue;
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE};
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, k == 0 ? &ded : NULL, DMABUF, dup(fd)};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, t};
        VkDeviceMemory m;
        r = vkAllocateMemory(dev, &ai, NULL, &m);
        if (r) {
            close(imp.fd);
            if (k == 1) r = VK_SUCCESS; /* without the struct a driver may refuse it */
            continue;
        }
        o->mems[o->nm++] = m;
        if ((r = vkBindImageMemory(dev, img, m, 0))) break;
    }
    if (!r) {
        VkBuffer b = buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, 1);
        if (b) {
            o->bufs[o->nb++] = b;
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(dev, b, &mr);
            uint32_t t = find_type(mr.memoryTypeBits & fp.memoryTypeBits, 0);
            if (t != UINT32_MAX && mr.size <= size) {
                VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, b};
                VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, DMABUF, dup(fd)};
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, t};
                VkDeviceMemory m;
                r = vkAllocateMemory(dev, &ai, NULL, &m);
                if (r) close(imp.fd);
                else {
                    o->mems[o->nm++] = m;
                    r = vkBindBufferMemory(dev, b, m, 0);
                }
            }
        }
    }
    close(fd);
    return r;
}

static void destroy(objs *o)
{
    for (int i = 0; i < o->ni; i++) vkDestroyImage(dev, o->imgs[i], NULL);
    for (int i = 0; i < o->nb; i++) vkDestroyBuffer(dev, o->bufs[i], NULL);
    for (int i = 0; i < o->nm; i++) vkFreeMemory(dev, o->mems[i], NULL);
    memset(o, 0, sizeof(*o));
}

static VkBufferView texel_view(VkBuffer b, VkDeviceSize off)
{
    VkBufferViewCreateInfo vci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, VK_FORMAT_R32_UINT, off, 4096};
    VkBufferView v = VK_NULL_HANDLE;
    return vkCreateBufferView(dev, &vci, NULL, &v) ? VK_NULL_HANDLE : v;
}

/* What a dying Zink process holds, then _exit without destroying any of it. */
static int victim(int round, int ready, int go)
{
    char name[32];
    snprintf(name, sizeof(name), "exitclean-victim-%d", round);
    if (setup(name, 1)) _exit(3);
    objs o[3] = {{{0}}};
    for (int i = 0; i < 3; i++)
        if (share_round(&o[i], 24 + 8 * i, 24)) _exit(4);
    /* mapped host memory, the buffer views on an imported buffer and on a plain one */
    VkBuffer hb = buffer(1u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    VkDeviceMemory hm;
    void *map;
    if (!hb || bound(hb, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, NULL, &hm) || vkMapMemory(dev, hm, 0, VK_WHOLE_SIZE, 0, &map)) _exit(5);
    memset(map, 0x5a, 4096);
    texel_view(hb, 0);
    texel_view(o[0].bufs[o[0].nb - 1], 0);
    /* a pipeline cache, a compute pipeline, a descriptor set */
    VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache pc;
    vkCreatePipelineCache(dev, &pcci, NULL, &pc);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
    VkShaderModule sm;
    if (vkCreateShaderModule(dev, &smci, NULL, &sm)) _exit(6);
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb};
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl);
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL},
                                        pl, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    if (vkCreateComputePipelines(dev, pc, 1, &cpci, NULL, &pipe)) _exit(7);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 4, 1, &ps};
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
    VkDescriptorSet ds;
    vkAllocateDescriptorSets(dev, &dsai, &ds);
    VkDescriptorBufferInfo dbi = {hb, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
    if (null_desc) {
        /* a null descriptor: on Mali the server's stand-ins (a buffer, an image, views) are made */
        VkDescriptorSet ds2;
        vkAllocateDescriptorSets(dev, &dsai, &ds2);
        VkDescriptorBufferInfo nbi = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet nw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds2, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &nbi, NULL};
        vkUpdateDescriptorSets(dev, 1, &nw, 0, NULL);
    }
    /* a submission left in flight */
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cp;
    vkCreateCommandPool(dev, &cpi, NULL, &cp);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 64, 1, 1);
    vkCmdFillBuffer(cb, hb, 0, 1u << 20, 0x01020304);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    /* one more share whose answers it never waits for, then gone */
    objs last = {{0}};
    share_round(&last, 16, 16);
    vkQueueWaitIdle(queue); /* every request has reached the server */
    char c = 1;
    if (write(ready, &c, 1) != 1 || read(go, &c, 1) != 1) _exit(8);
    _exit(0);
}

/* A process that makes a device and destroys it as it should: it is the last device made when the
 * victims go (on the tablet Steam's GPU query, destroyed 54 s before the crash). */
static int bystander(void)
{
    if (setup("exitclean-bystander", 0)) _exit(3);
    VkBuffer b = buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    if (b) vkDestroyBuffer(dev, b, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    _exit(0);
}

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

/* gamescope's side: exports and imports, views and images made and destroyed, until told to stop. */
static int survivor(void)
{
    if (setup("exitclean-survivor", 0)) _exit(3);
    signal(SIGUSR1, on_term);
    unsigned rounds = 0;
    VkBuffer hb = buffer(1u << 20, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, 0);
    VkDeviceMemory hm;
    if (!hb || bound(hb, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, NULL, &hm)) _exit(5);
    while (!stop) {
        objs o = {{0}};
        VkResult r = share_round(&o, 24, 24);
        if (r) {
            printf("FAIL survivor: share round %u: VkResult %d\n", rounds, r);
            fflush(stdout);
            _exit(1);
        }
        VkBufferView v1 = texel_view(hb, 0), v2 = texel_view(hb, 4096);
        if (!v1 || !v2) {
            printf("FAIL survivor: buffer views in round %u\n", rounds);
            fflush(stdout);
            _exit(1);
        }
        vkDestroyBufferView(dev, v1, NULL);
        vkDestroyBufferView(dev, v2, NULL);
        destroy(&o);
        rounds++;
    }
    vkDestroyBuffer(dev, hb, NULL);
    vkFreeMemory(dev, hm, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("survivor: %u rounds of export, import and views\n", rounds);
    fflush(stdout);
    _exit(0);
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 6;
    /* the extensions, from a process of its own (nothing initialised here before the forks) */
    pid_t probe = fork();
    if (probe == 0) _exit(setup("exitclean-probe", 0) == 1 ? 1 : 0);
    int st;
    waitpid(probe, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 1) {
        printf("SKIP no dma-buf export/import\nOK: 0 of 0 checks passed\n");
        return 0;
    }
    fflush(stdout);
    pid_t surv = fork();
    if (surv == 0) survivor();
    int failures = 0;
    for (int i = 0; i < rounds; i++) {
        /* two at once, as the two steamwebhelpers went */
        pid_t v[2];
        int rp[2], gp[2];
        if (pipe(rp) || pipe(gp)) return 1;
        for (int k = 0; k < 2; k++)
            if ((v[k] = fork()) == 0) victim(i * 2 + k, rp[1], gp[0]);
        close(rp[1]);
        close(gp[0]);
        char c;
        int ready = 0;
        while (ready < 2 && read(rp[0], &c, 1) == 1) ready++;
        /* every other round a device made and destroyed after the victims' */
        if (i % 2 == 0) {
            pid_t b = fork();
            if (b == 0) bystander();
            waitpid(b, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st)) {
                printf("FAIL bystander %d exited with %d\n", i, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
                failures++;
            }
        }
        for (int k = 0; k < 2; k++)
            if (write(gp[1], "g", 1) != 1) failures++;
        close(gp[1]);
        close(rp[0]);
        for (int k = 0; k < 2; k++) {
            waitpid(v[k], &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st)) {
                printf("FAIL victim %d exited with %d\n", i * 2 + k, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
                failures++;
            }
        }
        struct timespec ts = {0, 150000000};
        nanosleep(&ts, NULL); /* the cleanup runs while the survivor goes on */
        if (waitpid(surv, &st, WNOHANG) == surv) {
            printf("FAIL the survivor stopped in round %d (status %d)\n", i, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
            printf("FAIL: the survivor stopped\n");
            return 1;
        }
    }
    kill(surv, SIGUSR1);
    waitpid(surv, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("FAIL the survivor ended with %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
        failures++;
    }
    if (failures)
        printf("FAIL: %d of %d processes failed\n", failures, rounds * 2 + 1);
    else
        printf("OK: %d victims exited without destroying anything, the survivor went on\n", rounds * 2);
    return failures ? 1 : 0;
}
