/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The handle registry and the scoped context of the layer.
 */
#include <catch2/catch_all.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "layer_ctx.h"

namespace {

void *handle_at(uintptr_t address) { return reinterpret_cast<void *>(address); }

} // namespace

TEST_CASE("a registered handle gives its context back", "[layer_ctx]") {
    auto ctx = std::make_shared<LayerCtx>();
    void *handle = handle_at(0x1000);

    register_handle(handle, ctx);
    REQUIRE(ctx.use_count() == 2); // ours and the registry's

    auto got = get_ctx(handle);
    REQUIRE(got != nullptr);
    REQUIRE(got.get() == ctx.get());

    unregister_handle(handle);
    REQUIRE(get_ctx(handle) == nullptr);
}

TEST_CASE("a handle without a context is not registered", "[layer_ctx]") {
    void *handle = handle_at(0x1100);

    register_handle(handle, nullptr);

    REQUIRE(get_ctx(handle) == nullptr);
}

TEST_CASE("an unknown handle has no context", "[layer_ctx]") {
    REQUIRE(get_ctx(handle_at(0x1200)) == nullptr);
    REQUIRE(get_ctx(nullptr) == nullptr);
}

TEST_CASE("registering a handle again replaces its context", "[layer_ctx]") {
    auto first = std::make_shared<LayerCtx>();
    auto second = std::make_shared<LayerCtx>();
    void *handle = handle_at(0x1300);

    register_handle(handle, first);
    register_handle(handle, second);

    REQUIRE(get_ctx(handle).get() == second.get());

    unregister_handle(handle);
}

TEST_CASE("unregistering a handle that is not registered does nothing",
          "[layer_ctx]") {
    void *handle = handle_at(0x1400);

    unregister_handle(handle);

    REQUIRE(get_ctx(handle) == nullptr);
}

TEST_CASE("the context outlives its registration", "[layer_ctx]") {
    auto ctx = std::make_shared<LayerCtx>();
    void *handle = handle_at(0x1500);

    register_handle(handle, ctx);
    auto got = get_ctx(handle);
    unregister_handle(handle);

    REQUIRE(got != nullptr);
    REQUIRE(ctx.use_count() == 2); // ours and the one we got
    REQUIRE(got.use_count() == 2);
}

TEST_CASE("each handle has its own context", "[layer_ctx]") {
    auto first = std::make_shared<LayerCtx>();
    auto second = std::make_shared<LayerCtx>();
    void *a = handle_at(0x1600);
    void *b = handle_at(0x1700);

    register_handle(a, first);
    register_handle(b, second);

    REQUIRE(get_ctx(a).get() == first.get());
    REQUIRE(get_ctx(b).get() == second.get());

    unregister_handle(a);
    unregister_handle(b);
}

TEST_CASE("a scoped context makes its table the current one", "[layer_ctx]") {
    auto ctx = std::make_shared<LayerCtx>();
    ctx->t = std::make_unique<vkb_srv_table>();
    vkb_srv_table *table = ctx->t.get();
    vkb_srv_table *before = vkb_srv_current_table();

    {
        ScopedLayerContext scoped(ctx);
        REQUIRE(scoped.ctx != nullptr);
        REQUIRE(scoped.ctx.get() == ctx.get());
        REQUIRE(static_cast<bool>(scoped));
        REQUIRE(vkb_srv_current_table() == table);
    }

    REQUIRE(vkb_srv_current_table() == before);
}

TEST_CASE("a scoped context without a context leaves the current table alone",
          "[layer_ctx]") {
    vkb_srv_table *before = vkb_srv_current_table();

    ScopedLayerContext scoped(nullptr);

    REQUIRE(!scoped);
    REQUIRE(vkb_srv_current_table() == before);
}

TEST_CASE("a scoped context without a table leaves the current table alone",
          "[layer_ctx]") {
    auto ctx = std::make_shared<LayerCtx>(); // empty table
    vkb_srv_table *before = vkb_srv_current_table();

    ScopedLayerContext scoped(ctx);

    REQUIRE(static_cast<bool>(scoped));
    REQUIRE(vkb_srv_current_table() == before);
}

TEST_CASE("nested scoped contexts restore the tables in order", "[layer_ctx]") {
    auto outer = std::make_shared<LayerCtx>();
    outer->t = std::make_unique<vkb_srv_table>();
    auto inner = std::make_shared<LayerCtx>();
    inner->t = std::make_unique<vkb_srv_table>();
    vkb_srv_table *before = vkb_srv_current_table();

    {
        ScopedLayerContext scoped_outer(outer);
        REQUIRE(vkb_srv_current_table() == outer->t.get());
        {
            ScopedLayerContext scoped_inner(inner);
            REQUIRE(vkb_srv_current_table() == inner->t.get());
        }
        REQUIRE(vkb_srv_current_table() == outer->t.get());
    }

    REQUIRE(vkb_srv_current_table() == before);
}

TEST_CASE("a scoped context is neither copyable nor movable", "[layer_ctx]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<ScopedLayerContext>);
    STATIC_REQUIRE(!std::is_copy_assignable_v<ScopedLayerContext>);
    STATIC_REQUIRE(!std::is_move_constructible_v<ScopedLayerContext>);
    STATIC_REQUIRE(!std::is_move_assignable_v<ScopedLayerContext>);
}

TEST_CASE("the handle registry is thread-safe", "[layer_ctx]") {
    constexpr int kThreads = 8;
    constexpr int kHandles = 128;
    std::atomic<int> mismatches{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([t, &mismatches] {
            for (int i = 0; i < kHandles; i++) {
                void *handle =
                    handle_at(0x10000 + static_cast<uintptr_t>(t) * 1000 + i);
                auto ctx = std::make_shared<LayerCtx>();
                register_handle(handle, ctx);
                auto got = get_ctx(handle);
                if (!got || got.get() != ctx.get())
                    mismatches++;
                unregister_handle(handle);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();

    REQUIRE(mismatches == 0);
}
