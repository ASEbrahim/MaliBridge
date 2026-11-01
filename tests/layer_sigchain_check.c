/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * layer_sigchain_check: the layer's fault handler (layer/layer_crash.cpp) leaves the process's own
 * faults to the process. Like Wine or box64, this program installs its own SIGSEGV handler first;
 * then a device is made through the layer (which installs its handler over it), and a fault
 * outside any driver compile must still reach the program's handler, with its siginfo, once per
 * fault, three times.
 * Prints PASS/FAIL lines; exit status 0 when all pass.
 *
 *   cc -O1 -Ithird_party/Vulkan-Headers-1.4.341/include -o host/out/layer_sigchain_check \
 *      tests/layer_sigchain_check.c -ldl
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static sigjmp_buf jb;
static volatile sig_atomic_t hits;
static void *volatile fault_addr;

static void own_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig, (void)uc;
    hits++;
    fault_addr = si ? si->si_addr : NULL;
    siglongjmp(jb, 1);
}

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = own_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "layer_sigchain_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (create_instance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL vkCreateInstance\n");
        return 1;
    }
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
    PFN_vkDestroyInstance destroy_instance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    VkResult r = enum_pd(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
        printf("FAIL no physical device\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    check(create_device(pd, &dci, NULL, &dev) == VK_SUCCESS, "device made through the layer");

    struct sigaction now;
    sigaction(SIGSEGV, NULL, &now);
    check((now.sa_flags & SA_SIGINFO) && now.sa_sigaction != own_handler, "the layer's handler is installed over the program's");

    for (int i = 1; i <= 3; i++) {
        if (!sigsetjmp(jb, 1)) {
            *(volatile int *)(uintptr_t)(0x100 * i) = 1; /* outside any guarded call */
            check(0, "the fault returned");
        } else {
            char what[120];
            snprintf(what, sizeof(what), "fault %d reached the program's own handler (address %p, %d call(s))", i, fault_addr, (int)hits);
            check(hits == i && fault_addr == (void *)(uintptr_t)(0x100 * i), what);
        }
    }

    destroy_device(dev, NULL);
    destroy_instance(inst, NULL);
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
