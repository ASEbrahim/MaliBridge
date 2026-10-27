/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gs_hide (issue #11): the Galaxy S10+'s Mali-G76 with Samsung's r38p1 beta driver
 * ("v1.r38p1-01bet0") segfaults compiling the vertex + geometry + fragment pipelines Zink makes
 * for Xwayland's glamor and for Steam's update window. The bridge contains each crash, but after
 * three the device is lost; Xwayland dies of it, Steam loses its X connection, gamescope aborts.
 *
 * The fix is what those processes are shown: with the erratum on for the driver (server_errata.c:
 * r38p1 beta builds, or VKBRIDGE_ERRATA=+gs_hide) and the process named in VKBRIDGE_GS_HIDE_PROCS
 * (default "Xwayland,steam,steamwebhelper"; "*" every process; set and empty: none), the feature
 * queries answer geometryShader off (and transform feedback's geometryStreams with it, which needs
 * it), the property queries the geometry limits 0 and no geometry stage among the subgroup
 * stages, and a vkCreateDevice that enables geometryShader anyway has it taken off (logged). Zink
 * then draws GL_QUADS and the rest without generated geometry shaders, and caps itself below
 * OpenGL 3.2 - glamor and Steam's windows need far less. Games are other processes: they keep
 * geometry shaders (DXVK requires geometryShader), so a game's own geometry shaders still reach
 * this driver - the crash containment is all they have, as before.
 *
 * The process is told by its name in the hello (the process's comm, server_conn.c), at
 * vkCreateInstance: the wrappers are installed only on the listed processes' instances, so no
 * other process pays for them. They go over every other query wrapper (installed last), so what
 * the promotion and the vertex input emulation add stays and this takes off.
 *
 * Imitation (host): VKBRIDGE_TEST_CRASH_PIPELINE=gs faults inside every pipeline creation with a
 * geometry stage (server_guard.c), as the beta driver does; VKBRIDGE_FAKE_DRIVER_VERSION=
 * v1.r38p1-01bet0 puts the desktop driver in the range (host/gshide_test.sh).
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PROCS "Xwayland,steam,steamwebhelper"

static const char *procs_list(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_GS_HIDE_PROCS");
    return v ? v : DEFAULT_PROCS;
}

static int in_list(const char *list, const char *name)
{
    size_t nl = strlen(name);
    for (const char *c = list; c && *c;) {
        while (*c == ',' || *c == ' ') c++;
        size_t len = strcspn(c, ", ");
        if (len == 1 && *c == '*') return 1;
        if (len && len == nl && !strncmp(c, name, len)) return 1;
        c += len;
    }
    return 0;
}

int vkb_errata_gs_process(const vkb_srv_proc *p)
{
    return p && p->comm[0] && in_list(procs_list(), p->comm);
}

int vkb_errata_gs_hidden(const vkb_srv_table *t, VkPhysicalDevice pd, char *why, size_t cap)
{
    if (why && cap) why[0] = 0;
    if (!t || !vkb_errata_gs_process(t->proc)) return 0;
    return vkb_errata_gs_driver(t, pd, why, cap);
}

/* ------------------------------------------------------------------ what the process is shown */

static int features_off(VkPhysicalDeviceFeatures *f, void *chain)
{
    int n = 0;
    if (f && f->geometryShader) f->geometryShader = VK_FALSE, n++;
    for (VkBaseOutStructure *b = chain; b; b = b->pNext) switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
            ((VkPhysicalDeviceTransformFeedbackFeaturesEXT *)b)->geometryStreams = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
            ((VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *)b)->primitivesGeneratedQueryWithNonZeroStreams = VK_FALSE;
            break;
        }
    return n;
}

static void limits_off(VkPhysicalDeviceLimits *l)
{
    l->maxGeometryShaderInvocations = l->maxGeometryInputComponents = l->maxGeometryOutputComponents = 0;
    l->maxGeometryOutputVertices = l->maxGeometryTotalOutputComponents = 0;
}

static void properties_off(VkPhysicalDeviceProperties2 *p)
{
    limits_off(&p->properties.limits);
    for (VkBaseOutStructure *b = p->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES)
            ((VkPhysicalDeviceVulkan11Properties *)b)->subgroupSupportedStages &= ~VK_SHADER_STAGE_GEOMETRY_BIT;
        else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES)
            ((VkPhysicalDeviceSubgroupProperties *)b)->supportedStages &= ~VK_SHADER_STAGE_GEOMETRY_BIT;
        else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT) {
            VkPhysicalDeviceTransformFeedbackPropertiesEXT *x = (void *)b;
            if (x->maxTransformFeedbackStreams > 1) x->maxTransformFeedbackStreams = 1;
            x->transformFeedbackStreamsLinesTriangles = VK_FALSE;
        }
    }
}

static const vkb_srv_table *query_inst(void)
{
    const vkb_srv_table *t = vkb_srv_current_table();
    return t && t->is_device ? t->parent : t;
}

static int hidden(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    char why[320];
    if (!vkb_errata_gs_driver(t, pd, why, sizeof(why))) return 0;
    vkb_errata_info_once("errata: gs_hide: geometry shaders hidden from %s (%s) - issue #11", t->proc ? t->proc->comm : "?", why);
    return 1;
}

static VKAPI_ATTR void VKAPI_CALL gs_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->gs_f1) return;
    t->gs_f1(pd, f);
    if (hidden(t, pd)) features_off(f, NULL);
}

static VKAPI_ATTR void VKAPI_CALL gs_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->gs_f2) return;
    t->gs_f2(pd, f);
    if (hidden(t, pd)) features_off(&f->features, f->pNext);
}

static VKAPI_ATTR void VKAPI_CALL gs_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->gs_p1) return;
    t->gs_p1(pd, p);
    if (hidden(t, pd)) limits_off(&p->limits);
}

static VKAPI_ATTR void VKAPI_CALL gs_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->gs_p2) return;
    t->gs_p2(pd, p);
    if (hidden(t, pd)) properties_off(p);
}

void vkb_errata_gs_instance_install(vkb_srv_table *t)
{
    if (!vkb_errata_gs_process(t->proc)) return;
    /* the driver half is asked per query (per physical device); a driver outside the range costs
     * these processes a properties query per feature query, nothing else */
    if (t->dt.vkGetPhysicalDeviceFeatures) t->gs_f1 = t->dt.vkGetPhysicalDeviceFeatures, t->dt.vkGetPhysicalDeviceFeatures = gs_GetPhysicalDeviceFeatures;
    if (t->dt.vkGetPhysicalDeviceFeatures2) t->gs_f2 = t->dt.vkGetPhysicalDeviceFeatures2, t->dt.vkGetPhysicalDeviceFeatures2 = gs_GetPhysicalDeviceFeatures2;
    if (t->dt.vkGetPhysicalDeviceProperties)
        t->gs_p1 = t->dt.vkGetPhysicalDeviceProperties, t->dt.vkGetPhysicalDeviceProperties = gs_GetPhysicalDeviceProperties;
    if (t->dt.vkGetPhysicalDeviceProperties2)
        t->gs_p2 = t->dt.vkGetPhysicalDeviceProperties2, t->dt.vkGetPhysicalDeviceProperties2 = gs_GetPhysicalDeviceProperties2;
}

void vkb_errata_gs_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci)
{
    char why[320];
    if (!vkb_errata_gs_hidden(inst, pd, why, sizeof(why))) return;
    /* the request's arena holds the structures: ours to change */
    VkPhysicalDeviceFeatures *f = (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b && !f; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) f = &((VkPhysicalDeviceFeatures2 *)b)->features;
    if (features_off(f, (void *)ci->pNext))
        VKB_INFO("errata: gs_hide: %s enabled geometryShader anyway; taken off its device (%s) - issue #11", inst->proc->comm, why);
}

void vkb_errata_gs_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)e;
    const vkb_srv_proc *p = t->proc;
    if (vkb_errata_gs_process(p))
        vkb_errata_info_once("errata: gs_hide: %s is shown no geometry shaders on this driver (Zink draws without them)", p->comm);
    else
        vkb_errata_info_once("errata: gs_hide: %s keeps geometry shaders (not in VKBRIDGE_GS_HIDE_PROCS=%s)", p && p->comm[0] ? p->comm : "?",
                             procs_list());
}
