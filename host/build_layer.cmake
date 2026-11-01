# Used by the `vkb_layer` target in host/CMakeLists.txt:
#   cmake -DVKB=<repo> -DLAYER_BUILD_DIR=<dir> -DOUT=<dir> -P host/build_layer.cmake
#
# The layer's build tree is the top-level project's own (default: build-linux-x86-64), configured
# once here if it does not exist yet; its cache and flags are left alone afterwards.

foreach(v VKB LAYER_BUILD_DIR OUT)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "host/build_layer.cmake: -D${v}=<dir> is required")
    endif()
endforeach()

if(NOT EXISTS "${LAYER_BUILD_DIR}/CMakeCache.txt")
    message(STATUS "configuring the layer's build tree: ${LAYER_BUILD_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${VKB}" -B "${LAYER_BUILD_DIR}"
                -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release
        RESULT_VARIABLE rc
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "cannot configure the layer in ${LAYER_BUILD_DIR}")
    endif()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${LAYER_BUILD_DIR}"
            --target VkLayer_malibridge_compat
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cannot build VkLayer_malibridge_compat in ${LAYER_BUILD_DIR}")
endif()

# The library and the manifest the top-level project generates next to it (its library_path is
# "./libVkLayer_malibridge_compat.so", i.e. relative to the manifest).
foreach(f libVkLayer_malibridge_compat.so VkLayer_malibridge_compat.json)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LAYER_BUILD_DIR}/${f}" "${OUT}/${f}"
        RESULT_VARIABLE rc
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "no ${f} in ${LAYER_BUILD_DIR}")
    endif()
endforeach()
