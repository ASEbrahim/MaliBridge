/* Build (NDK): aarch64-linux-android26-clang -O1 -fPIE -pie -I third_party/Vulkan-Headers-1.4.341/include tests/gauntlet.c -ldl
 * Run on the device from /data/local/tmp: ./gauntlet recipes.txt (env knobs: RENDERPASS, STATIC_VP, NOFS, MINFEAT). */
/* gauntlet <recipes.txt> [filter]
 *
 * Builds pipelines straight on the device's Vulkan driver, no bridge: one line of the recipe
 * file = one pipeline = the SPIR-V files of its stages (a single compute module is a compute
 * pipeline). Layouts come from the shaders' own decorations, the rest of the state is generic
 * (dynamic rendering, one RGBA8 attachment per fragment output, triangle list or what the
 * geometry shader wants). Each pipeline is built in a forked child, so a driver crash is a line
 * in the report, not the end of the run.
 */
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define L(n) static PFN_##n n
L(vkGetInstanceProcAddr); L(vkCreateInstance); L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceFeatures2);
L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkCreateShaderModule); L(vkCreateDescriptorSetLayout);
L(vkCreatePipelineLayout); L(vkCreateGraphicsPipelines); L(vkCreateComputePipelines); L(vkDestroyPipeline);
L(vkDestroyShaderModule); L(vkDestroyPipelineLayout); L(vkDestroyDescriptorSetLayout); L(vkDestroyDevice); L(vkDestroyInstance);
L(vkEnumerateDeviceExtensionProperties); L(vkCreateRenderPass); L(vkDestroyRenderPass);

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;

/* ---------------------------------------------------------------- SPIR-V reflection */
enum { SpvOpEntryPoint = 15, SpvOpExecutionMode = 16, SpvOpTypeInt = 21, SpvOpTypeFloat = 22, SpvOpTypeVector = 23,
       SpvOpTypeImage = 25, SpvOpTypeSampler = 26, SpvOpTypeSampledImage = 27, SpvOpTypeArray = 28,
       SpvOpTypeRuntimeArray = 29, SpvOpTypeStruct = 30, SpvOpTypePointer = 32, SpvOpConstant = 43, SpvOpVariable = 59,
       SpvOpDecorate = 71 };
enum { DecBlock = 2, DecBufferBlock = 3, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34 };
enum { SCUniformConstant = 0, SCInput = 1, SCUniform = 2, SCOutput = 3, SCPushConstant = 9, SCStorageBuffer = 12 };
enum { EMVertex = 0, EMTessControl = 1, EMTessEval = 2, EMGeometry = 3, EMFragment = 4, EMGLCompute = 5 };
enum { ModeInputPoints = 19, ModeInputLines = 20, ModeInputLinesAdjacency = 21, ModeTriangles = 22,
       ModeInputTrianglesAdjacency = 23 };

typedef struct {
    uint32_t *words;
    size_t n;
    int model;
    int gs_input; /* execution mode */
    uint32_t bound;
    /* per id */
    uint32_t *op, *type, *storage, *loc, *set, *binding, *arrlen, *vecw, *scalar;
    uint8_t *block, *has_loc, *has_set, *is_image, *is_sampler, *is_sampled;
} shader;

static int load(shader *s, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    s->words = malloc(sz);
    s->n = fread(s->words, 1, sz, f) / 4;
    fclose(f);
    if (s->n < 5 || s->words[0] != 0x07230203) return 0;
    s->bound = s->words[3];
    size_t b = s->bound;
#define A(t, f) s->f = calloc(b, sizeof(t))
    A(uint32_t, op); A(uint32_t, type); A(uint32_t, storage); A(uint32_t, loc); A(uint32_t, set); A(uint32_t, binding);
    A(uint32_t, arrlen); A(uint32_t, vecw); A(uint32_t, scalar);
    A(uint8_t, block); A(uint8_t, has_loc); A(uint8_t, has_set); A(uint8_t, is_image); A(uint8_t, is_sampler); A(uint8_t, is_sampled);
    s->model = -1;
    s->gs_input = ModeTriangles;
    for (size_t i = 5; i < s->n;) {
        uint32_t w = s->words[i], op = w & 0xffff, len = w >> 16;
        if (!len || i + len > s->n) break;
        const uint32_t *o = s->words + i;
        switch (op) {
        case SpvOpEntryPoint: if (s->model < 0) s->model = (int)o[1]; break;
        case SpvOpExecutionMode:
            if (o[2] >= ModeInputPoints && o[2] <= ModeInputTrianglesAdjacency) s->gs_input = (int)o[2];
            break;
        case SpvOpDecorate:
            if (o[1] < b) {
                if (o[2] == DecLocation) { s->loc[o[1]] = o[3]; s->has_loc[o[1]] = 1; }
                else if (o[2] == DecDescriptorSet) { s->set[o[1]] = o[3]; s->has_set[o[1]] = 1; }
                else if (o[2] == DecBinding) s->binding[o[1]] = o[3];
                else if (o[2] == DecBlock) s->block[o[1]] = 1;
                else if (o[2] == DecBufferBlock) s->block[o[1]] = 2;
            }
            break;
        case SpvOpTypeInt: s->op[o[1]] = op; s->scalar[o[1]] = o[3] ? 2 : 3; s->vecw[o[1]] = 1; break; /* 2 sint, 3 uint */
        case SpvOpTypeFloat: s->op[o[1]] = op; s->scalar[o[1]] = 1; s->vecw[o[1]] = 1; break;
        case SpvOpTypeVector: s->op[o[1]] = op; s->scalar[o[1]] = s->scalar[o[2]]; s->vecw[o[1]] = o[3]; break;
        case SpvOpTypeImage: s->op[o[1]] = op; s->is_image[o[1]] = 1; s->is_sampled[o[1]] = o[7] == 1; s->type[o[1]] = o[3]; break;
        case SpvOpTypeSampler: s->op[o[1]] = op; s->is_sampler[o[1]] = 1; break;
        case SpvOpTypeSampledImage: s->op[o[1]] = op; s->is_image[o[1]] = 1; s->is_sampled[o[1]] = 1; s->is_sampler[o[1]] = 1; s->type[o[1]] = o[2]; break;
        case SpvOpTypeArray: s->op[o[1]] = op; s->type[o[1]] = o[2]; s->arrlen[o[1]] = o[3]; break; /* arrlen = constant id */
        case SpvOpTypeRuntimeArray: s->op[o[1]] = op; s->type[o[1]] = o[2]; s->arrlen[o[1]] = 0; break;
        case SpvOpTypeStruct: s->op[o[1]] = op; break;
        case SpvOpTypePointer: s->op[o[1]] = op; s->storage[o[1]] = o[2]; s->type[o[1]] = o[3]; break;
        case SpvOpConstant: s->op[o[2]] = op; s->vecw[o[2]] = o[3]; break; /* value in vecw */
        case SpvOpVariable: s->op[o[2]] = op; s->type[o[2]] = o[1]; s->storage[o[2]] = o[3]; break;
        }
        i += len;
    }
    return 1;
}

/* Walk pointer -> (array of)* -> element; returns element type id, sets *count (0 = runtime). */
static uint32_t element(const shader *s, uint32_t ptr, uint32_t *count)
{
    uint32_t t = s->type[ptr];
    *count = 1;
    while (s->op[t] == SpvOpTypeArray || s->op[t] == SpvOpTypeRuntimeArray) {
        if (s->op[t] == SpvOpTypeRuntimeArray) *count = 0;
        else *count *= s->vecw[s->arrlen[t]] ? s->vecw[s->arrlen[t]] : 1;
        t = s->type[t];
    }
    return t;
}

typedef struct {
    uint32_t set, binding, count, stages;
    VkDescriptorType type;
} binding_rec;

static binding_rec binds[256];
static uint32_t nbinds;

static void collect_bindings(const shader *s, VkShaderStageFlags stage)
{
    for (uint32_t id = 0; id < s->bound; id++) {
        if (s->op[id] != SpvOpVariable || !s->has_set[id]) continue;
        uint32_t count;
        uint32_t t = element(s, s->type[id], &count);
        VkDescriptorType dt;
        uint32_t sc = s->storage[id];
        if (sc == SCStorageBuffer || (sc == SCUniform && s->block[t] == 2)) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (sc == SCUniform) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (s->op[t] == SpvOpTypeSampledImage) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (s->op[t] == SpvOpTypeSampler) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (s->op[t] == SpvOpTypeImage) {
            const uint32_t *img = NULL;
            /* dim == buffer (5)? find the OpTypeImage instruction again */
            for (size_t i = 5; i < s->n;) {
                uint32_t w = s->words[i];
                if ((w & 0xffff) == SpvOpTypeImage && s->words[i + 1] == t) { img = s->words + i; break; }
                i += w >> 16;
            }
            int buffer = img && img[3] == 5;
            int sampled = img && img[7] == 1;
            dt = buffer ? (sampled ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                        : (sampled ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        } else continue;
        uint32_t k;
        for (k = 0; k < nbinds; k++)
            if (binds[k].set == s->set[id] && binds[k].binding == s->binding[id]) break;
        if (k == nbinds && nbinds < 256) {
            binds[nbinds++] = (binding_rec){s->set[id], s->binding[id], count ? count : 64, stage, dt};
        } else if (k < nbinds) binds[k].stages |= stage;
    }
}

static VkShaderStageFlagBits stage_of(int model)
{
    switch (model) {
    case EMVertex: return VK_SHADER_STAGE_VERTEX_BIT;
    case EMTessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case EMTessEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case EMGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case EMFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: return VK_SHADER_STAGE_COMPUTE_BIT;
    }
}

/* ---------------------------------------------------------------- device */
static int init(void)
{
    void *lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) return 0;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define I(n) n = (PFN_##n)vkGetInstanceProcAddr(inst, #n)
    I(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gauntlet", 1, "mesa zink", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return 0;
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceFeatures2); I(vkCreateDevice); I(vkGetDeviceProcAddr);
    I(vkDestroyInstance); I(vkEnumerateDeviceExtensionProperties);
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    /* Everything the driver has, like Zink asks for. */
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    f2.features.robustBufferAccess = 0;
    int minfeat = getenv("MINFEAT") != NULL;
    if (minfeat) {
        VkPhysicalDeviceFeatures keep = {0};
        keep.geometryShader = f2.features.geometryShader;
        keep.tessellationShader = f2.features.tessellationShader;
        f2.features = keep;
        memset(&f11.storageBuffer16BitAccess, 0, sizeof(f11) - offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess));
        memset(&f12.samplerMirrorClampToEdge, 0, sizeof(f12) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
        f12.uniformBufferStandardLayout = 1; f12.vulkanMemoryModel = 1; f12.vulkanMemoryModelDeviceScope = 1; f12.scalarBlockLayout = 1;
        memset(&f13.robustImageAccess, 0, sizeof(f13) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
        f13.dynamicRendering = 1;
    }
    const char *want[] = {"VK_KHR_swapchain", "VK_EXT_transform_feedback", "VK_EXT_provoking_vertex", "VK_EXT_line_rasterization",
                          "VK_EXT_custom_border_color", "VK_EXT_index_type_uint8", "VK_EXT_primitive_topology_list_restart",
                          "VK_KHR_shader_float_controls", "VK_EXT_shader_demote_to_helper_invocation",
                          "VK_KHR_shader_terminate_invocation", "VK_EXT_depth_clamp_zero_one", "VK_EXT_image_2d_view_of_3d",
                          "VK_EXT_shader_image_atomic_int64", "VK_EXT_conservative_rasterization", "VK_EXT_sampler_filter_minmax"};
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    const char *ext[32];
    uint32_t next = 0;
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]) && !minfeat; w++)
        for (uint32_t i = 0; i < ne; i++)
            if (!strcmp(ep[i].extensionName, want[w])) { ext[next++] = want[w]; break; }
    /* Feature structs of extensions Zink enables when present (all set, as the driver reports them). */
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, NULL, 1, 1};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &xfb, 1, 1};
    VkPhysicalDeviceLineRasterizationFeaturesEXT lr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, &pv};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &lr, 1, 1};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &cb, 1};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, &u8, 1, 1};
    VkPhysicalDeviceDepthClampZeroOneFeaturesEXT dz = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT, &plr, 1};
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, &dz, 1, 1};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT ia = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, &v3, 1, 0};
    /* Query the extension features so only supported ones are enabled. */
    VkPhysicalDeviceFeatures2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &ia};
    int has[16] = {0};
    for (uint32_t i = 0; i < next; i++) {
        if (!strcmp(ext[i], "VK_EXT_transform_feedback")) has[0] = 1;
        if (!strcmp(ext[i], "VK_EXT_provoking_vertex")) has[1] = 1;
        if (!strcmp(ext[i], "VK_EXT_line_rasterization")) has[2] = 1;
        if (!strcmp(ext[i], "VK_EXT_custom_border_color")) has[3] = 1;
        if (!strcmp(ext[i], "VK_EXT_index_type_uint8")) has[4] = 1;
        if (!strcmp(ext[i], "VK_EXT_primitive_topology_list_restart")) has[5] = 1;
        if (!strcmp(ext[i], "VK_EXT_depth_clamp_zero_one")) has[6] = 1;
        if (!strcmp(ext[i], "VK_EXT_image_2d_view_of_3d")) has[7] = 1;
        if (!strcmp(ext[i], "VK_EXT_shader_image_atomic_int64")) has[8] = 1;
    }
    /* Build the chain only from present extensions. */
    VkBaseOutStructure *chain = NULL;
    VkBaseOutStructure *items[] = {(void *)&xfb, (void *)&pv, (void *)&lr, (void *)&cb, (void *)&u8, (void *)&plr, (void *)&dz, (void *)&v3, (void *)&ia};
    for (int i = 8; i >= 0; i--) {
        if (!has[i]) continue;
        items[i]->pNext = chain;
        chain = items[i];
    }
    q.pNext = chain;
    if (chain) vkGetPhysicalDeviceFeatures2(pd, &q); /* fills the supported bits */
    f13.pNext = chain;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, NULL, next, ext, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) { fprintf(stderr, "vkCreateDevice: %d\n", r); return 0; }
#define D(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines);
    D(vkCreateComputePipelines); D(vkDestroyPipeline); D(vkDestroyShaderModule); D(vkDestroyPipelineLayout);
    D(vkDestroyDescriptorSetLayout); D(vkDestroyDevice); D(vkCreateRenderPass); D(vkDestroyRenderPass);
    return 1;
}

/* ---------------------------------------------------------------- one recipe */
static VkResult build(char **files, int nfiles, char *why, size_t whysz)
{
    shader sh[8];
    VkShaderModule mods[8];
    VkPipelineShaderStageCreateInfo st[8];
    nbinds = 0;
    int vs = -1, fs = -1, gs = -1, cs = -1;
    uint32_t max_set = 0;
    for (int i = 0; i < nfiles; i++) {
        memset(&sh[i], 0, sizeof(sh[i]));
        if (!load(&sh[i], files[i])) { snprintf(why, whysz, "cannot load %s", files[i]); return VK_ERROR_UNKNOWN; }
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sh[i].n * 4, sh[i].words};
        VkResult r = vkCreateShaderModule(dev, &mi, NULL, &mods[i]);
        if (r) { snprintf(why, whysz, "vkCreateShaderModule(%s)", files[i]); return r; }
        VkShaderStageFlagBits stage = stage_of(sh[i].model);
        st[i] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, stage, mods[i], "main", NULL};
        collect_bindings(&sh[i], stage);
        if (sh[i].model == EMVertex) vs = i;
        if (sh[i].model == EMFragment) fs = i;
        if (sh[i].model == EMGeometry) gs = i;
        if (sh[i].model == EMGLCompute) cs = i;
    }
    if (getenv("NOFS") && fs >= 0) { /* drop the fragment stage */
        vkDestroyShaderModule(dev, mods[fs], NULL);
        for (int i = fs; i + 1 < nfiles; i++) { sh[i] = sh[i + 1]; mods[i] = mods[i + 1]; st[i] = st[i + 1]; }
        nfiles--; fs = -1;
        if (gs > fs) {} /* indices: recompute */
        vs = gs = -1;
        for (int i = 0; i < nfiles; i++) { if (sh[i].model == EMVertex) vs = i; if (sh[i].model == EMGeometry) gs = i; }
    }
    for (uint32_t k = 0; k < nbinds; k++) if (binds[k].set > max_set) max_set = binds[k].set;
    uint32_t nsets = nbinds ? max_set + 1 : 0;
    VkDescriptorSetLayout dsl[16] = {0};
    for (uint32_t s = 0; s < nsets; s++) {
        VkDescriptorSetLayoutBinding b[256];
        uint32_t nb = 0;
        for (uint32_t k = 0; k < nbinds; k++)
            if (binds[k].set == s) b[nb++] = (VkDescriptorSetLayoutBinding){binds[k].binding, binds[k].type, binds[k].count, binds[k].stages, NULL};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b};
        VkResult r = vkCreateDescriptorSetLayout(dev, &ci, NULL, &dsl[s]);
        if (r) { snprintf(why, whysz, "vkCreateDescriptorSetLayout(set %u)", s); return r; }
    }
    VkShaderStageFlags all = 0;
    for (int i = 0; i < nfiles; i++) all |= st[i].stage;
    VkPushConstantRange pcr = {all, 0, 128};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, nsets, dsl, 1, &pcr};
    VkPipelineLayout pl;
    VkResult r = vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    if (r) { snprintf(why, whysz, "vkCreatePipelineLayout"); return r; }
    VkPipeline pipe = VK_NULL_HANDLE;
    if (cs >= 0) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, st[cs], pl, VK_NULL_HANDLE, -1};
        r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe);
        snprintf(why, whysz, "compute");
    } else {
        if (vs < 0) { snprintf(why, whysz, "no vertex shader"); return VK_ERROR_UNKNOWN; }
        /* vertex input: one binding per attribute, vec4 of the attribute's scalar kind */
        VkVertexInputBindingDescription vb[32];
        VkVertexInputAttributeDescription va[32];
        uint32_t na = 0;
        for (uint32_t id = 0; id < sh[vs].bound && na < 32; id++) {
            if (sh[vs].op[id] != SpvOpVariable || sh[vs].storage[id] != SCInput || !sh[vs].has_loc[id]) continue;
            uint32_t cnt;
            uint32_t t = element(&sh[vs], sh[vs].type[id], &cnt);
            uint32_t kind = sh[vs].scalar[t];
            VkFormat f = kind == 2 ? VK_FORMAT_R32G32B32A32_SINT : kind == 3 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            for (uint32_t c = 0; c < (cnt ? cnt : 1) && na < 32; c++) {
                vb[na] = (VkVertexInputBindingDescription){na, 16, VK_VERTEX_INPUT_RATE_VERTEX};
                va[na] = (VkVertexInputAttributeDescription){sh[vs].loc[id] + c, na, f, 0};
                na++;
            }
        }
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, na, vb, na, va};
        VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        if (gs >= 0) switch (sh[gs].gs_input) {
            case ModeInputPoints: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
            case ModeInputLines: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case ModeInputLinesAdjacency: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; break;
            case ModeInputTrianglesAdjacency: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; break;
            default: break;
            }
        int has_tess = 0;
        for (int i = 0; i < nfiles; i++) if (sh[i].model == EMTessControl || sh[i].model == EMTessEval) has_tess = 1;
        if (has_tess) topo = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, 0};
        VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, 3};
        VkViewport vpt = {0, 0, 64, 64, 0, 1};
        VkRect2D sci = {{0, 0}, {64, 64}};
        int static_vp = getenv("STATIC_VP") != NULL;
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, static_vp ? &vpt : NULL, 1, static_vp ? &sci : NULL};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1.0f;
        rs.rasterizerDiscardEnable = fs < 0;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        /* colour attachments: one per fragment output location */
        uint32_t nout = 0;
        if (fs >= 0)
            for (uint32_t id = 0; id < sh[fs].bound; id++)
                if (sh[fs].op[id] == SpvOpVariable && sh[fs].storage[id] == SCOutput && sh[fs].has_loc[id] && sh[fs].loc[id] + 1 > nout)
                    nout = sh[fs].loc[id] + 1;
        if (nout > 8) nout = 8;
        VkFormat fmts[8];
        VkPipelineColorBlendAttachmentState cba[8];
        for (uint32_t i = 0; i < nout; i++) {
            fmts[i] = VK_FORMAT_R8G8B8A8_UNORM;
            memset(&cba[i], 0, sizeof(cba[i]));
            cba[i].colorWriteMask = 0xf;
        }
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, nout, cba, {0}};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, static_vp ? 0 : 2, dyn};
        VkPipelineRenderingCreateInfo rend = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, nout, fmts, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
        VkRenderPass rp = VK_NULL_HANDLE;
        int use_rp = getenv("RENDERPASS") != NULL;
        if (use_rp) {
            VkAttachmentDescription att[8];
            VkAttachmentReference ref[8];
            for (uint32_t i = 0; i < nout; i++) {
                att[i] = (VkAttachmentDescription){0, fmts[i], VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                ref[i] = (VkAttachmentReference){i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            }
            VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, nout, ref, NULL, NULL, 0, NULL};
            VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, nout, att, 1, &sub, 0, NULL};
            vkCreateRenderPass(dev, &rpci, NULL, &rp);
        }
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, use_rp ? NULL : (const void *)&rend, 0, (uint32_t)nfiles, st, &vi, &ia,
                                           has_tess ? &ts : NULL, &vp, &rs, &ms, &ds, &cb, &dy, pl, rp, 0, VK_NULL_HANDLE, -1};
        r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe);
        if (rp) vkDestroyRenderPass(dev, rp, NULL);
        snprintf(why, whysz, "graphics %d stage(s), %u attachment(s), %u set(s), %u attr(s)%s%s%s%s", nfiles, nout, nsets, na,
                 use_rp ? " renderpass" : " dynrender", static_vp ? " staticvp" : "", getenv("NOFS") ? " nofs" : "", getenv("MINFEAT") ? " minfeat" : "");
    }
    if (pipe) vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    for (uint32_t s = 0; s < nsets; s++) if (dsl[s]) vkDestroyDescriptorSetLayout(dev, dsl[s], NULL);
    for (int i = 0; i < nfiles; i++) vkDestroyShaderModule(dev, mods[i], NULL);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: gauntlet <recipes> [filter]\n"); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    const char *filter = argc > 2 ? argv[2] : NULL;
    char line[4096];
    int total = 0, ok = 0, failed = 0, crashed = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        if (filter && !strstr(line, filter)) continue;
        total++;
        fflush(stdout);
        int pfd[2];
        if (pipe(pfd)) return 2;
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            char *files[8];
            int n = 0;
            for (char *t = strtok(line, " \t"); t && n < 8; t = strtok(NULL, " \t")) files[n++] = t;
            char why[256] = "";
            int r = init() ? (int)build(files, n, why, sizeof(why)) : -999;
            char out[512];
            int len = snprintf(out, sizeof(out), "%d %s", r, why);
            if (write(pfd[1], out, (size_t)len) < 0) {}
            _exit(r == 0 ? 0 : 1);
        }
        close(pfd[1]);
        char out[512] = "";
        ssize_t got = read(pfd[0], out, sizeof(out) - 1);
        if (got > 0) out[got] = 0;
        close(pfd[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            crashed++;
            printf("CRASH  signal %d  %s\n", WTERMSIG(status), line);
        } else if (got <= 0 || atoi(out) != 0) {
            failed++;
            printf("FAIL   %s  %s\n", out, line);
        } else {
            ok++;
            printf("ok     %s  %s\n", out + 2, line);
        }
        fflush(stdout);
    }
    printf("== %d pipeline(s): %d ok, %d failed, %d crashed\n", total, ok, failed, crashed);
    return crashed || failed ? 1 : 0;
}
