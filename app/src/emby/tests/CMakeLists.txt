# Standalone build for the live probe CLI (task T10).
#
#   cmake -S app/src/emby/tests -B /tmp/emby-live-build
#   cmake --build /tmp/emby-live-build -j
#
# Credentials are passed via environment variables at run time; this file
# contains none.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(emby_live_probe LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/emby)

add_executable(live_probe live_probe.cpp)
target_link_libraries(live_probe PRIVATE emby)
