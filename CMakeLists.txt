cmake_minimum_required(VERSION 3.24)

project(vacuum
  VERSION 0.1.0
  DESCRIPTION "Binary data transmission over audio, hidden inside ordinary MP3"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(VACUUM_BUILD_DESKTOP "Build the desktop CLI" ON)
option(VACUUM_BUILD_TESTS   "Build the test and benchmark binaries" ON)

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(core)
add_subdirectory(io)

if(VACUUM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(VACUUM_BUILD_DESKTOP)
  add_subdirectory(desktop)
endif()
