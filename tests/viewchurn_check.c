/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * viewchurn_check: image views made, rendered to with dynamic rendering and destroyed, over and
 * over - the pattern that Mali drivers r49p1-r51 get wrong when a destroyed view's handle value
 * comes back for a new view (erratum 3786926, MK-06: vkCmdBeginRendering's cache hits the stale
 * entry; the bridge holds such views back, server_errata_views.c). Each iteration clears a fresh
 * view of one of a few small images (two formats) to its own colour and copies it out; every
 * copy must have its iteration's colour. Prints how many handle values came back.
 *
 *   viewchurn_check [iterations [batch]] [--images]
 *     iterations  default 500; batch: iterations per submit (default 16)
 *     --images    the images are made and destroyed each batch too (their memory freed first)
 *
 *   cc -O2 -Ithird_party/Vulkan-Headers-1.4.341/include -o host/out/viewchurn_check tests/viewchurn_check.c -ldl
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FN(X)                                                                                                                    \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)          \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)       \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdPipelineBarrier) X(vkCmdBeginRendering)               \
    X(vkCmdEndRendering) X(vkCmdCopyImageToBuffer) X(vkResetCommandBuffer) X(vkDestroyCommandPool) X(vkDestroyDevice)             \
    X(vkInvalidateMappedMemoryRanges)
#define DECL(n) static PFN_##n n;
FN(DECL)

#define NIMG 7
#define W 4
#define MAXB 64

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkFormat fmt;
} image;

static int make_image(image *im, VkFormat fmt)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, W, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    im->fmt = fmt;
    if (vkCreateImage(dev, &ici, NULL, &im->img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, im->img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = mem_type(r.memoryTypeBits, 0);
    return !vkAllocateMemory(dev, &ai, NULL, &im->mem) && !vkBindImageMemory(dev, im->img, im->mem, 0);
}

/* iteration i's colour, as bytes in memory for the format */
static uint32_t want_px(uint32_t i, VkFormat f)
{
    uint32_t r = i & 255, g = (i >> 8) & 255, b = 0x5a, a = 0xff;
    return f == VK_FORMAT_B8G8R8A8_UNORM ? b | g << 8 | r << 16 | a << 24 : r | g << 8 | b << 16 | a << 24;
}

/* the handle values seen so far (a small open hash): 1 when v was seen before */
static uint64_t *seen;
static size_t seen_cap, seen_n;
static int seen_before(uint64_t v)
{
    if (seen_n * 2 >= seen_cap) {
        size_t oc = seen_cap;
        uint64_t *old = seen;
        seen_cap = oc ? oc * 2 : 4096;
        seen = calloc(seen_cap, sizeof(*seen));
        seen_n = 0;
        for (size_t i = 0; i < oc; i++)
            if (old[i]) seen_before(old[i]);
        free(old);
    }
    size_t h = (size_t)((v * 0x9e3779b97f4a7c15ull) >> 20) & (seen_cap - 1);
    while (seen[h]) {
        if (seen[h] == v) return 1;
        h = (h + 1) & (seen_cap - 1);
    }
    seen[h] = v;
    seen_n++;
    return 0;
}

int main(int argc, char **argv)
{
    int iters = 500, batch = 16, images_too = 0, pos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--images")) images_too = 1;
        else if (pos++ == 0) iters = atoi(argv[i]);
        else batch = atoi(argv[i]);
    }
    if (batch < 1 || batch > MAXB) batch = 16;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-viewchurn", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL instance\n");
        return 1;
    }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties mp_fn = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qf_fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create_dev = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    enumerate(inst, &n, &pd);
    if (!n) {
        printf("FAIL physical device: none\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    props_fn(pd, &props);
    mp_fn(pd, &mp);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        printf("SKIP: %s has no Vulkan 1.3\nOK: skipped\n", props.deviceName);
        return 0;
    }
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    qf_fn(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (create_dev(pd, &dci, NULL, &dev)) {
        printf("FAIL device\n");
        return 1;
    }
    printf("PASS device: %s\n", props.deviceName);
#define LOAD(n) n = (PFN_##n)gdpa(dev, #n);
    FN(LOAD)
    VkQueue queue;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cb);

    VkBuffer buf;
    VkDeviceMemory bmem;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, MAXB * W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, buf, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    vkAllocateMemory(dev, &ai, NULL, &bmem);
    vkBindBufferMemory(dev, buf, bmem, 0);
    uint32_t *px;
    vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px);

    image imgs[NIMG];
    if (!images_too)
        for (int i = 0; i < NIMG; i++)
            if (!make_image(&imgs[i], formats[i & 1])) {
                printf("FAIL image setup\n");
                return 1;
            }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int wrong = 0, reused = 0, first_wrong = -1;
    for (int base = 0; base < iters; base += batch) {
        int nb = iters - base < batch ? iters - base : batch;
        if (images_too)
            for (int i = 0; i < NIMG; i++)
                if (!make_image(&imgs[i], formats[(base / batch + i) & 1])) {
                    printf("FAIL image setup at iteration %d\n", base);
                    return 1;
                }
        VkImageView views[MAXB];
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        for (int j = 0; j < nb; j++) {
            int it = base + j;
            image *im = &imgs[it % NIMG];
            VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->img, VK_IMAGE_VIEW_TYPE_2D, im->fmt, {0},
                                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (vkCreateImageView(dev, &vci, NULL, &views[j])) {
                printf("FAIL vkCreateImageView at iteration %d\n", it);
                return 1;
            }
            reused += seen_before((uint64_t)views[j]);
            VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED, im->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            VkClearValue cv = {.color = {{(float)(it & 255) / 255.0f, (float)((it >> 8) & 255) / 255.0f, (float)0x5a / 255.0f, 1.0f}}};
            VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, views[j], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                             VK_ATTACHMENT_STORE_OP_STORE, cv};
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {W, W}}, 1, 0, 1, &att, NULL, NULL};
            vkCmdBeginRendering(cb, &ri);
            vkCmdEndRendering(cb);
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            VkBufferImageCopy cp = {(VkDeviceSize)j * W * W * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
            vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
        }
        VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buf, 0, VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        VkResult qr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (!qr) qr = vkQueueWaitIdle(queue);
        if (qr) {
            printf("FAIL submit/wait at iteration %d: %d\nFAILED: device lost\n", base, qr);
            return 1;
        }
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, bmem, 0, VK_WHOLE_SIZE};
        vkInvalidateMappedMemoryRanges(dev, 1, &mr);
        for (int j = 0; j < nb; j++) {
            int it = base + j;
            uint32_t want = want_px(it, imgs[it % NIMG].fmt), bad = 0;
            for (int k = 0; k < W * W; k++) bad += px[j * W * W + k] != want;
            if (bad) {
                if (first_wrong < 0) first_wrong = it, printf("  iteration %d: 0x%08x, want 0x%08x\n", it, px[j * W * W], want);
                wrong++;
            }
            vkDestroyImageView(dev, views[j], NULL);
        }
        if (images_too)
            for (int i = 0; i < NIMG; i++) {
                vkFreeMemory(dev, imgs[i].mem, NULL); /* before the image: allowed, and what the deferral must survive */
                vkDestroyImage(dev, imgs[i].img, NULL);
            }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (!images_too)
        for (int i = 0; i < NIMG; i++) {
            vkDestroyImage(dev, imgs[i].img, NULL);
            vkFreeMemory(dev, imgs[i].mem, NULL);
        }
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    printf("%s churn: %d iteration(s)%s, %d wrong (first at %d), %d view handle value(s) given out again, %.0f ms\n", wrong ? "FAIL" : "PASS",
           iters, images_too ? " with images made and destroyed" : "", wrong, first_wrong, reused, ms);
    if (wrong) printf("FAILED: %d of %d iterations wrong; %d handle values reused\n", wrong, iters, reused);
    else printf("OK: %d iterations, %d handle values reused\n", iters, reused);
    return wrong != 0;
}
