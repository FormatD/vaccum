cmake_minimum_required(VERSION 3.22)
project(vacuum_jni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The repository root, four levels up from android/app/src/main/cpp.
get_filename_component(VACUUM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../ ABSOLUTE)

# Reuse the desktop core verbatim rather than listing its sources again, so the
# phone and the CLI cannot drift apart.
add_subdirectory(${VACUUM_ROOT}/core ${CMAKE_CURRENT_BINARY_DIR}/vacuum_core)

add_library(vacuum_jni SHARED jni_bridge.cpp)
# No `log`: the bridge reports errors through nativeLastError instead of
# __android_log_print, so the only dependency is the core itself.
target_link_libraries(vacuum_jni PRIVATE vacuum_core)
target_compile_options(vacuum_jni PRIVATE -Wall -Wextra -Wpedantic -O2)
