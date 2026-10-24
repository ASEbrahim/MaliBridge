/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * compile_probe: how long the GPU driver takes to compile a graphics pipeline from given SPIR-V,
 * each in a child process killed after a timeout - for bisecting shaders a driver's compiler
 * never finishes (the Mali-G925 and Stray's colour-grading LUT, 2026-10-14) with rewritten
 * variants, natively on the device (no bridge involved).
 *
 *   compile_probe [--timeout S] --vs vs.spv [--gs gs.spv] fs.spv [fs2.spv ...]
 *
 * One line per fragment shader: "OK <ms>", "FAIL <VkResult>" or "TIMEOUT". The pipeline layout is
 * made from the stages' descriptor declarations (uniform/storage buffers, images, samplers,
 * combined image samplers; fixed-size arrays; runtime arrays as 64 with variable count off) plus
 * a 256-byte push constant range; one R8G8B8A8 colour attachment (dynamic rendering).
 *
 * Host: cc -O2 -Ithird_party/Vulkan-Headers-1.4.341/include -o compile_probe tests/compile_probe.c -ldl
 * Android: <ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang -O2 \
 *          -Ithird_party/Vulkan-Headers-1.4.341/include -o compile_probe tests/compile_probe.c -ldl
 */
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

static uint32_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    uint32_t *b = malloc(*n);
    if (b && fread(b, 1, *n, f) != *n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

#define MAXB 64
typedef struct binding {
    uint32_t set, binding, type, count;
} binding;
static binding binds[MAXB];
static uint32_t nbinds;

/* Descriptor declarations of one module (enough of SPIR-V for that). */
static void scan(const uint32_t *w, size_t bytes)
{
    size_t n = bytes / 4;
    enum { MAXID = 1 << 18 };
    uint32_t *set = calloc(MAXID, 4), *bind = calloc(MAXID, 4), *dec = calloc(MAXID, 1), *def = calloc(MAXID, 4);
    uint8_t *block = calloc(MAXID, 1), *buffer_block = calloc(MAXID, 1);
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc) break;
        if (op == 71 && wc >= 4 && w[i + 1] < MAXID) { /* OpDecorate */
            uint32_t id = w[i + 1], d = w[i + 2];
            if (d == 34) set[id] = w[i + 3], dec[id] |= 1;
            if (d == 33) bind[id] = w[i + 3], dec[id] |= 2;
        }
        if (op == 71 && wc >= 3 && w[i + 1] < MAXID && w[i + 2] == 2) block[w[i + 1]] = 1;
        if (op == 71 && wc >= 3 && w[i + 1] < MAXID && w[i + 2] == 3) buffer_block[w[i + 1]] = 1;
        uint32_t rid = 0;
        if ((op >= 19 && op <= 39) || op == 322) rid = w[i + 1]; /* types define their id first */
        if (op == 59) rid = w[i + 2];                            /* OpVariable */
        if (rid && rid < MAXID) def[rid] = (uint32_t)i;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc) break;
        if (op == 59 && w[i + 2] < MAXID && (dec[w[i + 2]] & 3) == 3) {
            uint32_t sc = w[i + 3], t = w[def[w[i + 1]] + 3], count = 1; /* pointer's pointee */
            uint32_t to = w[def[t]] & 0xffff;
            if (to == 28) { /* OpTypeArray: length constant */
                uint32_t len = w[def[t]] ? w[def[t] + 3] : 1;
                count = 1;
                for (size_t k = 5; k < n;) { /* find OpConstant len */
                    uint32_t kc = w[k] >> 16;
                    if (!kc) break;
                    if ((w[k] & 0xffff) == 43 && w[k + 2] == len) count = w[k + 3];
                    k += kc;
                }
                t = w[def[t] + 2];
            } else if (to == 29) { /* runtime array */
                count = 64;
                t = w[def[t] + 2];
            }
            uint32_t top = w[def[t]] & 0xffff, type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (sc == 12 || (sc == 2 && buffer_block[t])) type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if (top == 27) type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (top == 26) type = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if (top == 25) {
                uint32_t sampled = w[def[t] + 7], dim = w[def[t] + 3];
                type = dim == 5 ? (sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                       : sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            }
            uint32_t id = w[i + 2];
            int dup = 0;
            for (uint32_t k = 0; k < nbinds; k++) dup |= binds[k].set == set[id] && binds[k].binding == bind[id];
            if (!dup && nbinds < MAXB) binds[nbinds++] = (binding){set[id], bind[id], type, count};
        }
        i += wc;
    }
    free(set), free(bind), free(dec), free(def), free(block), free(buffer_block);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Child: build the pipeline once; exit 0 on success, else 100 + |VkResult| (capped). */
static int compile(const uint32_t *vs, size_t vsn, const uint32_t *gs, size_t gsn, const uint32_t *fs, size_t fsn)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) return 99;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define IP(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "compile_probe", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst)) return 98;
    IP(vkEnumeratePhysicalDevices); IP(vkCreateDevice); IP(vkGetDeviceProcAddr); IP(vkGetPhysicalDeviceFeatures2);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) return 97;
    VkPhysicalDeviceShaderFloatControls2Features fc2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, getenv("PROBE_NO_FC2") ? NULL : &fc2};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2); /* everything the device has, on (as a game's translation layer would) */
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char *exts[] = {"VK_KHR_shader_float_controls2"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, getenv("PROBE_NO_FC2") ? 0 : 1, exts, NULL};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        dci.enabledExtensionCount = 0;
        f13.pNext = NULL;
        if (vkCreateDevice(pd, &dci, NULL, &dev)) return 96;
    }
#define DP(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    DP(vkCreateShaderModule); DP(vkCreateDescriptorSetLayout); DP(vkCreatePipelineLayout); DP(vkCreateGraphicsPipelines);
    VkDescriptorSetLayout dsl[8] = {0};
    uint32_t nsets = 0;
    for (uint32_t i = 0; i < nbinds; i++)
        if (binds[i].set + 1 > nsets) nsets = binds[i].set + 1;
    if (nsets > 8) nsets = 8;
    for (uint32_t s = 0; s < nsets; s++) {
        VkDescriptorSetLayoutBinding b[MAXB];
        uint32_t nb = 0;
        for (uint32_t i = 0; i < nbinds; i++)
            if (binds[i].set == s) b[nb++] = (VkDescriptorSetLayoutBinding){binds[i].binding, binds[i].type, binds[i].count, VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b};
        vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl[s]);
    }
    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL_GRAPHICS, 0, 256};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, nsets, dsl, 1, &pcr};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkShaderModule m[3];
    const uint32_t *code[3] = {vs, gs, fs};
    size_t size[3] = {vsn, gsn, fsn};
    VkShaderStageFlagBits stage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineShaderStageCreateInfo st[3];
    uint32_t ns = 0;
    for (int i = 0; i < 3; i++) {
        if (!code[i]) continue;
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size[i], code[i]};
        if (vkCreateShaderModule(dev, &smci, NULL, &m[i])) return 95;
        st[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, stage[i], m[i], "main", NULL};
    }
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &fmt};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription vb = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
    vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2, vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, ns, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds, pl};
    VkPipeline pipe;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    return r == VK_SUCCESS ? 0 : 100 + (-r > 50 ? 50 : -r);
}

int main(int argc, char **argv)
{
    double timeout = 30;
    const char *vsp = NULL, *gsp = NULL;
    int first = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atof(argv[++i]);
        else if (!strcmp(argv[i], "--vs") && i + 1 < argc) vsp = argv[++i];
        else if (!strcmp(argv[i], "--gs") && i + 1 < argc) gsp = argv[++i];
        else {
            first = i;
            break;
        }
    }
    size_t vsn = 0, gsn = 0;
    uint32_t *vs = vsp ? load(vsp, &vsn) : NULL, *gs = gsp ? load(gsp, &gsn) : NULL;
    if (!vs || first >= argc) {
        fprintf(stderr, "usage: %s [--timeout S] --vs vs.spv [--gs gs.spv] fs.spv...\n", argv[0]);
        return 2;
    }
    for (int i = first; i < argc; i++) {
        size_t fsn = 0;
        uint32_t *fs = load(argv[i], &fsn);
        if (!fs) {
            printf("%-40s MISSING\n", argv[i]);
            continue;
        }
        nbinds = 0;
        scan(vs, vsn);
        if (gs) scan(gs, gsn);
        scan(fs, fsn);
        fflush(stdout);
        double t0 = now();
        pid_t pid = fork();
        if (pid == 0) _exit(compile(vs, vsn, gs, gsn, fs, fsn));
        int status = 0, done = 0;
        while (now() - t0 < timeout) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                done = 1;
                break;
            }
            usleep(20000);
        }
        double ms = (now() - t0) * 1000;
        if (!done) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            printf("%-40s TIMEOUT (> %.0f s)\n", argv[i], timeout);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("%-40s OK %.0f ms\n", argv[i], ms);
        } else {
            printf("%-40s FAIL (exit %d, signal %d) after %.0f ms\n", argv[i], WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                   WIFSIGNALED(status) ? WTERMSIG(status) : 0, ms);
        }
        free(fs);
    }
    return 0;
}
