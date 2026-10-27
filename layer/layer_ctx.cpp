/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "layer_ctx.h"

#include <memory>
#include <shared_mutex>
#include <vulkan/vulkan.h>

#include <mutex>
#include <pthread.h>
#include <shared_mutex>
#include <unordered_map>

std::shared_mutex g_handle_mutex;
std::unordered_map<void *, std::shared_ptr<LayerCtx>> g_handle_map;

static inline void *to_key(const void *handle) {
    return const_cast<void *>(handle);
}

std::shared_ptr<LayerCtx> get_ctx(const void *handle) {
    if (!handle)
        return nullptr;
    std::shared_lock lock(g_handle_mutex); // reader
    auto it = g_handle_map.find(to_key(handle));
    return (it != g_handle_map.end()) ? it->second : nullptr;
}

void register_handle(const void *handle, std::shared_ptr<LayerCtx> ctx) {
    if (!handle || !ctx)
        return;
    std::unique_lock lock(g_handle_mutex); // writer
    g_handle_map[to_key(handle)] = ctx;
}

void unregister_handle(const void *handle) {
    if (!handle)
        return;
    std::unique_lock lock(g_handle_mutex); // writer
    g_handle_map.erase(to_key(handle));
}
