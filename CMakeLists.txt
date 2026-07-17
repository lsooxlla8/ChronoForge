cmake_minimum_required(VERSION 3.24)

project(ChronoForge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHRONOFORGE_BUILD_TESTS "Build core verification tests" ON)

add_library(chronoforge_core
    src/cache_store.cpp
    src/effects.cpp
    src/file_executor.cpp
    src/mapped_tensor.cpp
    src/node_graph.cpp
    src/resource_planner.cpp
    src/spectral.cpp
)

target_include_directories(chronoforge_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(chronoforge_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(chronoforge_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(chronoforge_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(chronoforge-cli apps/cli/main.cpp)
target_link_libraries(chronoforge-cli PRIVATE chronoforge_core)

if(CHRONOFORGE_BUILD_TESTS)
    enable_testing()
    add_executable(chronoforge-core-tests tests/core_tests.cpp)
    target_link_libraries(chronoforge-core-tests PRIVATE chronoforge_core)
    add_test(NAME chronoforge-core-tests COMMAND chronoforge-core-tests)
    add_executable(chronoforge-visual-regression tests/visual_regression.cpp)
    target_link_libraries(chronoforge-visual-regression PRIVATE chronoforge_core)
    add_test(
        NAME chronoforge-visual-regression
        COMMAND chronoforge-visual-regression ${CMAKE_CURRENT_BINARY_DIR}/visual-regression
    )
endif()
