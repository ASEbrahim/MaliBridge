# Host (x86_64 Linux) build of both bridge halves, for testing against the desktop's own Vulkan
# driver: the server loads libvulkan.so.1 (pick a driver with VK_DRIVER_FILES in ITS environment),
# the client ICD is what test programs load (VK_DRIVER_FILES=$(OUT)/vkbridge_icd.json).
#
# This is the CMake equivalent of host/Makefile (which stays for the moment):
#   cmake -S host -B host/build && cmake --build host/build -j 8
#   cmake --build host/build --target test        # run the regression suite (run_tests.sh)
#
# The programs land in host/out by default, where the host/*.sh scripts look for them. Move them
# with -DOUT=<dir>; -DTSAN=ON builds with ThreadSanitizer into host/out-tsan. `clean` removes the
# build tree's objects; `clean-out` removes $(OUT) as the Makefile's `clean` does.
#
# The check scripts have targets too: `hook-list-test`, `quick-check`, `ci-test`, `layer-check`,
# `layer-quick-check` and `layer-errata-test`.
cmake_minimum_required(VERSION 3.19)
project(vkbridge_host C)

get_filename_component(VKB "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SRV "${VKB}/server")

option(TSAN "Build with ThreadSanitizer" OFF)
option(LAYER "Also build the layer (the top-level project) into $(OUT)" ON)

if(NOT DEFINED OUT)
    if(TSAN)
        set(_out_default "${CMAKE_CURRENT_SOURCE_DIR}/out-tsan")
    else()
        set(_out_default "${CMAKE_CURRENT_SOURCE_DIR}/out")
    endif()
    set(OUT "${_out_default}" CACHE PATH "Output directory for the host programs")
endif()
file(MAKE_DIRECTORY "${OUT}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_C_FLAGS)
    set(CMAKE_C_FLAGS "-O1 -g" CACHE STRING "C compiler flags" FORCE)
endif()

find_package(Threads REQUIRED)

# The bridge build named in the startup probe's device profile and the session report.
execute_process(
    COMMAND git -C "${VKB}" describe --always --dirty
    OUTPUT_VARIABLE VKB_GIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)
if(NOT GIT_RESULT EQUAL 0 OR NOT VKB_GIT)
    set(VKB_GIT "unknown")
endif()

set(WARN
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-sign-compare
)
set(SAN)
if(TSAN)
    set(SAN -fsanitize=thread -g)
endif()

add_library(vkb_host_flags INTERFACE)
target_include_directories(vkb_host_flags INTERFACE
    "${VKB}/third_party/Vulkan-Headers-1.4.341/include"
    "${VKB}/common"
    "${VKB}/common/gen"
)
target_compile_options(vkb_host_flags INTERFACE ${WARN} ${SAN})
target_link_options(vkb_host_flags INTERFACE ${SAN})

add_library(vkb_host_warn INTERFACE)
target_compile_options(vkb_host_warn INTERFACE ${WARN} ${SAN})
target_link_options(vkb_host_warn INTERFACE ${SAN})

# Put a target's artifacts in $(OUT), for single- and multi-config generators alike.
function(vkb_host_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${OUT}"
        LIBRARY_OUTPUT_DIRECTORY "${OUT}"
    )
    foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg} "${OUT}"
            LIBRARY_OUTPUT_DIRECTORY_${cfg} "${OUT}"
        )
    endforeach()
endfunction()

set(COMMON_SRC
    "${VKB}/common/vkb_wire.c"
    "${VKB}/common/gen/vkb_gen_structs.c"
)
set(CAPS_SRC "${VKB}/common/vkb_caps.c")

file(GLOB SERVER_SRC CONFIGURE_DEPENDS "${SRV}/*.c")
file(GLOB CLIENT_SRC CONFIGURE_DEPENDS "${VKB}/client/*.c")

add_executable(vkbridge-server
    ${SERVER_SRC}
    "${VKB}/common/gen/vkb_gen_server.c"
    ${COMMON_SRC}
    ${CAPS_SRC}
)
target_link_libraries(vkbridge-server PRIVATE
    vkb_host_flags
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)
target_include_directories(vkbridge-server PRIVATE "${SRV}")
target_compile_definitions(vkbridge-server PRIVATE VKB_GIT="${VKB_GIT}")
target_link_options(vkbridge-server PRIVATE -rdynamic)
vkb_host_output(vkbridge-server)

add_library(vulkan_droidbridge SHARED
    ${CLIENT_SRC}
    "${VKB}/common/gen/vkb_gen_client.c"
    ${COMMON_SRC}
    ${CAPS_SRC}
)
target_link_libraries(vulkan_droidbridge PRIVATE
    vkb_host_flags
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_include_directories(vulkan_droidbridge PRIVATE
    "${VKB}/client"
    "${VKB}/client/compat"
)
target_compile_definitions(vulkan_droidbridge PRIVATE VKB_CLIENT)
set_target_properties(vulkan_droidbridge PROPERTIES
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME "vulkan_droidbridge"
)
target_link_options(vulkan_droidbridge PRIVATE
    -Wl,-z,nodelete
    -Wl,--no-undefined
)
vkb_host_output(vulkan_droidbridge)

# The ICD manifest points at the library next to it (the scripts set VK_DRIVER_FILES to this file).
file(GENERATE
    OUTPUT "${OUT}/vkbridge_icd.json"
    CONTENT "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"${OUT}/libvulkan_droidbridge.so\",\"api_version\":\"1.4.341\"}}\n"
)

# The test programs load whatever Vulkan driver VK_DRIVER_FILES names (the bridge or a native one).
add_executable(vkbridge-smoke
    "${VKB}/tests/smoke.c"
    "${VKB}/tests/smoke_spv.h"
)
target_link_libraries(vkbridge-smoke PRIVATE vkb_host_flags ${CMAKE_DL_LIBS})
target_include_directories(vkbridge-smoke PRIVATE "${VKB}/tests")
vkb_host_output(vkbridge-smoke)

# Unsigned char as on the AArch64 guest the checks also run on: x86's signed char hid bcdec's
# plain-char sign extension of BC4/BC5 SNORM endpoints, which failed bcn_check on the tablet.
set(CHECK_TESTS
    bcn_check
    opaque_check
    dmabuf_image_check
    recycle_check
    desc_traffic_check
    query_check
    placed_check
    crash_contain_check
    desc_limits_check
    nulldesc_check
    pcache_check
    divisor_check
    gsfold_check
    view3d_check
    viewchurn_check
    wait_check
    bitwise_check
    dynstate_check
    vbstride_check
    viewport_check
    clampzero_check
    begin_check
    cullmode_check
    dedimport_check
    cached_check
    polygon_check
    gpuview_check
    dedhint_check
    planar_check
    fdprops_check
    memclass_check
    procaddr_check
    exitclean_check
    texcap_check
    asyncpipe_check
    robust2_check
    fp16_check
    promote_check
    shmbarrier_check
    stencillayout_check
    dynrend_check
    wave32_check
    xfb_check
)
foreach(t IN LISTS CHECK_TESTS)
    add_executable(${t}
        "${VKB}/tests/${t}.c"
        "${VKB}/tests/smoke_spv.h"
    )
    target_link_libraries(${t} PRIVATE vkb_host_flags ${CMAKE_DL_LIBS} m)
    target_include_directories(${t} PRIVATE "${VKB}/tests")
    target_compile_options(${t} PRIVATE -funsigned-char)
    vkb_host_output(${t})
endforeach()

target_sources(bcn_check PRIVATE "${SRV}/bc_spv.h")
target_sources(divisor_check PRIVATE
    "${VKB}/tests/divisor_vert.inc"
    "${VKB}/tests/divisor_frag.inc"
)
target_sources(vbstride_check PRIVATE
    "${VKB}/tests/divisor_vert.inc"
    "${VKB}/tests/divisor_frag.inc"
)
target_sources(dynstate_check PRIVATE
    "${VKB}/tests/dynstate_vert.inc"
    "${VKB}/tests/dynstate_frag.inc"
)
target_sources(cullmode_check PRIVATE
    "${VKB}/tests/cullmode_vert.inc"
    "${VKB}/tests/dynstate_frag.inc"
)
target_sources(gsfold_check PRIVATE
    "${VKB}/tests/gsfold_vert.inc"
    "${VKB}/tests/gsfold_geom.inc"
    "${VKB}/tests/gsfold_frag.inc"
    "${VKB}/tests/gsfold_lut.inc"
)
target_sources(bitwise_check PRIVATE "${VKB}/tests/bitwise_comp.inc")
target_sources(polygon_check PRIVATE
    "${VKB}/tests/polygon_vert.inc"
    "${VKB}/tests/polygon_frag.inc"
)
target_sources(clampzero_check PRIVATE "${VKB}/tests/clampzero_comp.inc")
target_sources(texcap_check PRIVATE "${VKB}/tests/texcap_comp.inc")
target_sources(asyncpipe_check PRIVATE
    "${VKB}/tests/cullmode_vert.inc"
    "${VKB}/tests/dynstate_frag.inc"
)
target_sources(dynrend_check PRIVATE
    "${VKB}/tests/polygon_vert.inc"
    "${VKB}/tests/polygon_frag.inc"
)
target_sources(promote_check PRIVATE
    "${VKB}/tests/promote_comp15.inc"
    "${VKB}/tests/promote_comp16.inc"
)
target_sources(shmbarrier_check PRIVATE "${VKB}/tests/shmbarrier_comp.inc")
target_sources(wave32_check PRIVATE "${VKB}/tests/wave32_comp.inc")
target_sources(xfb_check PRIVATE
    "${VKB}/tests/xfb_xfb_vert.inc"
    "${VKB}/tests/xfb_plain_vert.inc"
    "${VKB}/tests/xfb_so_geom.inc"
)
target_sources(viewport_check PRIVATE
    "${VKB}/tests/viewport_vert.inc"
    "${VKB}/tests/viewport_vert_mv.inc"
    "${VKB}/tests/viewport_geom.inc"
    "${VKB}/tests/viewport_frag.inc"
)

# DXVK's frame structure: it submits from another thread, so it needs pthread.
add_executable(flicker_check
    "${VKB}/tests/flicker_check.c"
    "${VKB}/tests/smoke_spv.h"
)
target_link_libraries(flicker_check PRIVATE
    vkb_host_flags
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)
target_include_directories(flicker_check PRIVATE "${VKB}/tests")
target_compile_options(flicker_check PRIVATE -funsigned-char)
vkb_host_output(flicker_check)

# The layer's tests (host/layer_check.sh): a minimal app, and the BCn emulation.
add_executable(layer-app "${VKB}/tests/layer_app.c")
target_link_libraries(layer-app PRIVATE vkb_host_flags vulkan)
vkb_host_output(layer-app)

# The MK-02 SPIR-V pass alone, over a corpus (host/viewport_test.sh)
add_executable(flip_corpus
    "${VKB}/tests/flip_corpus.c"
    "${SRV}/spirv_flip.c"
    "${SRV}/spirv_edit.c"
)
target_link_libraries(flip_corpus PRIVATE vkb_host_warn)
vkb_host_output(flip_corpus)

# The MK-03 SPIR-V pass alone, over a corpus (host/bitwise_test.sh)
add_executable(bitwise_corpus
    "${VKB}/tests/bitwise_corpus.c"
    "${SRV}/spirv_bitwise.c"
    "${SRV}/spirv_opclass.inc"
)
target_link_libraries(bitwise_corpus PRIVATE vkb_host_warn)
vkb_host_output(bitwise_corpus)

# The MK-32 SPIR-V pass alone, over a corpus (host/clampzero_test.sh)
add_executable(clampzero_corpus
    "${VKB}/tests/clampzero_corpus.c"
    "${SRV}/spirv_clampzero.c"
    "${SRV}/spirv_edit.c"
    "${SRV}/spirv_opclass.inc"
)
target_link_libraries(clampzero_corpus PRIVATE vkb_host_warn)
vkb_host_output(clampzero_corpus)

# The shl_barrier SPIR-V pass alone, over a corpus (host/shlbarrier_test.sh)
add_executable(shlbarrier_corpus
    "${VKB}/tests/shlbarrier_corpus.c"
    "${SRV}/spirv_shlbarrier.c"
)
target_link_libraries(shlbarrier_corpus PRIVATE vkb_host_warn)
vkb_host_output(shlbarrier_corpus)

# The version promotion's SPIR-V lowering alone, over a corpus (host/promote_test.sh)
add_executable(downgrade_corpus
    "${VKB}/tests/downgrade_corpus.c"
    "${SRV}/spirv_downgrade.c"
    "${SRV}/spirv_edit.c"
)
target_link_libraries(downgrade_corpus PRIVATE vkb_host_warn)
vkb_host_output(downgrade_corpus)

# The shm_barrier SPIR-V pass alone, over a corpus (host/shmbarrier_test.sh)
add_executable(shmbarrier_corpus
    "${VKB}/tests/shmbarrier_corpus.c"
    "${SRV}/spirv_shmbarrier.c"
    "${SRV}/spirv_uniform.c"
    "${SRV}/spirv_lockstep.c"
)
target_link_libraries(shmbarrier_corpus PRIVATE vkb_host_warn)
vkb_host_output(shmbarrier_corpus)

# The wave32 SPIR-V pass alone, over a corpus (host/wave32_test.sh)
add_executable(wave32_corpus
    "${VKB}/tests/wave32_corpus.c"
    "${SRV}/spirv_wave32.c"
    "${SRV}/spirv_uniform.c"
)
target_link_libraries(wave32_corpus PRIVATE vkb_host_warn)
vkb_host_output(wave32_corpus)

# The asynchronous compile's shader scan alone (host/asyncpipe_test.sh)
add_executable(effects_corpus
    "${VKB}/tests/effects_corpus.c"
    "${SRV}/spirv_effects.c"
    "${VKB}/tests/bitwise_comp.inc"
    "${VKB}/tests/clampzero_comp.inc"
    "${VKB}/tests/dynstate_frag.inc"
    "${VKB}/tests/cullmode_vert.inc"
    "${VKB}/tests/divisor_vert.inc"
    "${VKB}/tests/divisor_frag.inc"
)
target_include_directories(effects_corpus PRIVATE "${SRV}" "${VKB}/tests")
target_link_libraries(effects_corpus PRIVATE vkb_host_warn)
vkb_host_output(effects_corpus)

# `make -C host test` runs the whole regression suite; so does `cmake --build <dir> --target test`.
add_custom_target(test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" "${OUT}"
    DEPENDS
        vkbridge-server
        vulkan_droidbridge
        vkbridge-smoke
        flicker_check
        layer-app
        ${CHECK_TESTS}
        flip_corpus
        bitwise_corpus
        clampzero_corpus
        shlbarrier_corpus
        downgrade_corpus
        shmbarrier_corpus
        wave32_corpus
        effects_corpus
    USES_TERMINAL
    VERBATIM
)

# The layer's target, sources, flags and manifest are the top-level project's (the layer needs the
# server's objects and the C++ runtime): this only builds it there and stages the result in $(OUT).
set(LAYER_BUILD_DIR "${VKB}/build-linux-x86-64" CACHE PATH
    "The layer's build tree (the top-level CMake project)")

if(LAYER)
    add_custom_target(vkb_layer ALL
        COMMAND "${CMAKE_COMMAND}"
                "-DVKB=${VKB}"
                "-DLAYER_BUILD_DIR=${LAYER_BUILD_DIR}"
                "-DOUT=${OUT}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/build_layer.cmake"
        COMMENT "Building the layer (top-level CMakeLists.txt) into ${OUT}"
        USES_TERMINAL
        VERBATIM
    )
    set(LAYER_OUT "${OUT}")
    set(LAYER_DEP vkb_layer)
else()
    set(LAYER_OUT "${LAYER_BUILD_DIR}")
    set(LAYER_DEP)
endif()

# These targets only run the scripts, which take the directory holding the layer.
#
# quick-check, ci-test and layer-check use the scripts' own host/out (they take no output
# directory), so they expect the default -DOUT; layer-errata-test honours $(OUT) through VKB_OUT.

# host/hook_list_test.sh: the cached server hook list against the scanner (no build needed).
add_custom_target(hook-list-test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/hook_list_test.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
)

# host/quick_check.sh [plain|mali|both]
add_custom_target(quick-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/quick_check.sh"
    DEPENDS
        vkbridge-server
        vulkan_droidbridge
        vkbridge-smoke
        procaddr_check
        planar_check
        divisor_check
        desc_limits_check
        memclass_check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
)

# host/run_ci_tests.sh: the headless subset of the regression suite (the CI's run).
add_custom_target(ci-test
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run_ci_tests.sh"
    DEPENDS
        vkbridge-server
        vulkan_droidbridge
        vkbridge-smoke
        bcn_check
        nulldesc_check
        opaque_check
        planar_check
        memclass_check
        procaddr_check
        recycle_check
        desc_traffic_check
        flicker_check
        view3d_check
        viewchurn_check
        dynstate_check
        vbstride_check
        dedhint_check
        cullmode_check
        divisor_check
        polygon_check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
)

# host/layer_check.sh [full|quick] [layer-build-dir]
add_custom_target(layer-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/layer_check.sh" full "${LAYER_OUT}"
    DEPENDS
        ${LAYER_DEP}
        vkbridge-smoke
        bcn_check
        layer-app
        nulldesc_check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
)

# host/layer_quick_check.sh [layer-build-dir]
add_custom_target(layer-quick-check
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/layer_quick_check.sh" "${LAYER_OUT}"
    DEPENDS
        ${LAYER_DEP}
        vkbridge-smoke
        layer-app
        nulldesc_check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
)

# host/layer_errata_test.sh (it reads VKB_OUT and VKB_BUILD)
add_custom_target(layer-errata-test
    COMMAND "${CMAKE_COMMAND}" -E env
            "VKB_OUT=${OUT}"
            "VKB_BUILD=${LAYER_OUT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/layer_errata_test.sh"
    DEPENDS
        ${LAYER_DEP}
        vkbridge-smoke
        nulldesc_check
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM
)

# CMake's own `clean` only knows the build tree; the programs live in $(OUT).
add_custom_target(clean-out
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${OUT}"
    COMMENT "Removing ${OUT}"
    VERBATIM
)
