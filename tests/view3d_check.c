/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * view3d_check: image views whose counts are VK_REMAINING_ARRAY_LAYERS / VK_REMAINING_MIP_LEVELS,
 * rendered to and read back - above all a 2D-array view of a 3D image from slice 1 on, which Mali
 * drivers below r54p1 get wrong (erratum 4222073, MK-07; the bridge gives such views explicit
 * counts, server_errata_views.c). Each case clears the whole image to one colour, renders a clear
 * of another through the view (dynamic rendering, one layer per view layer) and reads every slice
 * and layer of every mip back: what the view covers must have the second colour, the rest the
 * first.
 *
 *   cc -O2 -Ithird_party/Vulkan-Headers-1.4.341/include -o host/out/view3d_check tests/view3d_check.c -ldl
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN(X)                                                                                                                    \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)          \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)       \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)              \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdCopyImageToBuffer) X(vkResetCommandBuffer) X(vkDestroyCommandPool)          \
    X(vkDestroyDevice) X(vkInvalidateMappedMemoryRanges)
#define DECL(n) static PFN_##n n;
FN(DECL)
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct {
    const char *name;
    VkImageType type;
    uint32_t w, h, d, mips, layers;
    VkImageViewType view_type;
    VkImageSubresourceRange range; /* as the app gives it, REMAINING and all */
    /* what the view really covers: mip, and from first to first + count - 1 of its layers/slices */
    uint32_t mip, first, count;
} vcase;

#define CLEAR0 0x11223344u /* the whole image, before */
#define CLEAR1 0xccbbaa99u /* through the view */

static VkClearColorValue colour(uint32_t c)
{
    VkClearColorValue v;
    for (int i = 0; i < 4; i++) v.float32[i] = (float)(c >> (8 * i) & 255) / 255.0f;
    return v;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss,
                    VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void run(const vcase *c)
{
    char detail[256];
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL,
                             c->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0, c->type,
                             VK_FORMAT_R8G8B8A8_UNORM, {c->w, c->h, c->d}, c->mips, c->layers, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkDeviceMemory imem, bmem;
    VkBuffer buf;
    if (vkCreateImage(dev, &ici, NULL, &img)) {
        check(0, c->name, "vkCreateImage failed");
        return;
    }
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = mem_type(r.memoryTypeBits, 0);
    vkAllocateMemory(dev, &ai, NULL, &imem);
    vkBindImageMemory(dev, img, imem, 0);
    /* every texel of every mip, one after another */
    VkDeviceSize offs[16], total = 0;
    for (uint32_t m = 0; m < c->mips; m++) {
        offs[m] = total;
        uint32_t w = c->w >> m ? c->w >> m : 1, h = c->h >> m ? c->h >> m : 1, d = c->d >> m ? c->d >> m : 1;
        total += (VkDeviceSize)w * h * d * c->layers * 4;
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    vkCreateBuffer(dev, &bci, NULL, &buf);
    vkGetBufferMemoryRequirements(dev, buf, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    vkAllocateMemory(dev, &ai, NULL, &bmem);
    vkBindBufferMemory(dev, buf, bmem, 0);
    uint32_t *px;
    vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px);

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, c->view_type, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0}, c->range};
    VkImageView view;
    if (vkCreateImageView(dev, &vci, NULL, &view)) {
        check(0, c->name, "vkCreateImageView failed");
        return;
    }
    uint32_t vw = c->w >> c->mip ? c->w >> c->mip : 1, vh = c->h >> c->mip ? c->h >> c->mip : 1;

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &bi);
    barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue c0 = colour(CLEAR0);
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c->mips, 0, c->layers};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &all);
    barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     VK_ATTACHMENT_STORE_OP_STORE, {.color = colour(CLEAR1)}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {vw, vh}}, c->count, 0, 1, &att, NULL, NULL};
    vkCmdBeginRendering(cb, &ri);
    vkCmdEndRendering(cb);
    barrier(img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (uint32_t m = 0; m < c->mips; m++) {
        uint32_t w = c->w >> m ? c->w >> m : 1, h = c->h >> m ? c->h >> m : 1, d = c->d >> m ? c->d >> m : 1;
        VkBufferImageCopy cp = {offs[m], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, c->layers}, {0, 0, 0}, {w, h, d}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    }
    VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buf, 0, VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VkResult qr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (!qr) qr = vkQueueWaitIdle(queue);
    if (qr) {
        snprintf(detail, sizeof(detail), "submit/wait %d", qr);
        check(0, c->name, detail);
        exit(1); /* a lost device: nothing after it means anything */
    }
    VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, bmem, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(dev, 1, &mr);

    /* every slice/layer of every mip: CLEAR1 exactly where the view is */
    char got[128] = "";
    size_t o = 0;
    int bad = 0;
    for (uint32_t m = 0; m < c->mips; m++) {
        uint32_t w = c->w >> m ? c->w >> m : 1, h = c->h >> m ? c->h >> m : 1, d = c->d >> m ? c->d >> m : 1;
        uint32_t n = c->type == VK_IMAGE_TYPE_3D ? d : c->layers;
        if (o < sizeof(got)) o += snprintf(got + o, sizeof(got) - o, "%smip %u ", m ? ", " : "", m);
        for (uint32_t s = 0; s < n; s++) {
            int in = m == c->mip && s >= c->first && s < c->first + c->count;
            uint32_t want = in ? CLEAR1 : CLEAR0, wrong = 0;
            const uint32_t *p = px + offs[m] / 4 + (size_t)s * w * h;
            for (uint32_t i = 0; i < w * h; i++) wrong += p[i] != want;
            bad += wrong != 0;
            if (o < sizeof(got)) o += snprintf(got + o, sizeof(got) - o, "%c", wrong ? (p[0] == CLEAR1 ? 'V' : p[0] == CLEAR0 ? '.' : '?') : in ? 'v' : '-');
        }
    }
    snprintf(detail, sizeof(detail), "%s view, base mip %u layer %u, %u layer(s) rendered: %s (v/- right, V written outside the "
             "view, . missed inside it)", c->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? "2D_ARRAY" : "2D", c->range.baseMipLevel,
             c->range.baseArrayLayer, c->count, got);
    check(!bad, c->name, detail);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, img, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkFreeMemory(dev, bmem, NULL);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-view3d", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) {
        check(0, "instance", NULL);
        return 1;
    }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties mp_fn = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qf_fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create_dev = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    enumerate(inst, &n, &pd);
    if (!n) {
        check(0, "physical device", "none");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    props_fn(pd, &props);
    mp_fn(pd, &mp);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        printf("SKIP: %s is Vulkan %u.%u, dynamic rendering needs 1.3\nOK: skipped\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
               VK_API_VERSION_MINOR(props.apiVersion));
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
        check(0, "device", NULL);
        return 1;
    }
    check(1, "device", props.deviceName);
#define LOAD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n);
    FN(LOAD)
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cb);

    const uint32_t RL = VK_REMAINING_ARRAY_LAYERS, RM = VK_REMAINING_MIP_LEVELS;
    const VkImageAspectFlags C = VK_IMAGE_ASPECT_COLOR_BIT;
    const vcase cases[] = {
        /* the erratum's own case (the Arm test): 4x4x4, 2D array from slice 1, REMAINING */
        {"3D 4x4x4 as 2D array from slice 1, REMAINING layers", VK_IMAGE_TYPE_3D, 4, 4, 4, 1, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
         {C, 0, 1, 1, RL}, 0, 1, 3},
        {"3D 4x4x4 as 2D array from slice 0, REMAINING layers and mips", VK_IMAGE_TYPE_3D, 4, 4, 4, 1, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
         {C, 0, RM, 0, RL}, 0, 0, 4},
        /* mip 1 of 8x8x8 has 4 slices: from slice 2, two of them */
        {"3D 8x8x8, 3 mips, mip 1 as 2D array from slice 2, REMAINING", VK_IMAGE_TYPE_3D, 8, 8, 8, 3, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
         {C, 1, RM, 2, RL}, 1, 2, 2},
        {"3D 4x4x4, the last slice as 2D, REMAINING layers", VK_IMAGE_TYPE_3D, 4, 4, 4, 1, 1, VK_IMAGE_VIEW_TYPE_2D, {C, 0, 1, 3, RL},
         0, 3, 1},
        {"2D array 4x4x6, 2 mips, mip 1 from layer 2, REMAINING layers", VK_IMAGE_TYPE_2D, 4, 4, 1, 2, 6, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
         {C, 1, 1, 2, RL}, 1, 2, 4},
        {"2D 8x8, 3 mips, the last mip, REMAINING mips and layers", VK_IMAGE_TYPE_2D, 8, 8, 1, 3, 1, VK_IMAGE_VIEW_TYPE_2D,
         {C, 2, RM, 0, RL}, 2, 0, 1},
        /* explicit counts: the reference */
        {"3D 4x4x4 as 2D array from slice 1, explicit 3 layers", VK_IMAGE_TYPE_3D, 4, 4, 4, 1, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
         {C, 0, 1, 1, 3}, 0, 1, 3},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) run(&cases[i]);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures != 0;
}
