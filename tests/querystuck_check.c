/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * querystuck_check: the query mirror (client/query.c) must not answer "not ready" forever for a
 * query the GPU has finished. Deep Rock Galactic on the S11 Ultra (2026-10-27): UE4's render thread
 * spun for minutes on the mirror's not-ready answer with the GPU and the server idle; with the
 * mirror off every load passed. Each case polls the way DXVK's GetData does (64-bit,
 * no WAIT, in a loop) after the GPU is known to be done (fence waited), and fails when the answer
 * is still "not ready" after 3 s.
 *
 *   querystuck_check multiview   a multiview pass: vkCmdEndQuery ends 2 queries, the mirror copied
 *                                only the first; the second, mirrored by its host reset, never
 *                                became ready in the mirror (fails before the stuck-query check)
 *   querystuck_check lost        a lost ready-word update, imitated (VKBRIDGE_TEST_QUERY_LOSE_READY=1:
 *                                the first copy's ready words are left out), as on the Ultra
 *   querystuck_check drg         Deep Rock Galactic's pattern through DXVK 3.1 on a tiler, on two
 *                                threads: query handles from a LIFO free list, reset from the host
 *                                when handed out, begun and ended in a render pass recorded into a
 *                                secondary command buffer that the primary executes, two frames in
 *                                flight; another thread spins on each frame's queries as UE4's
 *                                render thread does. Every result must come exact, in time, and
 *                                (mirror on) with no vkGetQueryPoolResults round trip at all
 *   querystuck_check burst [n]   the first frame after Deep Rock Galactic's loading bar, as the Ultra
 *                                stalled on it (2026-10-27): DXVK 3.1.1's 16384-query occlusion pool,
 *                                n (default 640) queries from 1100 on reset one by one from the host,
 *                                each begun and ended around a draw inside a render-pass secondary
 *                                (inheritance zero-filled: occlusionQueryEnable FALSE, as DXVK's
 *                                tiler mode records it), 8 passes each executing its secondary, in
 *                                ONE primary, submitted with no wait; then polled without WAIT from
 *                                the highest down as UE4 does. Every result must come exact within
 *                                3 s: what the bridge hands the driver is the app's pattern, intact
 *   querystuck_check burst-never   the same with the Ultra's driver imitated (the upper half never
 *                                available, VKBRIDGE_TEST_QUERY_NEVER_FROM) and the fallback too slow
 *                                to answer: they must stay not ready
 *   querystuck_check burst-visible the same with VKBRIDGE_QUERY_VISIBLE_MS=300 (client/qvisible.c): the
 *                                lower half exact, the upper half answered 1 sample (the first after
 *                                300 ms, its siblings at once); a query reset and never ended, and one
 *                                ended in a primary never submitted, stay not ready
 *
 *   querystuck_check stale       Batman: Arkham Asylum on the Galaxy Tab S12+ (, UE3 through
 *                                DXVK): a query slot whose last use counted 0 samples is reset and
 *                                reused for a draw that counts many, and the second command buffer's
 *                                values reach the host late (imitated, VKBRIDGE_TEST_QUERY_LOSE_VALUE=2:
 *                                the GPU's writes seen out of order, the "ready" before the value). The
 *                                mirror must never answer "ready" with the slot's old value: late is
 *                                allowed, wrong is not
 *   querystuck_check batman [frames]  UE3's occlusion pattern on the drg model: every slot alternates
 *                                between 0 samples and a full count from one use to the next, and every
 *                                third frame resets its queries in the command buffer
 *                                (vkCmdResetQueryPool) instead of from the host (read after its fence:
 *                                until the reset runs the old result is still the query's); every
 *                                answer exact, no round trips with the mirror on
 *
 * Round trips are counted from the client's VKBRIDGE_TRACE output, as in query_check.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
static int failures, checks, mirror = 1, precise, have_multiview;
static FILE *trace;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures2);
I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer);
I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateImage);
I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkAllocateMemory); I(vkCreateImageView); I(vkCreateRenderPass);
I(vkCreateFramebuffer); I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdDraw);
I(vkCmdBindPipeline); I(vkCmdPushConstants); I(vkCmdSetViewport); I(vkCmdSetScissor); I(vkCreateFence); I(vkWaitForFences);
I(vkResetFences); I(vkDestroyDevice); I(vkCreateQueryPool); I(vkDestroyQueryPool); I(vkResetQueryPool); I(vkCmdBeginQuery);
I(vkCmdEndQuery); I(vkGetQueryPoolResults); I(vkCmdExecuteCommands); I(vkResetCommandPool); I(vkDeviceWaitIdle);
I(vkDestroyCommandPool); I(vkCmdResetQueryPool);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

static long trace_count(const char *what)
{
    if (!trace) return 0;
    pthread_mutex_lock(&trace_lock);
    fflush(stderr);
    rewind(trace);
    char line[512];
    long n = 0;
    while (fgets(line, sizeof(line), trace))
        if (strstr(line, what)) n++;
    fseek(trace, 0, SEEK_END);
    pthread_mutex_unlock(&trace_lock);
    return n;
}

static long wire_polls(void) { return trace_count("trace: vkGetQueryPoolResults"); }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
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
typedef struct target {
    VkRenderPass rp;
    VkFramebuffer fb;
    VkPipeline pipe;
} target;
static VkPipelineLayout pl;
static target plain, multi;

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

/* layers 1: a plain pass; 2: a multiview pass drawing both layers (view mask 0b11). */
static int setup_target(target *t, uint32_t layers)
{
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, layers, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    uint32_t ty = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ty == UINT32_MAX) ty = find_type(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, ty};
    VkDeviceMemory mem;
    if (vkAllocateMemory(dev, &ai, NULL, &mem) || vkBindImageMemory(dev, img, mem, 0)) return 0;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img,
                                 layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, fmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkImageView view;
    if (vkCreateImageView(dev, &vci, NULL, &view)) return 0;
    VkAttachmentDescription att = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    uint32_t mask = (1u << layers) - 1, corr = mask;
    VkRenderPassMultiviewCreateInfo mv = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO, NULL, 1, &mask, 0, NULL, 1, &corr};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, layers > 1 ? &mv : NULL, 0, 1, &att, 1, &sub, 0, NULL};
    if (vkCreateRenderPass(dev, &rpci, NULL, &t->rp)) return 0;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, t->rp, 1, &view, W, H, 1};
    if (vkCreateFramebuffer(dev, &fci, NULL, &t->fb)) return 0;
    if (!pl) {
        VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
        if (vkCreatePipelineLayout(dev, &plci, NULL, &pl)) return 0;
    }
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
                                         pl, t->rp, 0, VK_NULL_HANDLE, -1};
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &t->pipe) == VK_SUCCESS;
}

static VkCommandBuffer alloc_cb(VkCommandPool pool, VkCommandBufferLevel level)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, level, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    return cb;
}

static void begin_primary(VkCommandBuffer cb)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
}

static void begin_pass(VkCommandBuffer cb, const target *t, VkSubpassContents contents)
{
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 1}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, t->rp, t->fb, {{0, 0}, {W, H}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, contents);
}

/* Inside a render pass: draws w x h samples (per view) into query q of pool. */
static void counted_draw(VkCommandBuffer cb, const target *t, VkQueryPool pool, uint32_t q, uint32_t w, uint32_t h)
{
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, t->pipe);
    VkViewport v = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color);
    vkCmdBeginQuery(cb, pool, q, precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndQuery(cb, pool, q);
}

static int submit_wait(VkCommandBuffer cb, VkFence f)
{
    if (vkEndCommandBuffer(cb)) return 0;
    vkResetFences(dev, 1, &f);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    return vkQueueSubmit(queue, 1, &si, f) == VK_SUCCESS && vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull) == VK_SUCCESS;
}

static VkQueryPool make_pool(uint32_t n)
{
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, n, 0};
    VkQueryPool p = VK_NULL_HANDLE;
    vkCreateQueryPool(dev, &ci, NULL, &p);
    return p;
}

/* DXVK's GetData: 64-bit, no flags that write, in a loop until ready or limit seconds. */
static VkResult spin(VkQueryPool pool, uint32_t q, uint64_t *v, double limit, double *took, long *polls)
{
    double t0 = now_s();
    VkResult r;
    long n = 0;
    do {
        n++;
        r = vkGetQueryPoolResults(dev, pool, q, 1, 8, v, 8, VK_QUERY_RESULT_64_BIT);
        if (r != VK_NOT_READY) break;
    } while (now_s() - t0 < limit);
    *took = now_s() - t0;
    if (polls) *polls += n;
    return r;
}

/* ------------------------------------------------------------------ cases */

static void case_multiview(void)
{
    char d[256];
    if (!have_multiview) {
        check(1, "multiview pass: the second view's query", "skipped (no multiview)");
        return;
    }
    VkFence f;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &f);
    VkQueryPool pool = make_pool(4);
    vkResetQueryPool(dev, pool, 0, 4); /* DXVK's way: reset from the host; it mirrors them */
    VkCommandBuffer cb = alloc_cb(cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_primary(cb);
    begin_pass(cb, &multi, VK_SUBPASS_CONTENTS_INLINE);
    counted_draw(cb, &multi, pool, 0, 16, 8); /* ends queries 0 and 1 (two views) */
    vkCmdEndRenderPass(cb);
    int ok = submit_wait(cb, f);
    long w0 = wire_polls();
    uint64_t v0 = 0, v1 = 0;
    double t0 = 0, t1 = 0;
    VkResult r0 = spin(pool, 0, &v0, 3.0, &t0, NULL);
    VkResult r1 = spin(pool, 1, &v1, 3.0, &t1, NULL);
    long wire = wire_polls() - w0;
    /* Where each view's samples land is the driver's choice: their sum is the draw's. */
    int sum_ok = precise ? v0 + v1 == 2 * 16 * 8 || v0 + v1 == 16 * 8 : v0 + v1 != 0;
    snprintf(d, sizeof(d), "query 0: %d after %.0f ms, query 1: %d after %.0f ms, %llu + %llu samples, %ld round trips", r0, t0 * 1e3, r1,
             t1 * 1e3, (unsigned long long)v0, (unsigned long long)v1, wire);
    check(ok && r0 == VK_SUCCESS && r1 == VK_SUCCESS && sum_ok, "multiview pass: both views' queries become ready (GPU done)", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

static void case_lost(void)
{
    char d[256];
    VkFence f;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &f);
    VkQueryPool pool = make_pool(8);
    int bad = -1;
    double worst = 0;
    long wire_total = 0;
    for (int it = 0; it < 4 && bad < 0; it++) {
        vkResetQueryPool(dev, pool, 0, 8);
        VkCommandBuffer cb = alloc_cb(cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_primary(cb);
        begin_pass(cb, &plain, VK_SUBPASS_CONTENTS_INLINE);
        for (uint32_t q = 0; q < 3; q++) counted_draw(cb, &plain, pool, q, 4 + q * 3, 5 + q);
        vkCmdEndRenderPass(cb);
        if (!submit_wait(cb, f)) {
            bad = 100 + it;
            break;
        }
        long w0 = wire_polls();
        for (uint32_t q = 0; q < 3 && bad < 0; q++) {
            uint64_t v = 0;
            double took = 0;
            VkResult r = spin(pool, q, &v, 3.0, &took, NULL);
            uint64_t want = (uint64_t)(4 + q * 3) * (5 + q);
            if (took > worst) worst = took;
            if (r != VK_SUCCESS || (precise ? v != want : v == 0)) {
                snprintf(d, sizeof(d), "round %d query %u: %d after %.0f ms, %llu (want %llu)", it, q, r, took * 1e3, (unsigned long long)v,
                         (unsigned long long)want);
                bad = it;
            }
        }
        long wire = wire_polls() - w0;
        wire_total += wire;
        /* Only the first round lost its update: the later ones are the mirror's, no round trip. */
        if (bad < 0 && it > 0 && mirror && wire) {
            snprintf(d, sizeof(d), "round %d: %ld round trips after the lost update was over", it, wire);
            bad = it;
        }
    }
    long told = trace_count("left out (VKBRIDGE_TEST_QUERY_LOSE_READY=1)");
    if (bad < 0)
        snprintf(d, sizeof(d), "4 rounds, slowest %.0f ms, %ld round trips (round 1 only), lost update imitated: %s", worst * 1e3, wire_total,
                 told ? "yes" : "NO (this client has no VKBRIDGE_TEST_QUERY_LOSE_READY)");
    check(bad < 0 && told > 0, "lost ready-word update: the query still comes ready", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

/* ------------------------------------------------------------------ a stale value */

static void case_stale(void)
{
    char d[300];
    VkFence f;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &f);
    VkQueryPool pool = make_pool(8);
    /* Round 0: 0 samples (a hidden object). Round 1: the same slots, now a large occluder, with its
     * values late (the imitation). Round 2: the same again, nothing imitated. */
    static const uint32_t ws[3][4] = {{0, 0, 0, 0}, {64, 48, 40, 32}, {0, 64, 0, 24}};
    int bad = -1, late = 0;
    long wire_late = 0;
    double worst = 0;
    for (int it = 0; it < 3 && bad < 0; it++) {
        vkResetQueryPool(dev, pool, 0, 4);
        VkCommandBuffer cb = alloc_cb(cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin_primary(cb);
        begin_pass(cb, &plain, VK_SUBPASS_CONTENTS_INLINE);
        for (uint32_t q = 0; q < 4; q++) counted_draw(cb, &plain, pool, q, ws[it][q], ws[it][q]);
        vkCmdEndRenderPass(cb);
        if (!submit_wait(cb, f)) {
            snprintf(d, sizeof(d), "round %d: submit", it);
            bad = it;
            break;
        }
        long w0 = wire_polls();
        for (uint32_t q = 0; q < 4 && bad < 0; q++) {
            uint64_t v = ~0ull;
            double took = 0;
            VkResult r = spin(pool, q, &v, 3.0, &took, NULL);
            uint64_t want = (uint64_t)ws[it][q] * ws[it][q];
            if (took > worst) worst = took;
            if (it == 1 && took > 0.05) late++;
            if (r != VK_SUCCESS || (precise ? v != want : (v != 0) != (want != 0))) {
                snprintf(d, sizeof(d), "round %d query %u: %d after %.0f ms, %llu samples (want %llu)%s", it, q, r, took * 1e3,
                         (unsigned long long)v, (unsigned long long)want,
                         r == VK_SUCCESS && v == (uint64_t)ws[0][q] * ws[0][q] ? ": the slot's previous value" : "");
                bad = it;
            }
        }
        if (it == 1) wire_late = wire_polls() - w0;
        if (bad < 0 && it == 2 && mirror && wire_polls() - w0) {
            snprintf(d, sizeof(d), "round 2: %ld round trips with nothing imitated", wire_polls() - w0);
            bad = it;
        }
    }
    long told = trace_count("values are left out (VKBRIDGE_TEST_QUERY_LOSE_VALUE)");
    if (bad < 0)
        snprintf(d, sizeof(d), "3 rounds exact, slowest %.0f ms, %d late answer(s) and %ld round trip(s) in the imitated round, imitation %s",
                 worst * 1e3, late, wire_late, told ? "on" : mirror ? "NOT ON (this client has no VKBRIDGE_TEST_QUERY_LOSE_VALUE)" : "not needed (mirror off)");
    check(bad < 0 && (told > 0 || !mirror), "a reused slot whose new value reaches the host late: never its old value", d);
    vkDestroyQueryPool(dev, pool, NULL);
}

/* ------------------------------------------------------------------ Deep Rock Galactic's pattern */

#define DRG_POOL 256
#define DRG_PER_FRAME 24
#define DRG_INFLIGHT 2
typedef struct drg_frame {
    VkCommandPool pool;
    VkCommandBuffer prim, sec[4];
    VkFence fence;
    uint32_t q[DRG_PER_FRAME];
    uint64_t want[DRG_PER_FRAME];
    int busy;
} drg_frame;

static VkQueryPool drg_pool;
static uint32_t drg_free[DRG_POOL], drg_nfree;
/* Handed from the recording thread to the spinning one: the frame number whose queries to spin on. */
static pthread_mutex_t drg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t drg_cond = PTHREAD_COND_INITIALIZER;
static long drg_posted = -1, drg_done = -1, drg_stuck, drg_wrong, drg_polls;
static double drg_worst;
static drg_frame drg_fr[DRG_INFLIGHT];
static char drg_why[200];

/* batman: UE3's pattern: 0 samples and full counts alternating per slot, every third
 * frame reset in the command buffer */
static int drg_batman;

static void *drg_spinner(void *arg)
{
    long frames = (long)(intptr_t)arg;
    for (long f = 0; f < frames; f++) {
        pthread_mutex_lock(&drg_lock);
        while (drg_posted < f) pthread_cond_wait(&drg_cond, &drg_lock);
        pthread_mutex_unlock(&drg_lock);
        drg_frame *fr = &drg_fr[f % DRG_INFLIGHT];
        long polls = 0;
        /* A query reset in a command buffer stays available, with its old result, until the reset
         * executes (the driver says so too): those frames are read once their work is done. */
        if (drg_batman && f % 3 == 2) vkWaitForFences(dev, 1, &fr->fence, VK_TRUE, 10000000000ull);
        for (int i = 0; i < DRG_PER_FRAME; i++) {
            uint64_t v = 0;
            double took = 0;
            VkResult r = spin(drg_pool, fr->q[i], &v, 3.0, &took, &polls);
            if (took > drg_worst) drg_worst = took;
            if (r == VK_NOT_READY) {
                if (!drg_stuck++) snprintf(drg_why, sizeof(drg_why), "frame %ld query %u not ready after 3 s", f, fr->q[i]);
            } else if (r != VK_SUCCESS || (precise ? v != fr->want[i] : (v != 0) != (fr->want[i] != 0))) {
                if (!drg_wrong++)
                    snprintf(drg_why, sizeof(drg_why), "frame %ld query %u: %d, %llu (want %llu)", f, fr->q[i], r, (unsigned long long)v,
                             (unsigned long long)fr->want[i]);
            }
        }
        pthread_mutex_lock(&drg_lock);
        drg_polls += polls;
        drg_done = f;
        pthread_cond_broadcast(&drg_cond);
        pthread_mutex_unlock(&drg_lock);
    }
    return NULL;
}

static void case_drg(long frames)
{
    char d[300];
    drg_pool = make_pool(DRG_POOL);
    for (uint32_t i = 0; i < DRG_POOL; i++) drg_free[i] = DRG_POOL - 1 - i;
    drg_nfree = DRG_POOL;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < DRG_INFLIGHT; i++) {
        vkCreateCommandPool(dev, &pci, NULL, &drg_fr[i].pool);
        drg_fr[i].prim = alloc_cb(drg_fr[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        for (int s = 0; s < 4; s++) drg_fr[i].sec[s] = alloc_cb(drg_fr[i].pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        vkCreateFence(dev, &fci, NULL, &drg_fr[i].fence);
    }
    long w0 = wire_polls();
    pthread_t th;
    pthread_create(&th, NULL, drg_spinner, (void *)(intptr_t)frames);
    int bad = 0;
    double t0 = now_s();
    for (long f = 0; f < frames && !bad; f++) {
        drg_frame *fr = &drg_fr[f % DRG_INFLIGHT];
        /* The frame's slot is free once the spinner is done with its queries and the GPU with its
         * work (DXVK: the command list recycled, the handles back on the free list). */
        pthread_mutex_lock(&drg_lock);
        while (drg_done < f - DRG_INFLIGHT) pthread_cond_wait(&drg_cond, &drg_lock);
        pthread_mutex_unlock(&drg_lock);
        if (vkWaitForFences(dev, 1, &fr->fence, VK_TRUE, 10000000000ull) != VK_SUCCESS) {
            bad = 1;
            break;
        }
        if (fr->busy)
            for (int i = DRG_PER_FRAME; i-- > 0;) drg_free[drg_nfree++] = fr->q[i];
        vkResetCommandPool(dev, fr->pool, 0);
        begin_primary(fr->prim);
        int cmd_reset = drg_batman && f % 3 == 2;
        /* Four render passes, each recorded into a secondary (DXVK's tiler mode); queries handed
         * out LIFO and reset from the host as DXVK's restartQueries does. */
        int qi = 0;
        for (int s = 0; s < 4; s++) {
            VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, plain.rp, 0, plain.fb, VK_FALSE, 0, 0};
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                           VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inh};
            vkBeginCommandBuffer(fr->sec[s], &bi);
            for (int k = 0; k < DRG_PER_FRAME / 4; k++, qi++) {
                uint32_t q = drg_free[--drg_nfree];
                if (cmd_reset) vkCmdResetQueryPool(fr->prim, drg_pool, q, 1); /* in the primary, before its passes */
                else vkResetQueryPool(dev, drg_pool, q, 1);
                uint32_t w = 1 + (uint32_t)((f * 7 + qi * 13) % W), h = 1 + (uint32_t)((f * 3 + qi * 5) % H);
                static uint32_t uses[DRG_POOL];
                if (drg_batman && uses[q]++ % 2) w = h = 0;          /* hidden this use, counted the next */
                else if (drg_batman) w = W, h = H - (uint32_t)(q % 7); /* a large occluder */
                fr->q[qi] = q;
                fr->want[qi] = (uint64_t)w * h;
                counted_draw(fr->sec[s], &plain, drg_pool, q, w, h);
            }
            vkEndCommandBuffer(fr->sec[s]);
            begin_pass(fr->prim, &plain, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
            vkCmdExecuteCommands(fr->prim, 1, &fr->sec[s]);
            vkCmdEndRenderPass(fr->prim);
        }
        fr->busy = 1;
        vkEndCommandBuffer(fr->prim);
        vkResetFences(dev, 1, &fr->fence);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &fr->prim, 0, NULL};
        if (vkQueueSubmit(queue, 1, &si, fr->fence) != VK_SUCCESS) bad = 1;
        pthread_mutex_lock(&drg_lock);
        drg_posted = f;
        pthread_cond_broadcast(&drg_cond);
        pthread_mutex_unlock(&drg_lock);
    }
    pthread_join(th, NULL);
    double secs = now_s() - t0;
    vkDeviceWaitIdle(dev);
    long wire = wire_polls() - w0;
    snprintf(d, sizeof(d), "%ld frames x %d queries in %.1f s, %ld polls, slowest %.1f ms, %ld round trips%s%s", frames, DRG_PER_FRAME, secs,
             drg_polls, drg_worst * 1e3, wire, *drg_why ? "; " : "", drg_why);
    check(!bad && !drg_stuck && !drg_wrong && (!mirror || wire == 0),
          drg_batman ? "UE3's pattern (slots alternating 0 and full counts, host and command-buffer resets, a spinning thread)"
                     : "Deep Rock Galactic's pattern (host resets, secondaries, a spinning thread)",
          d);
    for (int i = 0; i < DRG_INFLIGHT; i++) vkDestroyCommandPool(dev, drg_fr[i].pool, NULL);
    vkDestroyQueryPool(dev, drg_pool, NULL);
}


/* ------------------------------------------------------------------ the first frame after the loading bar */

#define BURST_POOL 16384
#define BURST_BASE 1100
#define BURST_PASSES 8

/* mode 0: burst, 1: burst-never, 2: burst-visible */
static void case_burst(int mode, uint32_t n)
{
    char d[400];
    if (n < BURST_PASSES) n = BURST_PASSES;
    if (n > BURST_POOL - BURST_BASE - 8) n = BURST_POOL - BURST_BASE - 8;
    uint32_t per = n / BURST_PASSES;
    n = per * BURST_PASSES;
    VkQueryPool pool = make_pool(BURST_POOL);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cp;
    vkCreateCommandPool(dev, &pci, NULL, &cp);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);
    uint64_t *want = calloc(n, sizeof(*want));
    VkCommandBuffer prim = alloc_cb(cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY), sec[BURST_PASSES];
    begin_primary(prim);
    for (int s = 0; s < BURST_PASSES; s++) {
        sec[s] = alloc_cb(cp, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        /* DXVK: VkCommandBufferInheritanceInfo zero-initialised but for the rendering formats */
        VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, plain.rp, 0, plain.fb, VK_FALSE, 0, 0};
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                       VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inh};
        vkBeginCommandBuffer(sec[s], &bi);
        for (uint32_t k = 0; k < per; k++) {
            uint32_t i = s * per + k, q = BURST_BASE + i;
            vkResetQueryPool(dev, pool, q, 1); /* DXVK: at record time, one by one */
            uint32_t w = 1 + (i * 7) % W, h = 1 + (i * 5) % H;
            want[i] = (uint64_t)w * h;
            counted_draw(sec[s], &plain, pool, q, w, h);
        }
        vkEndCommandBuffer(sec[s]);
        begin_pass(prim, &plain, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(prim, 1, &sec[s]);
        vkCmdEndRenderPass(prim);
    }
    vkEndCommandBuffer(prim);
    /* Two that must never be answered by the fallback: reset and never ended; ended in a primary
     * that is never submitted. */
    uint32_t q_noend = BURST_BASE + n + 1, q_nosub = BURST_BASE + n + 2;
    vkResetQueryPool(dev, pool, q_noend, 2);
    VkCommandBuffer lone = alloc_cb(cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_primary(lone);
    begin_pass(lone, &plain, VK_SUBPASS_CONTENTS_INLINE);
    counted_draw(lone, &plain, pool, q_nosub, 4, 4);
    vkCmdEndRenderPass(lone);
    vkEndCommandBuffer(lone);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &prim, 0, NULL};
    double t0 = now_s();
    VkResult sr = vkQueueSubmit(queue, 1, &si, f);
    uint32_t never = mode ? BURST_BASE + n / 2 : UINT32_MAX; /* the imitation's first query */
    uint32_t exact = 0, visible = 0, stuck = 0, wrong = 0, first_wrong = UINT32_MAX;
    double worst = 0, first_visible = 0, last_visible = 0;
    long polls = 0;
    for (uint32_t i = n; i-- > 0;) {
        uint32_t q = BURST_BASE + i;
        uint64_t v = 0;
        double took = 0;
        /* burst-never: the imitated ones are only checked to stay not ready for a moment */
        double limit = mode == 1 && q >= never ? (i == n - 1 || i == n / 2 ? 0.5 : 0.002) : 3.0;
        VkResult r = spin(pool, q, &v, limit, &took, &polls);
        if (took > worst) worst = took;
        int imitated = q >= never, ok = r == VK_SUCCESS && (precise ? v == want[i] : v != 0), good;
        if (!imitated) good = ok, exact += ok;
        else if (mode == 1) good = r == VK_NOT_READY, stuck += good;
        else good = r == VK_SUCCESS && v == 1, visible += good;
        if (good && imitated && mode == 2) {
            double at = now_s() - t0;
            if (!first_visible) first_visible = at;
            last_visible = at;
        }
        if (!good && !wrong++) first_wrong = q;
    }
    uint64_t v = 0;
    double tn = 0, ts = 0;
    VkResult rn = spin(pool, q_noend, &v, mode == 2 ? 0.8 : 0.05, &tn, NULL);
    VkResult rs = spin(pool, q_nosub, &v, mode == 2 ? 0.8 : 0.05, &ts, NULL);
    int fence_ok = vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull) == VK_SUCCESS;
    long told = trace_count("query visible #");
    uint32_t half = n - n / 2;
    if (mode == 0) {
        snprintf(d, sizeof(d), "%u queries in %d secondaries of one primary: %u exact, %u not, slowest %.1f ms, %ld polls%s", n, BURST_PASSES,
                 exact, wrong, worst * 1e3, polls, wrong ? " (first wrong below)" : told ? " (FALLBACK ANSWERED)" : "");
        check(sr == VK_SUCCESS && fence_ok && exact == n && !wrong && !told, "DRG's first frame after the loading bar: every query exact", d);
        if (wrong) printf("  first wrong query %u\n", first_wrong);
    } else if (mode == 1) {
        snprintf(d, sizeof(d), "%u exact, %u stay not ready (imitated), %u other", exact, stuck, wrong);
        check(sr == VK_SUCCESS && fence_ok && exact == n / 2 && stuck == half && !wrong,
              "the imitated driver leaves the upper half not ready (VKBRIDGE_TEST_QUERY_NEVER_FROM)", d);
    } else {
        snprintf(d, sizeof(d), "%u exact, %u answered visible (first at %.0f ms, last at %.0f ms after the submit), %u other; "
                 "never ended: %s, never submitted: %s; %ld line(s)",
                 exact, visible, first_visible * 1e3, last_visible * 1e3, wrong, rn == VK_NOT_READY ? "not ready" : "ANSWERED",
                 rs == VK_NOT_READY ? "not ready" : "ANSWERED", told);
        check(sr == VK_SUCCESS && fence_ok && exact == n / 2 && visible == half && !wrong && first_visible >= 0.3 &&
                  last_visible - first_visible < 1.0 && rn == VK_NOT_READY && rs == VK_NOT_READY && told >= 1,
              "VKBRIDGE_QUERY_VISIBLE_MS answers a done submission's stuck queries visible, nothing else", d);
    }
    free(want);
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyQueryPool(dev, pool, NULL);
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "all";
    long frames = argc > 2 ? atol(argv[2]) : 600;
    int all = !strcmp(what, "all");
    const char *mv = getenv("VKBRIDGE_QUERY_MIRROR");
    mirror = !(mv && !strcmp(mv, "0"));
    /* occlusion from the server: its round trips are expected (timestamps' are not counted then) */
    const char *mo = getenv("VKBRIDGE_QUERY_MIRROR_OCCLUSION");
    if (mo && !strcmp(mo, "0")) mirror = 0;
    if (!strcmp(what, "lost")) setenv("VKBRIDGE_TEST_QUERY_LOSE_READY", "1", 1);
    if (!strcmp(what, "stale")) setenv("VKBRIDGE_TEST_QUERY_LOSE_VALUE", "2", 1);
    drg_batman = !strcmp(what, "batman");
    int burst = !strcmp(what, "burst") ? 0 : !strcmp(what, "burst-never") ? 1 : !strcmp(what, "burst-visible") ? 2 : -1;
    uint32_t burst_n = argc > 2 ? (uint32_t)atol(argv[2]) : 640;
    if (burst > 0) {
        /* The imitation sits on the server's answers: the mirror would answer first */
        char from[16];
        snprintf(from, sizeof(from), "%u", BURST_BASE + (burst_n / BURST_PASSES * BURST_PASSES) / 2);
        setenv("VKBRIDGE_TEST_QUERY_NEVER_FROM", from, 1);
        setenv("VKBRIDGE_QUERY_MIRROR", "0", 1);
        /* burst-never: the fallback's tracking on (the imitation needs the pool types) but too slow to answer */
        setenv("VKBRIDGE_QUERY_VISIBLE_MS", burst == 2 ? "300" : "600000", 1);
        mirror = 0;
    }
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-querystuck-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    check(r == VK_SUCCESS, "instance", r ? "vkCreateInstance failed" : "");
    if (r) return 1;
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
    G(vkGetPhysicalDeviceFeatures2);
#undef G
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        check(0, "physical device", "none");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    precise = f2.features.occlusionQueryPrecise;
    have_multiview = f11.multiview;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    char d[512];
    snprintf(d, sizeof(d), "%s, precise occlusion %s, multiview %s, mirror %s", props.deviceName, precise ? "yes" : "no",
             have_multiview ? "yes" : "no", mirror ? "on" : "off (VKBRIDGE_QUERY_MIRROR=0)");
    check(props.apiVersion >= VK_API_VERSION_1_2, "physical device", d);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceVulkan11Features e11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
    e11.multiview = have_multiview;
    VkPhysicalDeviceFeatures en = {0};
    en.occlusionQueryPrecise = precise;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e11, 0, 1, &qci, 0, NULL, 0, NULL, &en};
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    check(r == VK_SUCCESS, "device", r ? "vkCreateDevice failed" : "");
    if (r) return 1;
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateShaderModule); G(vkCreatePipelineLayout); G(vkCreateImage); G(vkGetImageMemoryRequirements);
    G(vkBindImageMemory); G(vkAllocateMemory); G(vkCreateImageView); G(vkCreateRenderPass); G(vkCreateFramebuffer);
    G(vkCreateGraphicsPipelines); G(vkCmdBeginRenderPass); G(vkCmdEndRenderPass); G(vkCmdDraw); G(vkCmdBindPipeline);
    G(vkCmdPushConstants); G(vkCmdSetViewport); G(vkCmdSetScissor); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences);
    G(vkDestroyDevice); G(vkCreateQueryPool); G(vkDestroyQueryPool); G(vkResetQueryPool); G(vkCmdBeginQuery); G(vkCmdEndQuery);
    G(vkGetQueryPoolResults); G(vkCmdExecuteCommands); G(vkResetCommandPool); G(vkDeviceWaitIdle); G(vkDestroyCommandPool);
    G(vkCmdResetQueryPool);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    if (!setup_target(&plain, 1) || (have_multiview && !setup_target(&multi, 2))) {
        check(0, "draw setup", "render pass / pipeline");
        return 1;
    }
    if (!strcmp(what, "lost")) case_lost();
    if (all || !strcmp(what, "multiview")) case_multiview();
    if (all || !strcmp(what, "drg") || drg_batman) case_drg(frames);
    if (!strcmp(what, "stale")) case_stale();
    if (burst >= 0) case_burst(burst, burst_n);

    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
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
