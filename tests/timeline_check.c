/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * timeline_check: timeline semaphores as a client uses them (gamescope, Zink), for
 * host/timeline_test.sh. A Vulkan 1.1 instance and VK_KHR_timeline_semaphore (as on Mali r18/r19,
 * where the bridge keeps the timelines: server_timeline.c), or a Vulkan 1.2 one with the core
 * names ("core" argument, gamescope's way once promoted). Each step prints "ok"/"FAIL":
 *   1  the extension (or 1.2), the feature and maxTimelineSemaphoreValueDifference reported
 *   2  initial value; a host signal; waits that are met (timeout 0) and one that times out (20 ms)
 *   3  a GPU signal (64 MiB filled 8 times) with the app's fence: the wait returns once the work is
 *      done, the value is the signalled one, the app's fence signalled
 *   4  one submit of two batches: the second waits for the first's value and copies what it wrote:
 *      the copy holds the first batch's pattern (the order held)
 *   5  wait-before-signal: another thread submits a batch waiting for 20 that writes a pattern and
 *      signals 21; after 100 ms the value is still below 21; the host signals 20; 21 is reached and
 *      the pattern is there
 *   6  a wait for any of two semaphores, one of them reached
 *   7  binary semaphores beside it: one submit signals one, the next waits on it, with a
 *      VkTimelineSemaphoreSubmitInfo in the chain of a batch that has no timeline semaphore
 *   8  gamescope's pattern: 200 submits each signalling the next number, the value read as it goes
 *      never goes back, the last one waited for
 * "gamescope": the core names, as gamescope 3.16 asks (application name "gamescope", Vulkan 1.3
 * requested; the device with VkPhysicalDeviceVulkan12Features timelineSemaphore + scalarBlockLayout
 * and VkPhysicalDeviceVulkan13Features dynamicRendering, whatever the queries said).
 * Exit status: 0 all passed, 1 a failure, 77 no timeline semaphores offered.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c) printf("  ok   ");                                                                                      \
        else printf("  FAIL "), fails++;                                                                               \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
static VkQueue queue;
static PFN_vkQueueSubmit QueueSubmit;
static PFN_vkWaitSemaphores WaitSemaphores;
static PFN_vkSignalSemaphore SignalSemaphore;
static PFN_vkGetSemaphoreCounterValue GetValue;
static PFN_vkAllocateCommandBuffers AllocCmd;
static PFN_vkBeginCommandBuffer BeginCmd;
static PFN_vkEndCommandBuffer EndCmd;
static PFN_vkCmdFillBuffer CmdFill;
static PFN_vkCmdCopyBuffer CmdCopy;
static VkCommandPool pool;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static VkCommandBuffer cmd_fill(VkBuffer b, VkDeviceSize size, uint32_t pattern, int times)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    AllocCmd(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    BeginCmd(cb, &bi);
    for (int i = 0; i < times; i++) CmdFill(cb, b, 0, size, i == times - 1 ? pattern : pattern ^ 0xffffffffu);
    EndCmd(cb);
    return cb;
}

static VkCommandBuffer cmd_copy(VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    AllocCmd(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    BeginCmd(cb, &bi);
    VkBufferCopy r = {0, 0, size};
    CmdCopy(cb, src, dst, 1, &r);
    EndCmd(cb);
    return cb;
}

static VkResult submit(VkSemaphore *wait, const uint64_t *wv, uint32_t nw, VkCommandBuffer cb, VkSemaphore *sig, const uint64_t *sv,
                       uint32_t ns, VkFence f)
{
    VkPipelineStageFlags st[4] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, nw, wv, ns, sv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti, nw, wait, st, cb ? 1 : 0, &cb, ns, sig};
    pthread_mutex_lock(&qlock);
    VkResult r = QueueSubmit(queue, 1, &si, f);
    pthread_mutex_unlock(&qlock);
    return r;
}

static VkResult wait1(VkSemaphore s, uint64_t v, uint64_t timeout)
{
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s, &v};
    return WaitSemaphores(dev, &wi, timeout);
}

static uint64_t value(VkSemaphore s)
{
    uint64_t v = 0;
    GetValue(dev, s, &v);
    return v;
}

typedef struct mapped_buf {
    VkBuffer b;
    VkDeviceMemory m;
    uint32_t *p;
} mapped_buf;

typedef struct thr_arg {
    VkSemaphore s;
    mapped_buf *buf;
    VkResult r;
} thr_arg;

static void *wbs_thread(void *a)
{
    thr_arg *t = a;
    VkCommandBuffer cb;
    pthread_mutex_lock(&qlock);
    cb = cmd_fill(t->buf->b, 4096, 0xabcd1234u, 1);
    pthread_mutex_unlock(&qlock);
    uint64_t w = 20, s = 21;
    t->r = submit(&t->s, &w, 1, cb, &t->s, &s, 1, VK_NULL_HANDLE);
    return NULL;
}

int main(int argc, char **argv)
{
    int gs = argc > 1 && !strcmp(argv[1], "gamescope");
    int core = gs || (argc > 1 && !strcmp(argv[1], "core"));
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, gs ? "gamescope" : "timeline_check", 1, "none", 1,
                             gs ? VK_API_VERSION_1_3 : core ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkCreateDevice);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n || !pd) {
        printf("FAIL no GPU\n");
        return 1;
    }
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
    VkExtensionProperties *e = calloc(en ? en : 1, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, e);
    int ext = 0;
    for (uint32_t i = 0; i < en; i++) ext |= !strcmp(e[i].extensionName, "VK_KHR_timeline_semaphore");
    free(e);
    if (core && p2.properties.apiVersion < VK_API_VERSION_1_2) {
        printf("SKIP: the device is Vulkan 1.%u\n", VK_API_VERSION_MINOR(p2.properties.apiVersion));
        return 77;
    }
    if (!core && !ext) {
        printf("SKIP: no VK_KHR_timeline_semaphore\n");
        return 77;
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures tf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core ? (void *)&f12 : (void *)&tf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceTimelineSemaphoreProperties tp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, core ? (void *)&p12 : (void *)&tp};
    vkGetPhysicalDeviceProperties2(pd, &pp);
    VkBool32 feat = core ? f12.timelineSemaphore : tf.timelineSemaphore;
    uint64_t diff = core ? p12.maxTimelineSemaphoreValueDifference : tp.maxTimelineSemaphoreValueDifference;
    CHECK(feat && diff >= (1ull << 31), "1 %s, timelineSemaphore %u, maxTimelineSemaphoreValueDifference %llu",
          core ? "Vulkan 1.2" : "VK_KHR_timeline_semaphore", feat, (unsigned long long)diff);

    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceTimelineSemaphoreFeatures etf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, NULL, VK_TRUE};
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    if (gs) {
        e12.scalarBlockLayout = VK_TRUE;
        e12.pNext = &e13;
        printf("  device: %s, Vulkan 1.%u\n", p2.properties.deviceName, VK_API_VERSION_MINOR(p2.properties.apiVersion));
    }
    const char *exts[] = {"VK_KHR_timeline_semaphore"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, core ? (void *)&e12 : (void *)&etf, 0, 1, &q, 0, NULL, core ? 0 : 1, exts};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) {
        printf("FAIL device: %d\n", r);
        return 1;
    }
#define GD(name, var, alt) var = (PFN_##name)gdpa(dev, core ? #name : alt)
    GD(vkQueueSubmit, QueueSubmit, "vkQueueSubmit");
    GD(vkWaitSemaphores, WaitSemaphores, "vkWaitSemaphoresKHR");
    GD(vkSignalSemaphore, SignalSemaphore, "vkSignalSemaphoreKHR");
    GD(vkGetSemaphoreCounterValue, GetValue, "vkGetSemaphoreCounterValueKHR");
    GD(vkAllocateCommandBuffers, AllocCmd, "vkAllocateCommandBuffers");
    GD(vkBeginCommandBuffer, BeginCmd, "vkBeginCommandBuffer");
    GD(vkEndCommandBuffer, EndCmd, "vkEndCommandBuffer");
    GD(vkCmdFillBuffer, CmdFill, "vkCmdFillBuffer");
    GD(vkCmdCopyBuffer, CmdCopy, "vkCmdCopyBuffer");
    PFN_vkGetDeviceQueue GetQueue = (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue");
    PFN_vkCreateSemaphore CreateSem = (PFN_vkCreateSemaphore)gdpa(dev, "vkCreateSemaphore");
    PFN_vkDestroySemaphore DestroySem = (PFN_vkDestroySemaphore)gdpa(dev, "vkDestroySemaphore");
    PFN_vkCreateCommandPool CreatePool = (PFN_vkCreateCommandPool)gdpa(dev, "vkCreateCommandPool");
    PFN_vkCreateFence CreateFence = (PFN_vkCreateFence)gdpa(dev, "vkCreateFence");
    PFN_vkGetFenceStatus FenceStatus = (PFN_vkGetFenceStatus)gdpa(dev, "vkGetFenceStatus");
    PFN_vkCreateBuffer CreateBuffer = (PFN_vkCreateBuffer)gdpa(dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements BufReq = (PFN_vkGetBufferMemoryRequirements)gdpa(dev, "vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory AllocMem = (PFN_vkAllocateMemory)gdpa(dev, "vkAllocateMemory");
    PFN_vkBindBufferMemory BindBuf = (PFN_vkBindBufferMemory)gdpa(dev, "vkBindBufferMemory");
    PFN_vkMapMemory MapMem = (PFN_vkMapMemory)gdpa(dev, "vkMapMemory");
    PFN_vkDeviceWaitIdle WaitIdle = (PFN_vkDeviceWaitIdle)gdpa(dev, "vkDeviceWaitIdle");
    PFN_vkDestroyDevice DestroyDevice = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
    if (!WaitSemaphores || !SignalSemaphore || !GetValue) {
        printf("FAIL the timeline commands: wait %p signal %p value %p\n", (void *)WaitSemaphores, (void *)SignalSemaphore, (void *)GetValue);
        return 1;
    }
    GetQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    CreatePool(dev, &pci, NULL, &pool);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* buffers: a big device one, and small mapped ones */
    VkBuffer big;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_SHARING_MODE_EXCLUSIVE};
    CreateBuffer(dev, &bci, NULL, &big);
    VkMemoryRequirements mr;
    BufReq(dev, big, &mr);
    uint32_t t = 0;
    while (t < mp.memoryTypeCount && !((mr.memoryTypeBits >> t) & 1)) t++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    VkDeviceMemory bigm;
    AllocMem(dev, &mai, NULL, &bigm);
    BindBuf(dev, big, bigm, 0);
    mapped_buf mb[3];
    for (int i = 0; i < 3; i++) {
        VkBufferCreateInfo sci = bci;
        sci.size = 4096;
        CreateBuffer(dev, &sci, NULL, &mb[i].b);
        BufReq(dev, mb[i].b, &mr);
        uint32_t ht = 0;
        while (ht < mp.memoryTypeCount &&
               (!((mr.memoryTypeBits >> ht) & 1) ||
                (mp.memoryTypes[ht].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
            ht++;
        VkMemoryAllocateInfo hai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, ht};
        AllocMem(dev, &hai, NULL, &mb[i].m);
        BindBuf(dev, mb[i].b, mb[i].m, 0);
        MapMem(dev, mb[i].m, 0, VK_WHOLE_SIZE, 0, (void **)&mb[i].p);
        memset(mb[i].p, 0, 4096);
    }

    /* 2 */
    VkSemaphoreTypeCreateInfo ty = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 5};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ty};
    VkSemaphore s, s2;
    r = CreateSem(dev, &sci, NULL, &s);
    ty.initialValue = 0;
    CreateSem(dev, &sci, NULL, &s2);
    uint64_t v0 = value(s);
    VkSemaphoreSignalInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, s, 7};
    VkResult rs = SignalSemaphore(dev, &sig);
    uint64_t v1 = value(s);
    VkResult w7 = wait1(s, 7, 0), w8 = wait1(s, 8, 0);
    double t0 = now_ms();
    VkResult w8b = wait1(s, 8, 20000000ull);
    double dt = now_ms() - t0;
    CHECK(r == VK_SUCCESS && v0 == 5 && rs == VK_SUCCESS && v1 == 7 && w7 == VK_SUCCESS && w8 == VK_TIMEOUT && w8b == VK_TIMEOUT && dt >= 15 && dt < 1000,
          "2 initial %llu, host signal 7 -> %llu, wait 7: %d, wait 8 now: %d, wait 8 for 20 ms: %d after %.1f ms", (unsigned long long)v0,
          (unsigned long long)v1, w7, w8, w8b, dt);

    /* 3 */
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CreateFence(dev, &fci, NULL, &fence);
    VkCommandBuffer cb = cmd_fill(big, 64u << 20, 0x11111111u, 8);
    uint64_t sv = 10;
    t0 = now_ms();
    r = submit(NULL, NULL, 0, cb, &s, &sv, 1, fence);
    uint64_t right_after = value(s);
    VkResult w10 = wait1(s, 10, 5000000000ull);
    dt = now_ms() - t0;
    uint64_t v10 = value(s);
    VkResult fs = FenceStatus(dev, fence);
    /* the fence signals after the submit's last batch, a moment after the value (the sync mirror's
     * fence slot is a batch of its own after it): done within 50 ms */
    if (fs == VK_NOT_READY) fs = ((PFN_vkWaitForFences)gdpa(dev, "vkWaitForFences"))(dev, 1, &fence, VK_TRUE, 50000000ull);
    CHECK(r == VK_SUCCESS && (right_after == 7 || right_after == 10) && w10 == VK_SUCCESS && v10 == 10 && fs == VK_SUCCESS,
          "3 GPU signal 10: submit %d, value right after %llu, wait %d (%.1f ms), value %llu, app fence %d", r,
          (unsigned long long)right_after, w10, dt, (unsigned long long)v10, fs);

    /* 4 */
    VkCommandBuffer fillb = cmd_fill(mb[0].b, 4096, 0x5a5a0001u, 1), copyb = cmd_copy(mb[0].b, mb[1].b, 4096);
    {
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        uint64_t s11 = 11, w11 = 11, s12 = 12;
        VkTimelineSemaphoreSubmitInfo ta = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &s11};
        VkTimelineSemaphoreSubmitInfo tb = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &w11, 1, &s12};
        VkSubmitInfo two[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ta, 0, NULL, NULL, 1, &fillb, 1, &s},
                               {VK_STRUCTURE_TYPE_SUBMIT_INFO, &tb, 1, &s, &st, 1, &copyb, 1, &s}};
        r = QueueSubmit(queue, 2, two, VK_NULL_HANDLE);
        VkResult w12 = wait1(s, 12, 5000000000ull);
        CHECK(r == VK_SUCCESS && w12 == VK_SUCCESS && value(s) == 12 && mb[1].p[0] == 0x5a5a0001u && mb[1].p[1023] == 0x5a5a0001u,
              "4 two batches in one submit, the second waiting for the first: submit %d, wait 12 %d, copied 0x%08x", r, w12, mb[1].p[0]);
    }

    /* 5 */
    {
        thr_arg ta = {s, &mb[2], VK_ERROR_UNKNOWN};
        ty.initialValue = 0;
        pthread_t th;
        pthread_create(&th, NULL, wbs_thread, &ta);
        usleep(100000);
        uint64_t before = value(s);
        uint32_t px = mb[2].p[0];
        VkSemaphoreSignalInfo s20 = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, s, 20};
        SignalSemaphore(dev, &s20);
        VkResult w21 = wait1(s, 21, 5000000000ull);
        pthread_join(th, NULL);
        CHECK(before == 12 && px == 0 && w21 == VK_SUCCESS && ta.r == VK_SUCCESS && value(s) == 21 && mb[2].p[0] == 0xabcd1234u,
              "5 wait-before-signal: value %llu and buffer 0x%08x before the host signal, wait 21 %d, the waiting submit %d, buffer 0x%08x",
              (unsigned long long)before, px, w21, ta.r, mb[2].p[0]);
    }

    /* 6 */
    {
        VkSemaphore both[2] = {s2, s};
        uint64_t vals[2] = {1, 21};
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, VK_SEMAPHORE_WAIT_ANY_BIT, 2, both, vals};
        VkResult any = WaitSemaphores(dev, &wi, 0);
        wi.flags = 0;
        VkResult all = WaitSemaphores(dev, &wi, 0);
        CHECK(any == VK_SUCCESS && all == VK_TIMEOUT, "6 wait for any of two (one reached): %d; for both: %d", any, all);
    }

    /* 7 */
    {
        VkSemaphoreCreateInfo bin = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore b;
        CreateSem(dev, &bin, NULL, &b);
        memset(mb[0].p, 0, 4096);
        memset(mb[1].p, 0, 4096);
        VkCommandBuffer f2b = cmd_fill(mb[0].b, 4096, 0x77770007u, 1), c2b = cmd_copy(mb[0].b, mb[1].b, 4096);
        VkFence f2;
        CreateFence(dev, &fci, NULL, &f2);
        uint64_t zero = 0; /* a binary semaphore's value in the structure is ignored */
        VkResult a = submit(NULL, NULL, 0, f2b, &b, &zero, 1, VK_NULL_HANDLE);
        VkResult c = submit(&b, &zero, 1, c2b, NULL, NULL, 0, f2);
        PFN_vkWaitForFences WaitFences = (PFN_vkWaitForFences)gdpa(dev, "vkWaitForFences");
        VkResult wf = WaitFences(dev, 1, &f2, VK_TRUE, 5000000000ull);
        CHECK(a == VK_SUCCESS && c == VK_SUCCESS && wf == VK_SUCCESS && mb[1].p[512] == 0x77770007u,
              "7 binary semaphores beside it: %d %d, fence %d, copied 0x%08x", a, c, wf, mb[1].p[512]);
        DestroySem(dev, b, NULL);
    }

    /* 8 */
    {
        uint64_t last = value(s), back = 0;
        VkCommandBuffer cbs[8];
        for (int i = 0; i < 8; i++) cbs[i] = cmd_fill(mb[0].b, 4096, 0x100u + i, 1);
        VkResult rr = VK_SUCCESS;
        for (uint64_t i = 1; i <= 200 && rr == VK_SUCCESS; i++) {
            uint64_t v = 21 + i;
            rr = submit(NULL, NULL, 0, cbs[i % 8], &s, &v, 1, VK_NULL_HANDLE);
            uint64_t now = value(s);
            if (now < last) back++;
            last = now;
        }
        VkResult w = wait1(s, 221, 5000000000ull);
        CHECK(rr == VK_SUCCESS && !back && w == VK_SUCCESS && value(s) == 221, "8 200 numbered submits: %d, the value went back %llu time(s), last %llu",
              rr, (unsigned long long)back, (unsigned long long)value(s));
    }

    WaitIdle(dev);
    DestroySem(dev, s, NULL);
    DestroySem(dev, s2, NULL);
    DestroyDevice(dev, NULL);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
