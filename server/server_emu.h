/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VKB_SERVER_EMU_H
#define VKB_SERVER_EMU_H

#include "vkb_server.h"

/* Emulations that are implemented (the rest are never offered to the client). */
#define VKB_EMU_IMPLEMENTED (VKB_EMU_MAINT5 | VKB_EMU_CLIP_DISTANCE | VKB_EMU_CULL_DISTANCE | VKB_EMU_POINT_SIZE | VKB_EMU_BCN | \
                             VKB_EMU_DIVISOR | VKB_EMU_DEPTH_CLIP | VKB_EMU_ROBUSTNESS2 | \
                             VKB_EMU_DMABUF_EXPORT | VKB_EMU_DUAL_SRC | VKB_EMU_FILL_MODE | VKB_EMU_MULTI_VIEWPORT | \
                             VKB_EMU_MAINT6 | VKB_EMU_LOADSTORE_NONE | VKB_EMU_PUSH_DESC | VKB_EMU_TEXEL_ALIGN | \
                             VKB_EMU_DESC_LIMITS | VKB_EMU_OPAQUE_FD | VKB_EMU_MULTI_DRAW)

#define VKB_EMU_BUCKETS 1024

typedef struct vkb_emu_image {
    VkImage image;
    VkImageType type;
    VkFormat format;          /* what the driver holds */
    VkFormat client_format;   /* what the app asked for (differs for emulated formats) */
    VkExtent3D extent;
    uint32_t mips, layers;
    void *emu;                /* per-emulation state (BCn: decode resources) */
    int64_t stat_bytes, stat_bc_src, stat_ideal; /* accounting: driver's size; BC: the original's, the decoded texels' */
    struct vkb_emu_image *next;
} vkb_emu_image;

typedef struct vkb_emu_buffer {
    VkBuffer buffer;
    VkDeviceSize size;
    struct vkb_emu_buffer *next;
} vkb_emu_buffer;

typedef struct vkb_emu_device {
    uint32_t flags;
    vkb_srv_table *dev;
    pthread_mutex_t lock;
    vkb_emu_image *images[VKB_EMU_BUCKETS];
    vkb_emu_buffer *buffers[VKB_EMU_BUCKETS];
    void *bcn;       /* server_emu_bcn.c */
    void *cmdstate;  /* server_emu_cmdstate.c */
    void *divisor;   /* server_emu_divisor.c */
    void *shaders;   /* server_emu_shader.c */
} vkb_emu_device;

/* command buffer state (server_emu_cmdstate.c) */
void vkb_emu_cmdstate_install(vkb_srv_table *dev);
void vkb_emu_cmdstate_uninstall(vkb_srv_table *dev);
void vkb_emu_restore_compute(vkb_emu_device *e, VkCommandBuffer cb);
void *vkb_emu_cb_gfx(vkb_emu_device *e, VkCommandBuffer cb, size_t size);
VkDescriptorSet vkb_emu_cb_scratch_set(vkb_emu_device *e, VkCommandBuffer cb, VkDescriptorSetLayout dsl);
void *vkb_emu_cb_bcn(vkb_emu_device *e, VkCommandBuffer cb, size_t size);
/* vertex attribute divisor (server_emu_divisor.c) */
typedef struct div_info div_info;
size_t vkb_emu_divisor_info_size(void);
void vkb_emu_divisor_pipeline(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, div_info *out);
void vkb_emu_divisor_created(vkb_emu_device *e, VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *ci,
                             VkPipeline p, div_info *info);
void vkb_emu_divisor_install(vkb_srv_table *dev);
void vkb_emu_divisor_uninstall(vkb_srv_table *dev);
void vkb_emu_gfx_bind_pipeline(vkb_emu_device *e, VkCommandBuffer cb, VkPipeline p);
void vkb_emu_gfx_reset(void *gfx);
void vkb_emu_gfx_free(void *gfx);
/* BCn (server_emu_bcn.c) */
void vkb_emu_bcn_install(vkb_srv_table *dev);
void vkb_emu_bcn_uninstall(vkb_srv_table *dev);
void vkb_emu_bcn_quiesce(vkb_srv_table *dev);
VkFormat vkb_emu_bcn_view_format(vkb_emu_device *e, VkImage image, VkFormat requested);
void vkb_emu_bcn_buffer_usage(vkb_emu_device *e, VkBufferCreateInfo *ci);
int vkb_emu_bcn_blocks_readback(vkb_emu_device *e, VkImage img);
/* The command buffer's decode scratch goes back to the device (the buffer is begun again, reset or freed). */
void vkb_emu_bcn_cb_release(vkb_emu_device *e, void *cb_scratch);
VkDeviceSize vkb_emu_bcn_scratch_live(void);
/* the startup probe builds each transcoder on the driver (server_probe.c) */
int vkb_emu_bcn_transcoders(void);
const char *vkb_emu_bcn_transcoder_name(int i, uint32_t *probe_flag);
VkResult vkb_emu_bcn_build_transcoder(const vkb_dispatch *r, VkDevice d, VkPipelineCache cache, int i, double *ms);

/* layer (server_emu_layer.c) */
void vkb_emu_depth_clip_pipeline(vkb_emu_device *e, VkGraphicsPipelineCreateInfo *ci);
void vkb_emu_depth_clip_install(vkb_srv_table *dev);
vkb_emu_device *vkb_emu_cur(void);
const vkb_dispatch *vkb_emu_real(void);
void *vkb_emu_chain_take(const void *head, VkStructureType t);
int vkb_emu_strip_opaque(const void *head, VkStructureType t);
vkb_emu_image *vkb_emu_find_image(vkb_emu_device *e, VkImage img);
void vkb_emu_track_image(vkb_emu_device *e, VkImage img, const VkImageCreateInfo *ci, VkFormat client_format);

/* feature hooks (server_emu_features.c and friends) */
int vkb_emu_bcn_supported(const vkb_dispatch *idt, VkPhysicalDevice pd);
/* BC images transcoded to ETC2 / EAC / ASTC where the GPU has them (VKBRIDGE_BC_TRANSCODE=0: decoded). */
int vkb_emu_bcn_transcode_wanted(void);
void vkb_emu_install(vkb_srv_table *dev);
void vkb_emu_uninstall(vkb_srv_table *dev);
void vkb_emu_install_features(vkb_srv_table *dev);
void vkb_emu_uninstall_features(vkb_srv_table *dev);
void vkb_emu_image_create_info(vkb_emu_device *e, VkImageCreateInfo *ci);
void vkb_emu_image_release(vkb_emu_device *e, vkb_emu_image *img);
/* Rewrites a graphics pipeline's shaders; temporary modules are appended to mods. */
void vkb_emu_shader_pipeline(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                             uint32_t *nmods, uint32_t cap);
/* DXVK's pass-through stream output GS folded into the VS (Mali's geometry shaders are slow). */
void vkb_emu_shader_xfb_fold(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                             uint32_t *nmods, uint32_t cap);
void vkb_emu_shader_install(vkb_srv_table *dev);
int vkb_emu_dumping(void);
void vkb_emu_shader_drop_second_output(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                                       uint32_t *nmods, uint32_t cap);
void vkb_emu_compat_pipeline(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods, uint32_t *nmods,
                             uint32_t cap);
void vkb_emu_compat_install(vkb_srv_table *dev);
void vkb_emu_push_install(vkb_srv_table *dev);
void vkb_emu_push_uninstall(vkb_srv_table *dev);
/* descriptor limits (server_emu_desc.c): [update after bind][sampled image, storage image, storage buffer] */
void vkb_emu_desc_limits_get(const vkb_dispatch *d, VkPhysicalDevice pd, uint32_t out[2][3]);
void vkb_emu_desc_install(vkb_srv_table *dev);
void vkb_emu_desc_uninstall(vkb_srv_table *dev);
/* 1 when VKBRIDGE_FAKE_MISSING names t (host tests imitating Mali). */
int vkb_emu_faked(const char *t);
int vkb_emu_ext_hidden(const char *name); /* VKBRIDGE_FAKE_MISSING / VKBRIDGE_SERVER_HIDE_EXTS */
int vkb_emu_hiding_exts(void);
void vkb_emu_instance_install(vkb_srv_table *t); /* clients see the device extensions without the hidden ones */
void vkb_emu_compat_uninstall(vkb_srv_table *dev);
/* OPAQUE_FD on dma-bufs (server_opaque.c) */
int vkb_opaque_wanted(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t emu);
int vkb_opaque_buffer(vkb_emu_device *e, VkBufferCreateInfo *ci);
int vkb_opaque_image(vkb_emu_device *e, VkImageCreateInfo *ci);
int vkb_opaque_retry(const void *ci, VkStructureType t);
VkResult vkb_emu_driver_module(VkDevice device, const VkShaderModuleCreateInfo *ci, VkShaderModule *out);
VkShaderModule vkb_emu_shader_zero_base_instance(vkb_emu_device *e, VkDevice device, const VkPipelineShaderStageCreateInfo *vs);
void vkb_emu_shader_uninstall(vkb_srv_table *dev);
/* 1 = handled (the emulation recorded its own commands instead). */
int vkb_emu_copy_image(vkb_emu_device *e, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                       uint32_t n, const VkImageCopy *regions);
int vkb_emu_copy_image2(vkb_emu_device *e, VkCommandBuffer cb, const VkCopyImageInfo2 *info);
int vkb_emu_copy_buffer_to_image(vkb_emu_device *e, VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                 const VkBufferImageCopy *regions);
int vkb_emu_copy_buffer_to_image2(vkb_emu_device *e, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info);

#endif
