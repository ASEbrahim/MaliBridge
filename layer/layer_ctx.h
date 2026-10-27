/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <memory>
#include <shared_mutex>
#include <vulkan/vulkan.h>
#include <utility>

extern "C" {
#include "vkb_server.h"
}

struct LayerCtx {
    std::unique_ptr<vkb_srv_table> t;
    // one instance/pdevice is associated with several devices
    std::shared_ptr<LayerCtx> parent = nullptr;
    VkDevice device;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    vkb_srv_table *table() const noexcept { return t.get(); }
};

struct ScopedLayerContext {
    vkb_srv_table *previous;
    std::shared_ptr<LayerCtx> ctx;

    inline explicit ScopedLayerContext(std::shared_ptr<LayerCtx> ctx) noexcept
        : ctx(ctx) {
        previous = vkb_srv_current_table();
        if (ctx && ctx->t) {
            auto *t = ctx->t.get();
            if (previous != t) {
                vkb_srv_set_current_table(t);
            }
        }
    }

    inline ~ScopedLayerContext() {
        if (vkb_srv_current_table() != previous) {
            vkb_srv_set_current_table(previous);
        }
    }

    inline LayerCtx* operator->() const noexcept { return ctx.get(); }
    inline explicit operator bool() const noexcept { return ctx != nullptr; }

    ScopedLayerContext(const ScopedLayerContext &) = delete;
    ScopedLayerContext &operator=(const ScopedLayerContext &) = delete;
    ScopedLayerContext(ScopedLayerContext&&) = delete;
    ScopedLayerContext& operator=(ScopedLayerContext&&) = delete;
};

std::shared_ptr<LayerCtx> get_ctx(const void *handle);

inline ScopedLayerContext get_scoped_ctx(const void *handle) {
    return ScopedLayerContext(get_ctx(handle)); // elides away copy
}

void register_handle(const void *handle, std::shared_ptr<LayerCtx> ctx);

void unregister_handle(const void *handle);
