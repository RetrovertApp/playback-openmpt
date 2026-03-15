# OpenMPT Playback Plugin
#
# Decodes tracker music formats (MOD, XM, S3M, IT, etc.) using libopenmpt.

cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

enable_language(CXX)

# Download libopenmpt 0.8.4 (with scope capture patch for audio visualization)
download_library(
    NAME openmpt
    URL "http://lib.openmpt.org/files/libopenmpt/src/libopenmpt-0.8.4+release.autotools.tar.gz"
    FILENAME "libopenmpt-0.8.4.tar.gz"
    HASH "627f9bf11aacae615a1f2c982c7e88cb21f11b2d6f0267946f7c82c5eae4943b"
    EXTRACT_DIR "libopenmpt-0.8.4+release.autotools"
    PATCHES patches/scope-capture.patch
)

set(LIBOPENMPT_ROOT "${openmpt_SOURCE_DIR}")

# Collect sources matching autotools Makefile.am
file(GLOB LIBOPENMPT_COMMON_SOURCES "${LIBOPENMPT_ROOT}/common/*.cpp")
file(GLOB LIBOPENMPT_SOUNDDSP_SOURCES "${LIBOPENMPT_ROOT}/sounddsp/*.cpp")
file(GLOB LIBOPENMPT_SOUNDLIB_SOURCES "${LIBOPENMPT_ROOT}/soundlib/*.cpp")
file(GLOB LIBOPENMPT_SOUNDLIB_PLUGIN_SOURCES "${LIBOPENMPT_ROOT}/soundlib/plugins/*.cpp")
file(GLOB LIBOPENMPT_SOUNDLIB_PLUGIN_DMO_SOURCES "${LIBOPENMPT_ROOT}/soundlib/plugins/dmo/*.cpp")

set(LIBOPENMPT_CORE_SOURCES
    "${LIBOPENMPT_ROOT}/libopenmpt/libopenmpt_c.cpp"
    "${LIBOPENMPT_ROOT}/libopenmpt/libopenmpt_cxx.cpp"
    "${LIBOPENMPT_ROOT}/libopenmpt/libopenmpt_impl.cpp"
    "${LIBOPENMPT_ROOT}/libopenmpt/libopenmpt_ext_impl.cpp"
)

add_library(openmpt_playback SHARED
    openmpt_plugin.cpp
    ${LIBOPENMPT_COMMON_SOURCES}
    ${LIBOPENMPT_SOUNDDSP_SOURCES}
    ${LIBOPENMPT_SOUNDLIB_SOURCES}
    ${LIBOPENMPT_SOUNDLIB_PLUGIN_SOURCES}
    ${LIBOPENMPT_SOUNDLIB_PLUGIN_DMO_SOURCES}
    ${LIBOPENMPT_CORE_SOURCES}
)

set_target_properties(openmpt_playback PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(openmpt_playback PRIVATE
    ${LIBOPENMPT_ROOT}
    ${LIBOPENMPT_ROOT}/src
    ${LIBOPENMPT_ROOT}/common
)

target_compile_definitions(openmpt_playback PRIVATE LIBOPENMPT_BUILD)

if(WIN32)
    target_compile_definitions(openmpt_playback PRIVATE UNICODE _UNICODE)
endif()

# libopenmpt expects stdafx.h to be implicitly included before all source files
if(MSVC)
    target_compile_options(openmpt_playback PRIVATE "/FI${LIBOPENMPT_ROOT}/common/stdafx.h")
else()
    target_compile_options(openmpt_playback PRIVATE -include "${LIBOPENMPT_ROOT}/common/stdafx.h")
endif()

rv_set_plugin_properties(openmpt_playback)
