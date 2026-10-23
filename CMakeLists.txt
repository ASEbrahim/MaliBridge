cmake_minimum_required(VERSION 3.19)
project(vkbridge C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckIncludeFile)
find_package(Threads REQUIRED)
find_library(LOG_LIB log)

execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE VKB_GIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)

if(NOT GIT_RESULT EQUAL 0 OR NOT VKB_GIT)
    set(VKB_GIT "unknown")
endif()

check_include_file("jni.h" HAVE_SYS_JNI)

if(HAVE_SYS_JNI)
    set(JNI_AVAILABLE TRUE)
else()
    set(JNI_AVAILABLE FALSE)
    message(STATUS "jni.h not found: vkbridge_jni will be excluded from the default build.")
endif()

add_library(vkb_common_flags INTERFACE)
target_include_directories(vkb_common_flags INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers-1.4.341/include
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    # We don't call gen_vkbridge.py here since this is cached
    ${CMAKE_CURRENT_SOURCE_DIR}/common/gen
)
target_compile_definitions(vkb_common_flags INTERFACE
    VKB_GIT="${VKB_GIT}"
)
target_compile_options(vkb_common_flags INTERFACE
    -Wall
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

set(COMMON_GEN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/common/vkb_wire.c
    ${CMAKE_CURRENT_SOURCE_DIR}/common/gen/vkb_gen_structs.c
)

set(CAPS_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/common/vkb_caps.c
)

# Server-side
file(GLOB SERVER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/server/*.c")
set(SERVER_ALL_SOURCES
    ${SERVER_SOURCES}
    ${COMMON_GEN_SOURCES}
    ${CAPS_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/common/gen/vkb_gen_server.c
)

add_library(vkb_server_common INTERFACE)
target_link_libraries(vkb_server_common INTERFACE
    vkb_common_flags
    ${CMAKE_DL_LIBS}
    m
    $<$<BOOL:${LOG_LIB}>:${LOG_LIB}>
)
target_include_directories(vkb_server_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/server
)
target_link_options(vkb_server_common INTERFACE
    -Wl,-z,max-page-size=16384
    -Wl,--build-id=none
)

add_executable(vkbridge_server ${SERVER_ALL_SOURCES})
target_link_libraries(vkbridge_server PRIVATE vkb_server_common)
set_target_properties(vkbridge_server PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX "lib"
    SUFFIX ".so"
)

if(JNI_AVAILABLE)
    add_library(vkbridge_jni SHARED ${SERVER_ALL_SOURCES})
else()
    add_library(vkbridge_jni SHARED EXCLUDE_FROM_ALL ${SERVER_ALL_SOURCES})
endif()

target_link_libraries(vkbridge_jni PRIVATE vkb_server_common)
target_compile_definitions(vkbridge_jni PRIVATE VKB_JNI)
target_link_options(vkbridge_jni PRIVATE -Wl,--no-undefined)

# Client-side
file(GLOB CLIENT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/client/*.c")
set(CLIENT_ALL_SOURCES
    ${CLIENT_SOURCES}
    ${COMMON_GEN_SOURCES}
    ${CAPS_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/common/gen/vkb_gen_client.c
)

add_library(vulkan_droidbridge SHARED ${CLIENT_ALL_SOURCES})
target_link_libraries(vulkan_droidbridge PRIVATE
    vkb_common_flags
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_include_directories(vulkan_droidbridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/client
    ${CMAKE_CURRENT_SOURCE_DIR}/client/compat
)
target_compile_definitions(vulkan_droidbridge PRIVATE VKB_CLIENT)
set_target_properties(vulkan_droidbridge PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME "vulkan_droidbridge"
)
target_link_options(vulkan_droidbridge PRIVATE
    -Wl,--no-undefined
    -Wl,-z,nodelete
)

add_executable(droiddeck-vkbridge-smoke tests/smoke.c)
target_link_libraries(droiddeck-vkbridge-smoke PRIVATE
    vkb_common_flags
    ${CMAKE_DL_LIBS}
)
target_include_directories(droiddeck-vkbridge-smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

set(ICD_JSON_PATH "${CMAKE_CURRENT_BINARY_DIR}/droidbridge_icd.json")
file(GENERATE
    OUTPUT "${ICD_JSON_PATH}"
    CONTENT [=[{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/usr/local/lib/vkbridge/libvulkan_droidbridge.so",
        "api_version": "1.4.341"
    }
}
]=]
)

file(GLOB LAYER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/layer/*.cpp")
set(LAYER_ALL_SOURCES
    ${SERVER_SOURCES}
    ${COMMON_GEN_SOURCES}
    ${CAPS_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/common/gen/vkb_gen_server.c
    ${LAYER_SOURCES}
)

add_library(VkLayer_malibridge_compat SHARED ${LAYER_ALL_SOURCES})
target_compile_features(VkLayer_malibridge_compat PRIVATE cxx_std_17)
target_link_libraries(VkLayer_malibridge_compat PRIVATE vkb_server_common)
target_link_libraries(VkLayer_malibridge_compat PRIVATE Threads::Threads)
target_include_directories(VkLayer_malibridge_compat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/layer
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(VkLayer_malibridge_compat PRIVATE VKB_LAYER)
set_target_properties(VkLayer_malibridge_compat PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME "VkLayer_malibridge_compat"
)
target_link_options(VkLayer_malibridge_compat PRIVATE
    -Wl,-z,max-page-size=16384
    -Wl,--build-id=none
    # The loader exports vkGetInstanceProcAddr too, so without this, the dynamic linker binds the
    # layer's own copy to the loader's (the first definition in the global scope wins), and the
    # loader then takes the layer for itself causing a stackoverflow/deadlock
    -Wl,-Bsymbolic-functions
)

set(LAYER_JSON_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_malibridge_compat.json")
file(GENERATE
    OUTPUT "${LAYER_JSON_PATH}"
    CONTENT [=[{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_MALIBRIDGE_compat",
        "type": "GLOBAL",
        "library_path": "./libVkLayer_malibridge_compat.so",
        "api_version": "1.3.278",
        "implementation_version": "1",
        "description": "MaliBridge Compatibility & Errata Layer for Arm Mali",
        "enable_environment": {
            "ENABLE_MALIBRIDGE_COMPAT": "1"
        },
        "disable_environment": {
            "DISABLE_MALIBRIDGE_COMPAT": "1"
        }
    }
}
]=]
)

install(TARGETS vulkan_droidbridge
    LIBRARY DESTINATION lib/vkbridge
)
install(TARGETS vkbridge_server
    RUNTIME DESTINATION lib/vkbridge
)
install(TARGETS VkLayer_malibridge_compat
    RUNTIME DESTINATION lib/vkbridge
)

if(JNI_AVAILABLE)
    install(TARGETS vkbridge_jni
        LIBRARY DESTINATION lib/vkbridge
    )
endif()

install(TARGETS droiddeck-vkbridge-smoke
    RUNTIME DESTINATION bin
)
install(FILES "${ICD_JSON_PATH}"
    DESTINATION share/vkbridge
)
install(FILES "${LAYER_JSON_PATH}"
    DESTINATION etc/vulkan/implicit_layer.d
)

find_package(Catch2 3 QUIET)
set(layer_tests_target "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT layer_tests_target)
    set(layer_tests_target "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
set(layer_tests_32bit OFF)
if(CMAKE_C_FLAGS MATCHES "-m32" OR CMAKE_CXX_FLAGS MATCHES "-m32")
    set(layer_tests_32bit ON) # the host's Catch2 is not 32-bit
endif()
if(Catch2_FOUND AND NOT layer_tests_32bit AND
   layer_tests_target STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
    enable_testing()
    add_subdirectory(layer/tests)
else()
    message(STATUS "unit tests are not built (Catch2: ${Catch2_FOUND}, "
                   "target: ${layer_tests_target}, host: ${CMAKE_HOST_SYSTEM_PROCESSOR}, "
                   "32-bit: ${layer_tests_32bit})")
endif()
