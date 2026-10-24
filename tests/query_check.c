/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * query_check: occlusion and timestamp query results through the bridge (client/query.c answers
 * them from a mirror). Occlusion queries count the samples of a full-screen triangle cut by a
 * scissor, so every result has one exact value; timestamps are compared with the app's own
 * vkCmdCopyQueryPoolResults of the same queries. Round trips of vkGetQueryPoolResults are counted
 * from the client's VKBRIDGE_TRACE output (stderr goes to a temporary file for that), and must be
 * none where the mirror should answer - unless VKBRIDGE_QUERY_MIRROR=0, which runs the same checks
 * on the wire.
 *
 *   query_check [--gpu N]
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smoke_spv.h"

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool cpool;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks, mirror = 1, precise;
static FILE *trace;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures);
I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer);
I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdPipelineBarrier); I(vkCreateShaderModule); I(vkCreatePipelineLayout);
I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass);
I(vkCreateFramebuffer); I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdDraw);
I(vkCmdBindPipeline); I(vkCmdPushConstants); I(vkCmdSetViewport); I(vkCmdSetScissor); I(vkCreateFence); I(vkWaitForFences);
I(vkResetFences); I(vkDestroyDevice); I(vkCreateQueryPool); I(vkDestroyQueryPool); I(vkResetQueryPool); I(vkCmdResetQueryPool);
I(vkCmdBeginQuery); I(vkCmdEndQuery); I(vkCmdWriteTimestamp); I(vkCmdWriteTimestamp2); I(vkGetQueryPoolResults);
I(vkCmdCopyQueryPoolResults); I(vkCmdExecuteCommands); I(vkResetCommandBuffer); I(vkDeviceWaitIdle);
I(vkInvalidateMappedMemoryRanges);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

/* vkGetQueryPoolResults round trips so far (client trace lines). */
static long wire_polls(void)
{
    if (!trace) return 0;
    fflush(stderr);
    rewind(trace);
    char line[512];
    long n = 0;
    while (fgets(line, sizeof(line), trace))
        if (strstr(line, "trace: vkGetQueryPoolResults")) n++;
    fseek(trace, 0, SEEK_END);
    return n;
}

/* "no round trips" holds only with the mirror on. */
static int no_wire(long before)
{
    return !mirror || wire_polls() == before;
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ drawing setup */

#define W 64
#define H 64
static VkRenderPass rp;
static VkFramebuffer fb;
static VkPipeline gpipe;
static VkPipelineLayout pl;

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

static int setup_draw(void)
{
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    uint32_t t = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    VkDeviceMemory mem;
    if (vkAllocateMemory(dev, &ai, NULL, &mem) || vkBindImageMemory(dev, img, mem, 0)) return 0;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    if (vkCreateImageView(dev, &vci, NULL, &view)) return 0;
    VkAttachmentDescription att = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 0, NULL};
    if (vkCreateRenderPass(dev, &rpci, NULL, &rp)) return 0;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1};
    if (vkCreateFramebuffer(dev, &fci, NULL, &fb)) return 0;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    if (vkCreatePipelineLayout(dev, &plci, NULL, &pl)) return 0;
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(spv_smoke_vert, sizeof(spv_smoke_vert)), "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(spv_smoke_frag, sizeof(spv_smoke_frag)), "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds,
                                         pl, rp, 0, VK_NULL_HANDLE, -1};
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &gpipe) == VK_SUCCESS;
}

static VkCommandBuffer alloc_cb(VkCommandBufferLevel level)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, level, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    return cb;
}

static VkCommandBuffer begin(void)
{
    VkCommandBuffer cb = alloc_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void begin_pass(VkCommandBuffer cb, VkSubpassContents contents)
{
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 1}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, contents);
}

/* Inside a render pass: draws w x h samples (exactly) into query q of pool. */
static void counted_draw(VkCommandBuffer cb, VkQueryPool pool, uint32_t q, uint32_t w, uint32_t h)
{
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
    VkViewport v = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color);
    if (pool) vkCmdBeginQuery(cb, pool, q, precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (pool) vkCmdEndQuery(cb, pool, q);
}

static VkFence fence;

static int submit_nowait(VkCommandBuffer cb)
{
    if (vkEndCommandBuffer(cb)) return 0;
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    return vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS;
}

static int submit(VkCommandBuffer cb)
{
    return submit_nowait(cb) && vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
}

static VkQueryPool make_pool(VkQueryType type, uint32_t n)
{
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, type, n, 0};
    VkQueryPool p = VK_NULL_HANDLE;
    vkCreateQueryPool(dev, &ci, NULL, &p);
    return p;
}

static int samples_ok(uint64_t got, uint64_t want)
{
    return precise ? got == want : (got != 0) == (want != 0);
}

/* ------------------------------------------------------------------ tests */

static void test_occlusion(void)
{
    char d[256];
    VkQueryPool pool = make_pool(VK_QUERY_TYPE_OCCLUSION, 64);
    vkResetQueryPool(dev, pool, 0, 64);
    VkCommandBuffer cb = begin();
    begin_pass(cb, VK_SUBPASS_CONTENTS_INLINE);
    /* 0..7 consecutive; 20, 22, 25 apart (separate copies) */
    uint32_t qs[11] = {0, 1, 2, 3, 4, 5, 6, 7, 20, 22, 25};
    uint64_t want[11];
    for (int i = 0; i < 11; i++) {
        uint32_t w = 8 * (uint32_t)(i % 8 + 1), h = 4 * (uint32_t)(i + 1);
        want[i] = (uint64_t)w * h;
        counted_draw(cb, pool, qs[i], w, h);
    }
    vkCmdEndRenderPass(cb);
    int ok = submit(cb);
    long w0 = wire_polls();
    int bad = -1;
    uint64_t r64[8];
    VkResult r = vkGetQueryPoolResults(dev, pool, 0, 8, sizeof(r64), r64, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    for (int i = 0; i < 8; i++)
        if (!samples_ok(r64[i], want[i]) && bad < 0) bad = i;
    for (int i = 8; i < 11; i++) {
        uint64_t v = 0;
        VkResult ri = vkGetQueryPoolResults(dev, pool, qs[i], 1, 8, &v, 8, VK_QUERY_RESULT_64_BIT);
        if ((ri != VK_SUCCESS || !samples_ok(v, want[i])) && bad < 0) bad = i;
    }
    if (bad >= 0) snprintf(d, sizeof(d), "query index %d wrong (want %llu)", bad, (unsigned long long)want[bad]);
    else snprintf(d, sizeof(d), "11 queries, exact sample counts%s", precise ? "" : " (non-zero only: no precise queries)");
    check(ok && r == VK_SUCCESS && bad < 0 && no_wire(w0), "occlusion queries (64-bit, WAIT and plain)", d);

    /* 32-bit with availability, stride 12. The padding is checked with the mirror only: the wire
     * stub copies back all of dataSize, gaps included. */
    w0 = wire_polls();
    uint32_t r32[8 * 3];
    memset(r32, 0xAB, sizeof(r32));
    r = vkGetQueryPoolResults(dev, pool, 0, 8, sizeof(r32), r32, 12, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    bad = -1;
    for (int i = 0; i < 8; i++)
        if (!samples_ok(r32[i * 3], want[i]) || r32[i * 3 + 1] != 1 || (mirror && r32[i * 3 + 2] != 0xABABABABu)) bad = bad < 0 ? i : bad;
    snprintf(d, sizeof(d), bad < 0 ? "values, availability 1, padding untouched" : "query %d: %u avail %u pad 0x%x", bad,
             bad < 0 ? 0 : r32[bad * 3], bad < 0 ? 0 : r32[bad * 3 + 1], bad < 0 ? 0 : r32[bad * 3 + 2]);
    check(r == VK_SUCCESS && bad < 0 && no_wire(w0), "32-bit results with availability, stride 12", d);

    /* reset from the host: not ready, nothing written without flags; availability 0; partial 0 */
    vkResetQueryPool(dev, pool, 0, 8);
    w0 = wire_polls();
    uint64_t s[2] = {0x1111, 0x2222};
    VkResult r1 = vkGetQueryPoolResults(dev, pool, 3, 1, 8, s, 8, VK_QUERY_RESULT_64_BIT);
    VkResult r2 = vkGetQueryPoolResults(dev, pool, 3, 1, 16, s, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    int avail_zero = s[1] == 0;
    uint64_t p[2] = {0x3333, 0x4444};
    VkResult r3 = vkGetQueryPoolResults(dev, pool, 3, 1, 16, p, 16,
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
    /* query 20 was not reset: still available */
    uint64_t mix[2] = {0, 0};
    VkResult r4 = vkGetQueryPoolResults(dev, pool, 20, 1, 8, mix, 8, VK_QUERY_RESULT_64_BIT);
    snprintf(d, sizeof(d), "plain %d, availability %d (avail %llu), partial %d (value %llu avail %llu), other query %d", r1, r2,
             (unsigned long long)s[1], r3, (unsigned long long)p[0], (unsigned long long)p[1], r4);
    /* After a host reset the query is no longer mirrored until reused: these go to the wire. */
    check(r1 == VK_NOT_READY && r2 == VK_NOT_READY && avail_zero && (r3 == VK_NOT_READY || r3 == VK_SUCCESS) && p[1] == 0 && p[0] <= want[3] &&
              r4 == VK_SUCCESS && samples_ok(mix[0], want[8]),
          "host reset: not ready, availability 0, partial", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

/* Polling without WAIT until the GPU finishes, after a host reset, many times. */
static void test_host_reset_reuse(void)
{
    char d[256];
    VkQueryPool pool = make_pool(VK_QUERY_TYPE_OCCLUSION, 4);
    long w0 = wire_polls();
    int bad = -1, iters = 200;
    long not_ready = 0, polls = 0;
    for (int it = 0; it < iters && bad < 0; it++) {
        vkResetQueryPool(dev, pool, 0, 4);
        VkCommandBuffer cb = begin();
        begin_pass(cb, VK_SUBPASS_CONTENTS_INLINE);
        uint64_t want[4];
        for (uint32_t q = 0; q < 4; q++) {
            uint32_t w = 1 + (uint32_t)((it * 7 + q * 13) % W), h = 1 + (uint32_t)((it * 3 + q * 5) % H);
            want[q] = (uint64_t)w * h;
            counted_draw(cb, pool, q, w, h);
        }
        vkCmdEndRenderPass(cb);
        if (!submit_nowait(cb)) {
            bad = it;
            break;
        }
        uint64_t v[4];
        VkResult r;
        for (;;) {
            polls++;
            r = vkGetQueryPoolResults(dev, pool, 0, 4, sizeof(v), v, 8, VK_QUERY_RESULT_64_BIT);
            if (r != VK_NOT_READY) break;
            not_ready++;
            if (not_ready > 50000000) break;
        }
        for (uint32_t q = 0; q < 4; q++)
            if (r != VK_SUCCESS || !samples_ok(v[q], want[q])) bad = it;
        vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull);
    }
    long wire = wire_polls() - w0;
    snprintf(d, sizeof(d), "%d rounds, %ld polls (%ld not ready), %ld round trips%s", iters, polls, not_ready, wire,
             bad < 0 ? "" : " - wrong result");
    check(bad < 0 && no_wire(w0), "host reset + reuse, polled without WAIT", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

/* vkCmdResetQueryPool: the mirror forgets the reset queries when the reset executes. */
static void test_gpu_reset(void)
{
    char d[256];
    VkQueryPool pool = make_pool(VK_QUERY_TYPE_OCCLUSION, 8);
    int bad = -1;
    long w0 = wire_polls();
    for (int it = 0; it < 20 && bad < 0; it++) {
        VkCommandBuffer cb = begin();
        vkCmdResetQueryPool(cb, pool, 0, 8);
        begin_pass(cb, VK_SUBPASS_CONTENTS_INLINE);
        uint64_t want[6];
        for (uint32_t q = 0; q < 6; q++) {
            uint32_t w = 2 + (uint32_t)((it * 5 + q * 11) % (W - 2)), h = 3 + (uint32_t)((it + q * 7) % (H - 3));
            want[q] = (uint64_t)w * h;
            counted_draw(cb, pool, q, w, h);
        }
        vkCmdEndRenderPass(cb);
        if (!submit(cb)) bad = 100 + it;
        uint64_t v[6];
        VkResult r = vkGetQueryPoolResults(dev, pool, 0, 6, sizeof(v), v, 8, VK_QUERY_RESULT_64_BIT);
        for (uint32_t q = 0; q < 6; q++)
            if (r != VK_SUCCESS || !samples_ok(v[q], want[q])) bad = it;
    }
    snprintf(d, sizeof(d), bad < 0 ? "20 rounds of reset + 6 queries in one command buffer" : "round %d wrong", bad);
    check(bad < 0 && no_wire(w0), "GPU reset (vkCmdResetQueryPool) + reuse", d);

    /* Query 0 reset by a later command buffer: unavailable once that runs, from the mirror. */
    w0 = wire_polls();
    VkCommandBuffer cb = begin();
    vkCmdResetQueryPool(cb, pool, 0, 2);
    int ok = submit(cb);
    uint64_t v[4] = {0, 7, 0, 7};
    VkResult r = vkGetQueryPoolResults(dev, pool, 0, 2, sizeof(v), v, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    snprintf(d, sizeof(d), "result %d, availability %llu %llu", r, (unsigned long long)v[1], (unsigned long long)v[3]);
    check(ok && r == VK_NOT_READY && v[1] == 0 && v[3] == 0 && no_wire(w0), "GPU reset alone makes queries unavailable", d);

    /* Ended and then reset again in the same command buffer: no copy may wait for it (a hang). */
    cb = begin();
    vkCmdResetQueryPool(cb, pool, 0, 8);
    begin_pass(cb, VK_SUBPASS_CONTENTS_INLINE);
    counted_draw(cb, pool, 2, 10, 10);
    counted_draw(cb, pool, 3, 10, 5);
    vkCmdEndRenderPass(cb);
    vkCmdResetQueryPool(cb, pool, 2, 1);
    ok = submit(cb);
    uint64_t a[4];
    r = vkGetQueryPoolResults(dev, pool, 2, 2, sizeof(a), a, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    snprintf(d, sizeof(d), "result %d, query 2 avail %llu, query 3 = %llu avail %llu", r, (unsigned long long)a[1],
             (unsigned long long)a[2], (unsigned long long)a[3]);
    check(ok && r == VK_NOT_READY && a[1] == 0 && a[3] == 1 && samples_ok(a[2], 50), "query ended then reset in one command buffer", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

static void test_timestamps(uint32_t valid_bits, int have_ts2)
{
    char d[256];
    if (!valid_bits) {
        check(1, "timestamps", "skipped (queue family has no timestamps)");
        return;
    }
    VkQueryPool pool = make_pool(VK_QUERY_TYPE_TIMESTAMP, 8);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    uint32_t t = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
    VkDeviceMemory mem;
    uint64_t *ptr = NULL;
    if (t == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0) ||
        vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&ptr)) {
        check(0, "timestamps", "buffer setup");
        return;
    }
    int bad = -1;
    long w0 = wire_polls();
    for (int it = 0; it < 50 && bad < 0; it++) {
        vkResetQueryPool(dev, pool, 0, 8);
        VkCommandBuffer cb = begin();
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
        begin_pass(cb, VK_SUBPASS_CONTENTS_INLINE);
        counted_draw(cb, VK_NULL_HANDLE, 0, W, H); /* no query */
        vkCmdEndRenderPass(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
        if (have_ts2) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, 2);
        else vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 2);
        /* The app's own copy of the same results, to compare with. */
        vkCmdCopyQueryPoolResults(cb, pool, 0, 3, buf, 0, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        if (!submit(cb)) {
            bad = it;
            break;
        }
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
        vkInvalidateMappedMemoryRanges(dev, 1, &mr);
        uint64_t v[3];
        VkResult r = vkGetQueryPoolResults(dev, pool, 0, 3, sizeof(v), v, 8, VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS || v[0] != ptr[0] || v[1] != ptr[1] || v[2] != ptr[2] || v[1] < v[0]) {
            snprintf(d, sizeof(d), "round %d: %d, %llu %llu %llu vs copy %llu %llu %llu", it, r, (unsigned long long)v[0],
                     (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)ptr[0], (unsigned long long)ptr[1],
                     (unsigned long long)ptr[2]);
            bad = it;
        }
    }
    if (bad < 0) snprintf(d, sizeof(d), "50 rounds equal to the app's own copies, increasing%s", have_ts2 ? " (incl. vkCmdWriteTimestamp2)" : "");
    check(bad < 0 && no_wire(w0), "timestamps", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

/* Queries ended inside secondaries: right whichever way they are answered; reused secondaries too. */
static void test_secondary(void)
{
    char d[256];
    VkQueryPool pool = make_pool(VK_QUERY_TYPE_OCCLUSION, 8);
    VkQueryPool tsp = make_pool(VK_QUERY_TYPE_TIMESTAMP, 2);
    VkCommandBuffer sec = alloc_cb(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, rp, 0, fb, VK_FALSE, 0, 0};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inh};
    vkBeginCommandBuffer(sec, &bi);
    counted_draw(sec, pool, 5, 16, 16);
    counted_draw(sec, pool, 6, 3, 7);
    vkEndCommandBuffer(sec);
    VkCommandBuffer sec2 = alloc_cb(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBufferInheritanceInfo inh2 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo bi2 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, &inh2};
    vkBeginCommandBuffer(sec2, &bi2);
    vkCmdWriteTimestamp(sec2, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsp, 1);
    vkEndCommandBuffer(sec2);
    int bad = -1;
    long w0 = wire_polls();
    for (int it = 0; it < 5 && bad < 0; it++) {
        /* The secondaries were recorded before this reset: their queries count all the same. */
        vkResetQueryPool(dev, pool, 0, 8);
        vkResetQueryPool(dev, tsp, 0, 2);
        VkCommandBuffer cb = begin();
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tsp, 0);
        begin_pass(cb, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(cb, 1, &sec);
        vkCmdEndRenderPass(cb);
        vkCmdExecuteCommands(cb, 1, &sec2);
        if (!submit(cb)) {
            bad = 100 + it;
            break;
        }
        uint64_t v[2], ts[2];
        VkResult r = vkGetQueryPoolResults(dev, pool, 5, 2, sizeof(v), v, 8, VK_QUERY_RESULT_64_BIT);
        VkResult rt = vkGetQueryPoolResults(dev, tsp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS || !samples_ok(v[0], 256) || !samples_ok(v[1], 21) || rt != VK_SUCCESS || ts[1] < ts[0]) {
            snprintf(d, sizeof(d), "round %d: %d %llu %llu, timestamps %d %llu %llu", it, r, (unsigned long long)v[0], (unsigned long long)v[1],
                     rt, (unsigned long long)ts[0], (unsigned long long)ts[1]);
            bad = it;
        }
    }
    long wire = wire_polls() - w0;
    if (bad < 0) snprintf(d, sizeof(d), "5 rounds of reused secondaries (render pass + plain), %ld round trips", wire);
    check(bad < 0 && no_wire(w0), "queries ended in secondary command buffers", d);
    vkDestroyQueryPool(dev, pool, NULL);
    vkDestroyQueryPool(dev, tsp, NULL);
}

/* Many pools created and destroyed (mirrors freed with them), results right in each. */
static void test_pool_churn(void)
{
    char d[128];
    int bad = -1;
    for (int it = 0; it < 64 && bad < 0; it++) {
        VkQueryPool pool = make_pool(VK_QUERY_TYPE_OCCLUSION, 1 + (uint32_t)it * 3);
        uint32_t q = (uint32_t)it * 3;
        vkResetQueryPool(dev, pool, 0, q + 1);
        VkCommandBuffer cb = begin();
        begin_pass(cb, VK_SUBPASS_CONTENTS_INLINE);
        counted_draw(cb, pool, q, 1 + (uint32_t)it % W, 2);
        vkCmdEndRenderPass(cb);
        uint64_t v = 0;
        if (!submit(cb) || vkGetQueryPoolResults(dev, pool, q, 1, 8, &v, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS ||
            !samples_ok(v, (uint64_t)(1 + it % W) * 2))
            bad = it;
        vkDestroyQueryPool(dev, pool, NULL);
    }
    snprintf(d, sizeof(d), bad < 0 ? "64 pools of 1-190 queries" : "pool %d wrong", bad);
    check(bad < 0, "query pool create/destroy churn", d);
}

int main(int argc, char **argv)
{
    int gpu = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--gpu") && i + 1 < argc) gpu = atoi(argv[++i]);
    const char *mv = getenv("VKBRIDGE_QUERY_MIRROR");
    mirror = !(mv && !strcmp(mv, "0"));
    /* The client's trace of round trips, into a file this test reads back. */
    int saved_err = dup(2);
    trace = tmpfile();
    if (trace) {
        setenv("VKBRIDGE_TRACE", "1", 1);
        dup2(fileno(trace), 2);
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-query-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    check(r == VK_SUCCESS, "instance", r ? "vkCreateInstance failed" : "");
    if (r) return 1;
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
    G(vkGetPhysicalDeviceFeatures);
#undef G
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    if ((uint32_t)gpu >= n) {
        check(0, "physical device", "none");
        return 1;
    }
    pd = pds[gpu];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(pd, &feats);
    precise = feats.occlusionQueryPrecise;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    int v13 = props.apiVersion >= VK_API_VERSION_1_3;
    char d[400];
    snprintf(d, sizeof(d), "%s, Vulkan %u.%u, precise occlusion %s, mirror %s", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion), precise ? "yes" : "no", mirror ? "on" : "off (VKBRIDGE_QUERY_MIRROR=0)");
    check(props.apiVersion >= VK_API_VERSION_1_2, "physical device", d);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13 ? &f13 : NULL};
    f12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceFeatures en = {0};
    en.occlusionQueryPrecise = feats.occlusionQueryPrecise;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 0, NULL, &en};
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    check(r == VK_SUCCESS, "device", r ? "vkCreateDevice failed" : "");
    if (r) return 1;
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkBindBufferMemory);
    G(vkMapMemory); G(vkCmdPipelineBarrier); G(vkCreateShaderModule); G(vkCreatePipelineLayout); G(vkCreateImage);
    G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateImageView); G(vkCreateRenderPass); G(vkCreateFramebuffer);
    G(vkCreateGraphicsPipelines); G(vkCmdBeginRenderPass); G(vkCmdEndRenderPass); G(vkCmdDraw); G(vkCmdBindPipeline);
    G(vkCmdPushConstants); G(vkCmdSetViewport); G(vkCmdSetScissor); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences);
    G(vkDestroyDevice); G(vkCreateQueryPool); G(vkDestroyQueryPool); G(vkResetQueryPool); G(vkCmdResetQueryPool);
    G(vkCmdBeginQuery); G(vkCmdEndQuery); G(vkCmdWriteTimestamp); G(vkCmdWriteTimestamp2); G(vkGetQueryPoolResults);
    G(vkCmdCopyQueryPoolResults); G(vkCmdExecuteCommands); G(vkResetCommandBuffer); G(vkDeviceWaitIdle);
    G(vkInvalidateMappedMemoryRanges);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &fence);
    if (!setup_draw()) {
        check(0, "draw setup", "render pass / pipeline");
        return 1;
    }

    test_occlusion();
    test_host_reset_reuse();
    test_gpu_reset();
    test_timestamps(qp[qfam].timestampValidBits, v13 && vkCmdWriteTimestamp2);
    test_secondary();
    test_pool_churn();

    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    /* The client's own messages (not its trace) back to the real stderr. */
    fflush(stderr);
    dup2(saved_err, 2);
    if (trace) {
        rewind(trace);
        char line[512];
        while (fgets(line, sizeof(line), trace))
            if (!strstr(line, ": trace: ")) fputs(line, stderr);
    }
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
