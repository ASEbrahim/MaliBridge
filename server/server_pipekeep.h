/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Kept graphics pipeline create infos, for layers that make pipeline variants after the app's own
 * vkCreateGraphicsPipelines has returned (server_pipekeep.c).
 */
#ifndef VKB_SERVER_PIPEKEEP_H
#define VKB_SERVER_PIPEKEEP_H

#include "vkb_server.h"

typedef struct vkb_pk vkb_pk;       /* one layer's store: shader code and the objects kept create infos name */
typedef struct vkb_pk_ci vkb_pk_ci; /* one kept create info */

/* A store whose own calls (variant compiles, deferred destroys) go to *next, the layer's next. */
vkb_pk *vkb_pk_create(const vkb_dispatch *next);
/* At device destroy: the records are freed; the objects they name are left to the device. */
void vkb_pk_destroy(vkb_pk *s);

/* The layer's vkCreateShaderModule (after next made it) and vkDestroyShaderModule (before). */
void vkb_pk_module_created(vkb_pk *s, VkShaderModule m, const VkShaderModuleCreateInfo *ci);
void vkb_pk_module_destroyed(vkb_pk *s, VkShaderModule m);
/* The layer's vkDestroyPipelineLayout / vkDestroyRenderPass: 1 when a kept create info still
 * names it (it is destroyed with the last of them), 0 when the layer is to pass the call on. */
int vkb_pk_layout_destroy(vkb_pk *s, VkPipelineLayout l);
int vkb_pk_render_pass_destroy(vkb_pk *s, VkRenderPass rp);

/* ci kept, encoded, with its shaders' SPIR-V (NULL: a module the store did not see made, or a
 * create info that does not encode). Derivative state is not kept. */
vkb_pk_ci *vkb_pk_keep(vkb_pk *s, VkDevice dev, const VkGraphicsPipelineCreateInfo *ci);
void vkb_pk_release(vkb_pk *s, vkb_pk_ci *k);
size_t vkb_pk_size(const vkb_pk_ci *k); /* bytes held (create info and shader code) */

/* A pipeline made from k through next, with no pipeline cache (the persistent one beneath takes
 * it). edit gets the decoded create info first, its stages' modules already replaced by fresh
 * ones; it may change the create info in place and take new structures from arena. A nonzero
 * return from edit fails the make with that VkResult. */
typedef VkResult (*vkb_pk_edit)(VkGraphicsPipelineCreateInfo *ci, vkb_arena *arena, void *ctx);
VkResult vkb_pk_make(vkb_pk *s, const vkb_pk_ci *k, vkb_pk_edit edit, void *ctx, VkPipeline *out);

#endif
