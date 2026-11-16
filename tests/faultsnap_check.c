/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * faultsnap_check: the programs of host/faultsnap_test.sh: which calls were in the driver AT a contained fault,
 * and which only started after it. a player log (a player's RE8, Mali-G615 r44p1): the list of other calls in flight was made
 * after the fault's jump, 22-30 ms later, so the calls it named had started after the fault.
 *   faultsnap_check hold                one call held in the server (vkCreateEvent; the test holds it with VKBRIDGE_TEST_DELAY_CMD)
 *                                       that is running before and during the fault
 *   faultsnap_check fault               one allocation that the server faults on (VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory)
 *   faultsnap_check late <log> <ms>     waits for the CONTAINED line in the server's log, then <ms> more, then the held call: a
 *                                       call that STARTS after the fault
 *   faultsnap_check bgcompile <s>       a marked graphics pipeline (VKBRIDGE_TEST_CRASH_PIPELINE=slow): with
 *                                       VKBRIDGE_ASYNC_COMPILE=3 its compile runs on the server's background worker; the program
 *                                       stays <s> seconds, then ends
 * Each prints "pid <n>" first, "ready" once its device is made, and one result line last; exit 0 when the call did what the
 * mode expects (fault: the allocation failed; the others: success).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const uint32_t spv_vert[] = {
#include "cullmode_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "dynstate_frag.inc"
};

#define TEST_CRASH_SPEC_ID 0x7E57C0DEu /* server_guard.c */

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
#define G(name) ((PFN_##name)gipa(inst, #name))

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int setup(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 0;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "faultsnap_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (G(vkCreateInstance)(&ici, NULL, &inst) != VK_SUCCESS) return 0;
    uint32_t n = 1;
    G(vkEnumeratePhysicalDevices)(inst, &n, &pd);
    if (!n) return 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    return G(vkCreateDevice)(pd, &dci, NULL, &dev) == VK_SUCCESS;
}

static VkResult held_call(void)
{
    VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent e = VK_NULL_HANDLE;
    VkResult r = G(vkCreateEvent)(dev, &eci, NULL, &e);
    if (r == VK_SUCCESS) G(vkDestroyEvent)(dev, e, NULL);
    return r;
}

/* the CONTAINED line in the server's log, read as it grows (every 1 ms) */
static int wait_contained(const char *log, double limit_ms)
{
    double t0 = now_ms();
    long off = 0;
    char buf[65536 + 1];
    char tail[512] = "";
    while (now_ms() - t0 < limit_ms) {
        FILE *f = fopen(log, "r");
        if (f) {
            if (fseek(f, off, SEEK_SET) == 0) {
                size_t k = fread(buf, 1, sizeof(buf) - 1, f);
                buf[k] = 0;
                off += (long)k;
                /* the line may be split between two reads: look in the last bytes of the one before too */
                static char joined[sizeof(tail) + sizeof(buf)];
                size_t tl = strlen(tail);
                memcpy(joined, tail, tl);
                memcpy(joined + tl, buf, k + 1);
                if (strstr(joined, "CONTAINED driver crash")) {
                    fclose(f);
                    return 1;
                }
                size_t jl = tl + k, keep = jl > sizeof(tail) - 1 ? sizeof(tail) - 1 : jl;
                memcpy(tail, joined + jl - keep, keep);
                tail[keep] = 0;
            }
            fclose(f);
        }
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int bgcompile(int stay_s)
{
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_vert), spv_vert};
    VkShaderModule vs, fs;
    if (G(vkCreateShaderModule)(dev, &smci, NULL, &vs)) return 0;
    smci.codeSize = sizeof(spv_frag), smci.pCode = spv_frag;
    if (G(vkCreateShaderModule)(dev, &smci, NULL, &fs)) return 0;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    VkPipelineLayout layout;
    if (G(vkCreatePipelineLayout)(dev, &plci, NULL, &layout)) return 0;
    VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 0, NULL};
    VkRenderPass rp;
    if (G(vkCreateRenderPass)(dev, &rpci, NULL, &rp)) return 0;
    static const uint32_t mark = 1;
    static const VkSpecializationMapEntry me = {TEST_CRASH_SPEC_ID, 0, 4};
    VkSpecializationInfo si = {1, &me, 4, &mark};
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", &si},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
    };
    VkVertexInputBindingDescription vb = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 0, NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, 4, 4, 0, 1};
    VkRect2D sc = {{0, 0}, {4, 4}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms,
                                       NULL, &cbs, NULL, layout, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = now_ms();
    VkResult r = G(vkCreateGraphicsPipelines)(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    printf("pipeline create returned %d in %.0f ms\n", r, now_ms() - t0);
    fflush(stdout);
    sleep((unsigned)stay_s);
    if (p) G(vkDestroyPipeline)(dev, p, NULL);
    G(vkDestroyRenderPass)(dev, rp, NULL);
    G(vkDestroyPipelineLayout)(dev, layout, NULL);
    G(vkDestroyShaderModule)(dev, vs, NULL);
    G(vkDestroyShaderModule)(dev, fs, NULL);
    return r == VK_SUCCESS;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("pid %d\n", (int)getpid());
    if (!setup()) {
        printf("FAIL setup\n");
        return 2;
    }
    printf("ready\n");
    int ok = 0;
    if (!strcmp(mode, "hold")) {
        double t0 = now_ms();
        VkResult r = held_call();
        printf("held call: %d after %.0f ms\n", r, now_ms() - t0);
        ok = r == VK_SUCCESS;
    } else if (!strcmp(mode, "fault")) {
        VkPhysicalDeviceMemoryProperties mp;
        G(vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 65536, 0};
        VkDeviceMemory m = VK_NULL_HANDLE;
        VkResult r = G(vkAllocateMemory)(dev, &mai, NULL, &m);
        printf("allocation: %d\n", r);
        if (r == VK_SUCCESS) G(vkFreeMemory)(dev, m, NULL);
        ok = r != VK_SUCCESS;
    } else if (!strcmp(mode, "late") && argc > 3) {
        if (!wait_contained(argv[2], 60000)) {
            printf("FAIL no CONTAINED line\n");
            return 1;
        }
        double seen = now_ms();
        struct timespec ts = {0, atoi(argv[3]) * 1000000L};
        nanosleep(&ts, NULL);
        double start = now_ms();
        VkResult r = held_call();
        printf("late call started %.1f ms after the CONTAINED line was seen: %d after %.0f ms\n", start - seen, r, now_ms() - start);
        ok = r == VK_SUCCESS;
    } else if (!strcmp(mode, "bgcompile") && argc > 2) {
        ok = bgcompile(atoi(argv[2]));
    } else {
        printf("FAIL usage: hold | fault | late <log> <ms> | bgcompile <s>\n");
        return 2;
    }
    G(vkDestroyDevice)(dev, NULL);
    G(vkDestroyInstance)(inst, NULL);
    return ok ? 0 : 1;
}
