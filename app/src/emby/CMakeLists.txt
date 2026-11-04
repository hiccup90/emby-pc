# Emby/Jellyfin API client module (Qt6 Network only, no Widgets dependency).
#
# Integration (done by the app root CMakeLists):
#     add_subdirectory(src/emby)
#     target_link_libraries(emby-pc PRIVATE emby)
#
# The module also exports EMBY_SOURCES for consumers that prefer to add the
# sources directly to their own target.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(emby_api LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(EMBY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/embytypes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/embytypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/deviceprofile.h
    ${CMAKE_CURRENT_SOURCE_DIR}/deviceprofile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/embyclient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/embyclient.cpp
)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(EMBY_SOURCES "${EMBY_SOURCES}" PARENT_SCOPE)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Network)

if(NOT TARGET emby)
    add_library(emby STATIC ${EMBY_SOURCES})
    add_library(emby::emby ALIAS emby)
    set_target_properties(emby PROPERTIES
        AUTOMOC ON
        AUTOUIC OFF
        AUTORCC OFF
    )
    target_include_directories(emby PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    )
    target_link_libraries(emby PUBLIC Qt6::Network Qt6::Core)
    target_compile_features(emby PUBLIC cxx_std_20)
endif()

option(EMBY_BUILD_TESTS "Build the Emby API client self-tests" OFF)
if(EMBY_BUILD_TESTS)
    enable_testing()
    add_executable(emby_tests tests/embyclient_test.cpp)
    target_link_libraries(emby_tests PRIVATE emby)
    add_test(NAME emby_tests COMMAND emby_tests)
endif()
