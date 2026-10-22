# Unit tests for the layer
cmake_minimum_required(VERSION 3.19)

project(layer_tests C CXX)

find_package(Catch2 3 REQUIRED)
find_package(Threads REQUIRED)

file(GLOB LAYER_TEST_SERVER_SOURCES "${CMAKE_SOURCE_DIR}/server/*.c")
list(REMOVE_ITEM LAYER_TEST_SERVER_SOURCES "${CMAKE_SOURCE_DIR}/server/server_main.c")

add_executable(layer_tests
    test_capabilities.cpp
    test_layer_ctx.cpp
    test_support.cpp
    ../capabilities.cpp
    ../layer_ctx.cpp
    ${LAYER_TEST_SERVER_SOURCES}
    ${CMAKE_SOURCE_DIR}/common/vkb_caps.c
    ${CMAKE_SOURCE_DIR}/common/vkb_wire.c
    ${CMAKE_SOURCE_DIR}/common/gen/vkb_gen_structs.c
    ${CMAKE_SOURCE_DIR}/common/gen/vkb_gen_server.c
)

target_include_directories(layer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(layer_tests PRIVATE
    vkb_server_common
    Catch2::Catch2WithMain
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)

target_compile_features(layer_tests PRIVATE cxx_std_17)
target_compile_options(layer_tests PRIVATE
    -Wall
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

add_test(NAME layer_tests COMMAND layer_tests)
