/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * ycbcr_compute_check: compute pipelines whose layout holds an immutable YCbCr-conversion sampler,
 * as gamescope's composite shaders have (16 of them in its compositing layout). Mali and Samsung's
 * Xclipse 960 do not build such pipelines (Xclipse: VK_ERROR_UNKNOWN for 135 of gamescope's 144,
 * every game black with sound, MaliBridge#20); the server puts a plain sampler in their place in
 * compute bindings (server_spvfix.c, VKBRIDGE_YCBCR_SWAP). VKBRIDGE_FAKE_MISSING=ycbcrcompute
 * imitates the refusal on the host.
 *
 * Checks, in order:
 *   1  an NV12 (G8_B8R8_2PLANE_420) conversion and a sampler with it;
 *   2  a set layout with that sampler immutable in a compute binding, and a pipeline layout;
 *   3  a compute pipeline with that layout: built, or with --expect-fail refused with
 *      VK_ERROR_UNKNOWN (the refusal imitated, the swap off);
 *   4  the same shader with a plain sampler in the binding: built (the imitation only refuses
 *      layouts with YCbCr samplers);
 *   5  the YCbCr layouts destroyed, a plain one made after them: its pipeline built (a reused
 *      handle is not taken for the old layout).
 *
 *   ycbcr_compute_check [--expect-fail]
 * Last line "OK: ..." or "FAILED: ..."; exit 0 only if all pass (77: the device has no YCbCr
 * conversion for NV12).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t comp_spv[] = {
#include "ycbcr_comp.inc"
};

#define FMT VK_FORMAT_G8_B8R8_2PLANE_420_UNORM

static int failures, checks;

static void check(int ok, const char *what, VkResult r)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%d)\n", ok ? "PASS" : "FAIL", what, r);
    fflush(stdout);
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static PFN_vkGetDeviceProcAddr gdpa;
#define I(name) ((PFN_##name)gipa(inst, #name))
#define D(name) ((PFN_##name)gdpa(dev, #name))

/* A set layout (binding 0: the combined image sampler, immutable; binding 1: a storage buffer)
 * and a pipeline layout with it. */
static VkResult layouts(VkSampler s, VkDescriptorSetLayout *dsl, VkPipelineLayout *pl)
{
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &s},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkResult r = D(vkCreateDescriptorSetLayout)(dev, &dci, NULL, dsl);
    if (r != VK_SUCCESS) return r;
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, dsl, 0, NULL};
    return D(vkCreatePipelineLayout)(dev, &pci, NULL, pl);
}

static VkResult pipeline(VkShaderModule m, VkPipelineLayout pl, VkPipeline *p)
{
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL},
                                      pl, VK_NULL_HANDLE, -1};
    *p = VK_NULL_HANDLE;
    return D(vkCreateComputePipelines)(dev, VK_NULL_HANDLE, 1, &ci, NULL, p);
}

int main(int argc, char **argv)
{
    int expect_fail = argc > 1 && !strcmp(argv[1], "--expect-fail");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: no Vulkan loader\n");
        return 1;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ycbcr_compute_check", 1, "gamescope", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAILED: no instance\n");
        return 1;
    }
    uint32_t n = 1;
    I(vkEnumeratePhysicalDevices)(inst, &n, &pd);
    if (!n) {
        printf("FAILED: no device\n");
        return 1;
    }
    VkFormatProperties fp;
    I(vkGetPhysicalDeviceFormatProperties)(pd, FMT, &fp);
    if (!(fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT))) {
        printf("SKIP: the device has no YCbCr conversion for NV12 (features 0x%x)\n", fp.optimalTilingFeatures);
        return 77;
    }
    VkChromaLocation loc = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT ? VK_CHROMA_LOCATION_MIDPOINT
                                                                                                     : VK_CHROMA_LOCATION_COSITED_EVEN;
    uint32_t nq = 8, qf = 0;
    VkQueueFamilyProperties qp[8];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            qf = i;
            break;
        }
    float prio = 1.0f;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.samplerYcbcrConversion = VK_TRUE;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f11, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkResult r = I(vkCreateDevice)(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) {
        printf("FAILED: vkCreateDevice %d\n", r);
        return 1;
    }
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

    /* 1: gamescope's NV12 conversion (BT.709, narrow range, linear chroma) and its sampler */
    VkSamplerYcbcrConversionCreateInfo cci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, NULL, FMT,
                                              VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
                                              {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY},
                                              loc, loc, VK_FILTER_LINEAR, VK_FALSE};
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)) cci.chromaFilter = VK_FILTER_NEAREST;
    VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
    r = D(vkCreateSamplerYcbcrConversion)(dev, &cci, NULL, &conv);
    check(r == VK_SUCCESS, "NV12 YCbCr conversion", r);
    VkSamplerYcbcrConversionInfo cinfo = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, conv};
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &cinfo, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                               VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (cci.chromaFilter == VK_FILTER_NEAREST) sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VkSampler ys = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    if (r == VK_SUCCESS) r = D(vkCreateSampler)(dev, &sci, NULL, &ys);
    check(r == VK_SUCCESS, "sampler with the conversion", r);
    sci.pNext = NULL;
    VkResult rp = D(vkCreateSampler)(dev, &sci, NULL, &ps);
    if (r != VK_SUCCESS || rp != VK_SUCCESS) goto out;

    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(comp_spv), comp_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    r = D(vkCreateShaderModule)(dev, &mci, NULL, &m);
    check(r == VK_SUCCESS, "compute module", r);
    if (r != VK_SUCCESS) goto out;

    /* 2, 3 */
    VkDescriptorSetLayout ydsl = VK_NULL_HANDLE, pdsl = VK_NULL_HANDLE, adsl = VK_NULL_HANDLE;
    VkPipelineLayout ypl = VK_NULL_HANDLE, ppl = VK_NULL_HANDLE, apl = VK_NULL_HANDLE;
    VkPipeline yp, pp, ap;
    r = layouts(ys, &ydsl, &ypl);
    check(r == VK_SUCCESS, "set and pipeline layout, the YCbCr sampler immutable in a compute binding", r);
    if (r != VK_SUCCESS) goto out;
    r = pipeline(m, ypl, &yp);
    if (expect_fail)
        check(r == VK_ERROR_UNKNOWN && !yp, "compute pipeline with the YCbCr layout: refused (imitated, swap off)", r);
    else
        check(r == VK_SUCCESS && yp, "compute pipeline with the YCbCr layout: built", r);
    if (yp) D(vkDestroyPipeline)(dev, yp, NULL);

    /* 4 */
    r = layouts(ps, &pdsl, &ppl);
    if (r == VK_SUCCESS) r = pipeline(m, ppl, &pp);
    check(r == VK_SUCCESS && pp, "compute pipeline with a plain sampler in the binding: built", r);
    if (r == VK_SUCCESS) D(vkDestroyPipeline)(dev, pp, NULL);

    /* 5 */
    D(vkDestroyPipelineLayout)(dev, ypl, NULL);
    D(vkDestroyDescriptorSetLayout)(dev, ydsl, NULL);
    r = layouts(ps, &adsl, &apl);
    if (r == VK_SUCCESS) r = pipeline(m, apl, &ap);
    check(r == VK_SUCCESS && ap, "after the YCbCr layouts are destroyed, a plain layout's pipeline: built", r);
    if (r == VK_SUCCESS) D(vkDestroyPipeline)(dev, ap, NULL);
    D(vkDestroyPipelineLayout)(dev, apl, NULL);
    D(vkDestroyDescriptorSetLayout)(dev, adsl, NULL);
    D(vkDestroyPipelineLayout)(dev, ppl, NULL);
    D(vkDestroyDescriptorSetLayout)(dev, pdsl, NULL);
    D(vkDestroyShaderModule)(dev, m, NULL);
out:
    if (ys) D(vkDestroySampler)(dev, ys, NULL);
    if (ps) D(vkDestroySampler)(dev, ps, NULL);
    if (conv) D(vkDestroySamplerYcbcrConversion)(dev, conv, NULL);
    D(vkDestroyDevice)(dev, NULL);
    I(vkDestroyInstance)(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks%s\n", checks, expect_fail ? " (refusal imitated, swap off)" : "");
    return failures ? 1 : 0;
}
