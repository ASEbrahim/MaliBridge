/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * recycle_check: the bridge client's buffer view recycling (client/bufview.c). A view destroyed
 * and created again with the same parameters comes back without a round trip; it must still
 * work, never be out twice at once, and never outlive its buffer. New views are named by the
 * client and created without a reply (server_bvmap.c): they must read their buffer through
 * descriptor writes and update templates alike.
 *
 *   cc -O2 -Ithird_party/Vulkan-Headers-1.4.341/include -Icommon -Icommon/gen -Itests \
 *      -o host/out/recycle_check tests/recycle_check.c -ldl          (from the repository root)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* glslc -O of:
 *   layout(local_size_x = 64) in;
 *   layout(binding = 0) uniform usamplerBuffer src;
 *   layout(std430, binding = 1) writeonly buffer Out { uint v[]; } dst;
 *   void main() { uint i = gl_GlobalInvocationID.x; dst.v[i] = texelFetch(src, int(i)).x; } */
static const uint32_t spv_texel_comp[] = {
    0x07230203,0x00010000,0x000d000a,0x00000027,
    0x00000000,0x00020011,0x00000001,0x00020011,
    0x0000002e,0x0006000b,0x00000001,0x4c534c47,
    0x6474732e,0x3035342e,0x00000000,0x0003000e,
    0x00000000,0x00000001,0x0006000f,0x00000005,
    0x00000004,0x6e69616d,0x00000000,0x0000000b,
    0x00060010,0x00000004,0x00000011,0x00000040,
    0x00000001,0x00000001,0x00040047,0x0000000b,
    0x0000000b,0x0000001c,0x00040047,0x00000010,
    0x00000006,0x00000004,0x00040048,0x00000011,
    0x00000000,0x00000019,0x00050048,0x00000011,
    0x00000000,0x00000023,0x00000000,0x00030047,
    0x00000011,0x00000003,0x00040047,0x00000013,
    0x00000022,0x00000000,0x00040047,0x00000013,
    0x00000021,0x00000001,0x00040047,0x0000001a,
    0x00000022,0x00000000,0x00040047,0x0000001a,
    0x00000021,0x00000000,0x00040047,0x00000026,
    0x0000000b,0x00000019,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00040015,
    0x00000006,0x00000020,0x00000000,0x00040017,
    0x00000009,0x00000006,0x00000003,0x00040020,
    0x0000000a,0x00000001,0x00000009,0x0004003b,
    0x0000000a,0x0000000b,0x00000001,0x0004002b,
    0x00000006,0x0000000c,0x00000000,0x00040020,
    0x0000000d,0x00000001,0x00000006,0x0003001d,
    0x00000010,0x00000006,0x0003001e,0x00000011,
    0x00000010,0x00040020,0x00000012,0x00000002,
    0x00000011,0x0004003b,0x00000012,0x00000013,
    0x00000002,0x00040015,0x00000014,0x00000020,
    0x00000001,0x0004002b,0x00000014,0x00000015,
    0x00000000,0x00090019,0x00000017,0x00000006,
    0x00000005,0x00000000,0x00000000,0x00000000,
    0x00000001,0x00000000,0x0003001b,0x00000018,
    0x00000017,0x00040020,0x00000019,0x00000000,
    0x00000018,0x0004003b,0x00000019,0x0000001a,
    0x00000000,0x00040017,0x0000001f,0x00000006,
    0x00000004,0x00040020,0x00000022,0x00000002,
    0x00000006,0x0004002b,0x00000006,0x00000024,
    0x00000040,0x0004002b,0x00000006,0x00000025,
    0x00000001,0x0006002c,0x00000009,0x00000026,
    0x00000024,0x00000025,0x00000025,0x00050036,
    0x00000002,0x00000004,0x00000000,0x00000003,
    0x000200f8,0x00000005,0x00050041,0x0000000d,
    0x0000000e,0x0000000b,0x0000000c,0x0004003d,
    0x00000006,0x0000000f,0x0000000e,0x0004003d,
    0x00000018,0x0000001b,0x0000001a,0x0004007c,
    0x00000014,0x0000001d,0x0000000f,0x00040064,
    0x00000017,0x0000001e,0x0000001b,0x0005005f,
    0x0000001f,0x00000020,0x0000001e,0x0000001d,
    0x00050051,0x00000006,0x00000021,0x00000020,
    0x00000000,0x00060041,0x00000022,0x00000023,
    0x00000013,0x00000015,0x0000000f,0x0003003e,
    0x00000023,0x00000021,0x000100fd,0x00010038
};

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties mp;
static VkDeviceSize texel_align;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkQueueSubmit); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkFreeMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdPipelineBarrier); I(vkCreateShaderModule); I(vkCreateDescriptorSetLayout);
I(vkCreatePipelineLayout); I(vkCreateComputePipelines); I(vkCreateDescriptorPool); I(vkAllocateDescriptorSets);
I(vkUpdateDescriptorSets); I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets); I(vkCmdDispatch); I(vkCreateFence);
I(vkWaitForFences); I(vkDestroyFence); I(vkCreateBufferView); I(vkDestroyBufferView); I(vkFreeCommandBuffers);
I(vkFlushMappedMemoryRanges); I(vkInvalidateMappedMemoryRanges); I(vkGetBufferMemoryRequirements2);
I(vkCreateDescriptorUpdateTemplate); I(vkUpdateDescriptorSetWithTemplate);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *ptr;
    VkMemoryPropertyFlags flags;
} buffer;

#define N 4096 /* texels per dispatch */

static int make_buffer(buffer *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(dev, &ci, NULL, &b->buf)) return 0;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b->buf, &r);
    uint32_t t = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && t == UINT32_MAX; i++)
        if ((r.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) t = i;
    if (t == UINT32_MAX) return 0;
    b->flags = mp.memoryTypes[t].propertyFlags;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    if (vkAllocateMemory(dev, &ai, NULL, &b->mem) || vkBindBufferMemory(dev, b->buf, b->mem, 0)) return 0;
    return !vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->ptr);
}

static void free_buffer(buffer *b)
{
    vkDestroyBuffer(dev, b->buf, NULL);
    vkFreeMemory(dev, b->mem, NULL);
}

static void range_sync(buffer *b, int to_gpu)
{
    if (b->flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, b->mem, 0, VK_WHOLE_SIZE};
    if (to_gpu) vkFlushMappedMemoryRanges(dev, 1, &r);
    else vkInvalidateMappedMemoryRanges(dev, 1, &r);
}

/* A texel buffer source: N texels of `seed`'s pattern starting at texel_align bytes. */
static int make_source(buffer *b, uint32_t seed)
{
    VkDeviceSize size = texel_align + N * 4 + 4096;
    if (!make_buffer(b, size, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)) return 0;
    for (uint32_t i = 0; i < size / 4; i++) b->ptr[i] = 0xdead0000u ^ i;
    uint32_t *p = (uint32_t *)((char *)b->ptr + texel_align);
    for (uint32_t i = 0; i < N; i++) p[i] = i * seed + 1;
    range_sync(b, 1);
    return 1;
}

static VkBufferView make_view(VkBuffer buf, VkDeviceSize offset, VkDeviceSize range)
{
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, buf, VK_FORMAT_R32_UINT, offset, range};
    VkBufferView v = VK_NULL_HANDLE;
    if (vkCreateBufferView(dev, &ci, NULL, &v)) return VK_NULL_HANDLE;
    return v;
}

/* ------------------------------------------------------------------ the compute read-back */

static VkDescriptorSetLayout dsl;
static VkPipelineLayout pl;
static VkPipeline pipe;
static VkDescriptorPool dpool;
static VkDescriptorUpdateTemplate tmpl; /* both bindings, from a tmpl_data */
static int use_template;
static buffer out;

typedef struct {
    VkDescriptorBufferInfo out;
    uint32_t pad;
    VkBufferView view;
} tmpl_data;

static int setup_compute(void)
{
    VkDescriptorSetLayoutBinding binds[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, binds};
    if (vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl)) return 0;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    if (vkCreatePipelineLayout(dev, &plci, NULL, &pl)) return 0;
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_texel_comp), spv_texel_comp};
    VkShaderModule sm;
    if (vkCreateShaderModule(dev, &smci, NULL, &sm)) return 0;
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm,
                                                   "main", NULL};
    cpci.layout = pl;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) return 0;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 64, 2, ps};
    if (vkCreateDescriptorPool(dev, &dpci, NULL, &dpool)) return 0;
    VkDescriptorUpdateTemplateEntry te[2] = {{0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, offsetof(tmpl_data, view), 0},
                                             {1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(tmpl_data, out), 0}};
    VkDescriptorUpdateTemplateCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, NULL, 0, 2, te,
                                                VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, dsl};
    if (vkCreateDescriptorUpdateTemplate(dev, &tci, NULL, &tmpl)) return 0;
    return make_buffer(&out, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

/* Runs the shader over `view`; 1 if the output is source pattern `seed`. */
static int read_through(VkBufferView view, uint32_t seed, char *detail, size_t len)
{
    memset(out.ptr, 0, N * 4);
    range_sync(&out, 1);
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &dsl};
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(dev, &dsai, &set)) {
        snprintf(detail, len, "descriptor set");
        return 0;
    }
    VkDescriptorBufferInfo bi = {out.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &view},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL}};
    if (use_template) {
        tmpl_data td = {bi, 0, view};
        vkUpdateDescriptorSetWithTemplate(dev, set, tmpl, &td);
    } else {
        vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    }
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &cbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    int ok = !vkQueueSubmit(queue, 1, &si, f) && !vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
    vkDestroyFence(dev, f, NULL);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    if (!ok) {
        snprintf(detail, len, "submit failed");
        return 0;
    }
    range_sync(&out, 0);
    uint32_t first = N, wrong = 0;
    for (uint32_t i = 0; i < N; i++)
        if (out.ptr[i] != i * seed + 1) {
            if (first == N) first = i;
            wrong++;
        }
    if (wrong) {
        /* Rare on the host (texels from 4032 or 4048 on read as 0, twice in some 30 suite runs):
         * say whether the results arrive late (the same memory is right 50 ms on) or are wrong
         * (the view or the descriptor), so the next one tells which. */
        uint32_t got = out.ptr[first];
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
        range_sync(&out, 0);
        uint32_t later = 0;
        for (uint32_t i = 0; i < N; i++) later += out.ptr[i] != i * seed + 1;
        snprintf(detail, len, "texel %u is 0x%x, want 0x%x (%u of %u texels wrong; 50 ms later %u)", first, got, first * seed + 1, wrong, N,
                 later);
        return 0;
    }
    snprintf(detail, len, "%u texels read through the view", N);
    return 1;
}

/* ------------------------------------------------------------------ checks */

static void test_reuse(void)
{
    char d[200];
    buffer a;
    if (!make_source(&a, 3)) {
        check(0, "source buffer", "cannot create/map");
        return;
    }
    VkBufferView first = make_view(a.buf, texel_align, N * 4);
    vkDestroyBufferView(dev, first, NULL);
    int same = 0, rounds = 2000;
    double t0 = now_ms();
    for (int i = 0; i < rounds; i++) {
        VkBufferView v = make_view(a.buf, texel_align, N * 4);
        same += v == first;
        vkDestroyBufferView(dev, v, NULL);
    }
    double us = (now_ms() - t0) * 1000.0 / rounds;
    snprintf(d, sizeof(d), "%d of %d creates got the parked view back (%.2f us per create+destroy)", same, rounds, us);
    check(same == rounds, "identical view recycled", d);

    /* Two identical views at once must be two views; the parked one goes to the first. */
    VkBufferView v1 = make_view(a.buf, texel_align, N * 4), v2 = make_view(a.buf, texel_align, N * 4);
    snprintf(d, sizeof(d), "%#llx, %#llx", (unsigned long long)v1, (unsigned long long)v2);
    check(v1 == first && v2 && v2 != v1, "a view is never out twice", d);
    /* Another range is another key. */
    VkBufferView v3 = make_view(a.buf, texel_align, N * 2);
    check(v3 && v3 != v1 && v3 != v2, "different range, different view", NULL);
    vkDestroyBufferView(dev, v3, NULL);

    check(read_through(v1, 3, d, sizeof(d)), "recycled view reads its buffer", d);
    check(read_through(v2, 3, d, sizeof(d)), "second view reads its buffer", d);
    vkDestroyBufferView(dev, v2, NULL);
    vkDestroyBufferView(dev, v1, NULL);

    /* The buffer goes with views parked: a new buffer (often the same handle again) must get a
     * view of itself, not a stale one of the old buffer. The old memory stays until after the
     * test, so new buffers live elsewhere and a stale view would read the old pattern. */
    VkBuffer old = a.buf;
    vkDestroyBuffer(dev, a.buf, NULL);
    buffer b;
    if (!make_source(&b, 7)) {
        check(0, "second source buffer", "cannot create/map");
        return;
    }
    VkBufferView vb = make_view(b.buf, texel_align, N * 4);
    int ok = read_through(vb, 7, d, sizeof(d));
    char d2[260];
    snprintf(d2, sizeof(d2), "%s (buffer handle %s)", d, b.buf == old ? "reused" : "new");
    check(ok, "no stale view after vkDestroyBuffer", d2);

    /* A view still out when its buffer is destroyed, destroyed after it: not parked. */
    old = b.buf;
    vkDestroyBuffer(dev, b.buf, NULL);
    vkDestroyBufferView(dev, vb, NULL);
    buffer c;
    if (!make_source(&c, 11)) {
        check(0, "third source buffer", "cannot create/map");
        return;
    }
    VkBufferView vc = make_view(c.buf, texel_align, N * 4);
    ok = read_through(vc, 11, d, sizeof(d));
    snprintf(d2, sizeof(d2), "%s (buffer handle %s)", d, c.buf == old ? "reused" : "new");
    check(ok, "view destroyed after its buffer is not reused", d2);
    vkDestroyBufferView(dev, vc, NULL);
    free_buffer(&c);
    vkFreeMemory(dev, b.mem, NULL);
    vkFreeMemory(dev, a.mem, NULL);
}

/* More distinct views than the cache holds: the oldest are destroyed for real, the rest work. */
static void test_eviction(void)
{
    char d[200];
    buffer a;
    if (!make_source(&a, 5)) {
        check(0, "eviction buffer", "cannot create/map");
        return;
    }
    enum { M = 6000 };
    static VkBufferView v[M];
    int made = 0;
    for (int i = 0; i < M; i++) {
        v[i] = make_view(a.buf, i & 1 ? 0 : texel_align, N * 4 - (VkDeviceSize)(i / 2) * 4);
        made += v[i] != VK_NULL_HANDLE;
    }
    for (int i = 0; i < M; i++) vkDestroyBufferView(dev, v[i], NULL);
    int back = 0;
    for (int i = 0; i < M; i++) {
        VkBufferView w = make_view(a.buf, i & 1 ? 0 : texel_align, N * 4 - (VkDeviceSize)(i / 2) * 4);
        back += w == v[i];
        v[i] = w;
    }
    int ok = read_through(v[0], 5, d, sizeof(d));
    for (int i = 0; i < M; i++) vkDestroyBufferView(dev, v[i], NULL);
    char d2[300];
    snprintf(d2, sizeof(d2), "%d views made, %d came back from the cache; %s", made, back, d);
    check(made == M && back > 0 && back < M && ok, "bounded cache", d2);
    free_buffer(&a);
}

/* Distinct views made back to back (named by the client, no reply awaited), read through
 * descriptor writes and through an update template, then destroyed. */
static void test_named(void)
{
    char d[200];
    buffer a;
    if (!make_source(&a, 13)) {
        check(0, "named views buffer", "cannot create/map");
        return;
    }
    enum { M = 300 };
    static VkBufferView v[M];
    int made = 0, dup = 0;
    double t0 = now_ms();
    for (int i = 0; i < M; i++) {
        v[i] = make_view(a.buf, texel_align, N * 4 + (VkDeviceSize)(M - 1 - i) * 4);
        made += v[i] != VK_NULL_HANDLE;
        for (int j = 0; j < i; j++) dup += v[j] == v[i];
    }
    snprintf(d, sizeof(d), "%d of %d made, %d duplicates (%.2f us per create)", made, M, dup, (now_ms() - t0) * 1000.0 / M);
    check(made == M && !dup, "distinct new views", d);
    check(read_through(v[M - 1], 13, d, sizeof(d)), "newest view through vkUpdateDescriptorSets", d);
    use_template = 1;
    check(read_through(v[M / 2], 13, d, sizeof(d)), "a view through vkUpdateDescriptorSetWithTemplate", d);
    VkBufferView w = make_view(a.buf, texel_align, N * 4 + 4 * M);
    check(read_through(w, 13, d, sizeof(d)), "a view just made through a template", d);
    use_template = 0;
    vkDestroyBufferView(dev, w, NULL);
    for (int i = 0; i < M; i++) vkDestroyBufferView(dev, v[i], NULL);
    VkBufferView x = make_view(a.buf, texel_align, N * 4 + 8);
    check(read_through(x, 13, d, sizeof(d)), "view made after the others were destroyed", d);
    vkDestroyBufferView(dev, x, NULL);
    free_buffer(&a);
}

/* Threads creating and destroying the same view: no handle is ever held by two at once. */
static VkBuffer mt_buf;
static pthread_mutex_t mt_lock = PTHREAD_MUTEX_INITIALIZER;
static VkBufferView mt_held[64];
static int mt_dup, mt_fail;

static void *mt_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 3000; i++) {
        VkBufferView v = make_view(mt_buf, texel_align, N * 4);
        pthread_mutex_lock(&mt_lock);
        if (!v) mt_fail++;
        for (int t = 0; t < 64; t++)
            if (v && mt_held[t] == v) mt_dup++;
        mt_held[id] = v;
        pthread_mutex_unlock(&mt_lock);
        pthread_mutex_lock(&mt_lock);
        mt_held[id] = VK_NULL_HANDLE;
        pthread_mutex_unlock(&mt_lock);
        vkDestroyBufferView(dev, v, NULL);
    }
    return NULL;
}

static void test_threads(void)
{
    buffer a;
    if (!make_source(&a, 9)) {
        check(0, "thread buffer", "cannot create/map");
        return;
    }
    mt_buf = a.buf;
    pthread_t th[6];
    for (int i = 0; i < 6; i++) pthread_create(&th[i], NULL, mt_worker, (void *)(intptr_t)i);
    for (int i = 0; i < 6; i++) pthread_join(th[i], NULL);
    char d[120];
    snprintf(d, sizeof(d), "6 threads x 3000: %d failed creates, %d handles out twice", mt_fail, mt_dup);
    check(!mt_fail && !mt_dup, "threads", d);
    VkBufferView v = make_view(a.buf, texel_align, N * 4);
    check(read_through(v, 9, d, sizeof(d)), "view after the threads", d);
    vkDestroyBufferView(dev, v, NULL);
    free_buffer(&a);
}

/* Buffers created alike get their requirements from the client's cache (memory.c): the answers
 * must be what the first buffer got, dedicated-allocation info included, and a buffer created
 * otherwise must not share them. */
static void test_requirements(void)
{
    char d[200];
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536 + 12,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer first;
    if (vkCreateBuffer(dev, &ci, NULL, &first)) {
        check(0, "requirements", "vkCreateBuffer");
        return;
    }
    VkMemoryDedicatedRequirements ded0 = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 r0 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded0};
    VkBufferMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, NULL, first};
    vkGetBufferMemoryRequirements2(dev, &ri, &r0);
    int same = 0, rounds = 500;
    double t0 = now_ms();
    for (int i = 0; i < rounds; i++) {
        VkBuffer b;
        vkCreateBuffer(dev, &ci, NULL, &b);
        VkMemoryDedicatedRequirements ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 r = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded};
        ri.buffer = b;
        vkGetBufferMemoryRequirements2(dev, &ri, &r);
        VkMemoryRequirements r1;
        vkGetBufferMemoryRequirements(dev, b, &r1);
        same += !memcmp(&r.memoryRequirements, &r0.memoryRequirements, sizeof(r1)) && !memcmp(&r1, &r0.memoryRequirements, sizeof(r1)) &&
                ded.prefersDedicatedAllocation == ded0.prefersDedicatedAllocation &&
                ded.requiresDedicatedAllocation == ded0.requiresDedicatedAllocation && r.pNext == &ded;
        vkDestroyBuffer(dev, b, NULL);
    }
    snprintf(d, sizeof(d), "%d of %d alike buffers (size %llu, types 0x%x; %.1f us per create+2 queries+destroy)", same, rounds,
             (unsigned long long)r0.memoryRequirements.size, r0.memoryRequirements.memoryTypeBits, (now_ms() - t0) * 1000.0 / rounds);
    check(same == rounds, "requirements of alike buffers", d);
    ci.size = 1 << 20;
    VkBuffer big;
    vkCreateBuffer(dev, &ci, NULL, &big);
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(dev, big, &rb);
    snprintf(d, sizeof(d), "size %llu", (unsigned long long)rb.size);
    check(rb.size >= ci.size, "requirements of another size", d);
    vkDestroyBuffer(dev, big, NULL);
    vkDestroyBuffer(dev, first, NULL);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-recycle", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        check(0, "instance", NULL);
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        check(0, "physical device", "none");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    texel_align = props.limits.minTexelBufferOffsetAlignment < 256 ? 256 : props.limits.minTexelBufferOffsetAlignment;
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        check(0, "device", NULL);
        return 1;
    }
    check(1, "device", props.deviceName);
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory);
    G(vkFreeMemory); G(vkBindBufferMemory); G(vkMapMemory); G(vkCmdPipelineBarrier); G(vkCreateShaderModule);
    G(vkCreateDescriptorSetLayout); G(vkCreatePipelineLayout); G(vkCreateComputePipelines); G(vkCreateDescriptorPool);
    G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets); G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdDispatch);
    G(vkCreateFence); G(vkWaitForFences); G(vkDestroyFence); G(vkCreateBufferView); G(vkDestroyBufferView);
    G(vkFreeCommandBuffers); G(vkDestroyDevice); G(vkFlushMappedMemoryRanges); G(vkInvalidateMappedMemoryRanges);
    G(vkGetBufferMemoryRequirements2); G(vkCreateDescriptorUpdateTemplate); G(vkUpdateDescriptorSetWithTemplate);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    if (!setup_compute()) {
        check(0, "compute setup", NULL);
        return 1;
    }

    test_reuse();
    test_eviction();
    test_named();
    test_threads();
    test_requirements();

    /* Views parked at this point go with the device. */
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
