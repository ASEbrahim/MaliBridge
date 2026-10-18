/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dedhint_check: the dedicated-allocation hint (client/memory.c, VKBRIDGE_DEDICATED_HINT) as an app
 * sees it in VkMemoryDedicatedRequirements, with Mali's preference imitated beneath it
 * (VKBRIDGE_TEST_PREFER_DEDICATED=1: every answer comes with prefersDedicatedAllocation set):
 *   - an ordinary buffer and image, through vkGet{Buffer,Image}MemoryRequirements2 (the buffer
 *     twice: the second answer comes from the client's cache) and through
 *     vkGetDevice{Buffer,Image}MemoryRequirements (Vulkan 1.3): preference cleared with "clear",
 *     kept with "pass";
 *   - a 64 MiB image: kept either way;
 *   - a buffer and an image the app made for external memory (opaque fd or dma-buf, whichever the
 *     driver makes): kept either way, and requiresDedicatedAllocation never cleared (where the
 *     driver requires it, the preference is still set).
 *
 *   dedhint_check clear   the switch on (VKBRIDGE_DEDICATED_HINT=1)
 *   dedhint_check pass    the switch off (=0, or the default on a GPU that is not Arm's)
 * Exit status 0 only if every check passes.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
#define GD(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

static void expect(const char *what, const VkMemoryRequirements2 *r, const VkMemoryDedicatedRequirements *d, int prefers)
{
    char detail[160];
    snprintf(detail, sizeof(detail), "prefers %u requires %u (want prefers %d), %llu bytes", d->prefersDedicatedAllocation,
             d->requiresDedicatedAllocation, prefers, (unsigned long long)r->memoryRequirements.size);
    check(!!d->prefersDedicatedAllocation == prefers && (!d->requiresDedicatedAllocation || d->prefersDedicatedAllocation), what, detail);
}

int main(int argc, char **argv)
{
    int clear = argc > 1 && !strcmp(argv[1], "clear");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dedhint_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        check(0, "device", "");
        return 1;
    }
    GD(vkCreateBuffer);
    GD(vkDestroyBuffer);
    GD(vkCreateImage);
    GD(vkDestroyImage);
    GD(vkGetBufferMemoryRequirements2);
    GD(vkGetImageMemoryRequirements2);
    GD(vkDestroyDevice);
    PFN_vkGetDeviceBufferMemoryRequirements dbuf = NULL;
    PFN_vkGetDeviceImageMemoryRequirements dimg = NULL;
    if (props.apiVersion >= VK_API_VERSION_1_3) {
        dbuf = (PFN_vkGetDeviceBufferMemoryRequirements)gdpa(dev, "vkGetDeviceBufferMemoryRequirements");
        dimg = (PFN_vkGetDeviceImageMemoryRequirements)gdpa(dev, "vkGetDeviceImageMemoryRequirements");
    }

    VkMemoryDedicatedRequirements d = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 r = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &d};
#define RESET() (d.pNext = NULL, d.prefersDedicatedAllocation = d.requiresDedicatedAllocation = 7, r.pNext = &d)

    /* An ordinary buffer, twice (the second answer from the client's cache) */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4u << 20,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf, buf2;
    if (vkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS || vkCreateBuffer(dev, &bci, NULL, &buf2) != VK_SUCCESS) {
        check(0, "buffer", "not created");
        return 1;
    }
    VkBufferMemoryRequirementsInfo2 bri = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, NULL, buf};
    RESET();
    vkGetBufferMemoryRequirements2(dev, &bri, &r);
    expect("ordinary buffer", &r, &d, !clear);
    bri.buffer = buf2;
    RESET();
    vkGetBufferMemoryRequirements2(dev, &bri, &r);
    expect("ordinary buffer, cached answer", &r, &d, !clear);
    if (dbuf) {
        VkDeviceBufferMemoryRequirements dbr = {VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, NULL, &bci};
        RESET();
        dbuf(dev, &dbr, &r);
        expect("ordinary buffer, vkGetDeviceBufferMemoryRequirements", &r, &d, !clear);
    }

    /* An ordinary texture */
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {512, 512, 1}, 10, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ii, NULL, &img) != VK_SUCCESS) {
        check(0, "image", "not created");
        return 1;
    }
    VkImageMemoryRequirementsInfo2 iri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
    RESET();
    vkGetImageMemoryRequirements2(dev, &iri, &r);
    expect("ordinary image", &r, &d, !clear);
    if (dimg) {
        VkDeviceImageMemoryRequirements dir = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, NULL, &ii, 0};
        RESET();
        dimg(dev, &dir, &r);
        expect("ordinary image, vkGetDeviceImageMemoryRequirements", &r, &d, !clear);
    }

    /* A large render target: keeps it */
    VkImageCreateInfo big = ii;
    big.extent = (VkExtent3D){4096, 4096, 1};
    big.mipLevels = 1;
    big.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage bigimg;
    if (vkCreateImage(dev, &big, NULL, &bigimg) == VK_SUCCESS) {
        iri.image = bigimg;
        RESET();
        vkGetImageMemoryRequirements2(dev, &iri, &r);
        expect("64 MiB render target", &r, &d, 1);
        vkDestroyImage(dev, bigimg, NULL);
    } else {
        check(0, "64 MiB render target", "not created");
    }

    /* Made for external memory by the app: keeps it, whatever the switch */
    static const VkExternalMemoryHandleTypeFlagBits types[] = {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int ext_img = 0, ext_buf = 0;
    for (int t = 0; t < 2; t++) {
        char what[96];
        if (!ext_img) {
            VkExternalMemoryImageCreateInfo eii = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, types[t]};
            VkImageCreateInfo ei = ii;
            ei.pNext = &eii;
            ei.mipLevels = 1;
            VkImage eimg;
            if (vkCreateImage(dev, &ei, NULL, &eimg) == VK_SUCCESS) {
                ext_img = 1;
                iri.image = eimg;
                RESET();
                vkGetImageMemoryRequirements2(dev, &iri, &r);
                snprintf(what, sizeof(what), "image made for external memory (handle type 0x%x)", types[t]);
                expect(what, &r, &d, 1);
                vkDestroyImage(dev, eimg, NULL);
            }
        }
        if (!ext_buf) {
            VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, types[t]};
            VkBufferCreateInfo eb = bci;
            eb.pNext = &ebi;
            VkBuffer ebuf;
            if (vkCreateBuffer(dev, &eb, NULL, &ebuf) == VK_SUCCESS) {
                ext_buf = 1;
                bri.buffer = ebuf;
                RESET();
                vkGetBufferMemoryRequirements2(dev, &bri, &r);
                snprintf(what, sizeof(what), "buffer made for external memory (handle type 0x%x)", types[t]);
                expect(what, &r, &d, 1);
                vkDestroyBuffer(dev, ebuf, NULL);
            }
        }
    }
    if (!ext_img) printf("SKIP image made for external memory: the driver made none\n");
    if (!ext_buf) printf("SKIP buffer made for external memory: the driver made none\n");

    /* The ordinary image again, after the external ones came and went (no stale entries) */
    iri.image = img;
    RESET();
    vkGetImageMemoryRequirements2(dev, &iri, &r);
    expect("ordinary image, asked again", &r, &d, !clear);

    vkDestroyImage(dev, img, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkDestroyBuffer(dev, buf2, NULL);
    vkDestroyDevice(dev, NULL);
    if (failures) printf("FAIL: %d of %d checks failed (%s)\n", failures, checks, clear ? "clear" : "pass");
    else printf("OK: %d checks (%s)\n", checks, clear ? "clear" : "pass");
    return failures != 0;
}
