cmake_minimum_required(VERSION 3.24...4.4)
project(WinampOpus VERSION 1.5.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Build with Microsoft Visual C++ on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Winamp 5.9 needs an x86 DLL. Configure with -A Win32 or a Win32 preset.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Keep the plugin, its static dependencies, and test hosts on the same DLL CRT.
# Release-like configurations use /MD; Debug uses /MDd (not for distribution).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Initialize Release IPO for the plugin and all source-built static libraries.
# Older dependency CMakeLists must honor IPO rather than silently ignore it.
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
# Keep the existing floating-point rules and runtime CPU dispatch unchanged.
include(CheckIPOSupported)
check_ipo_supported(RESULT winamp_opus_ipo_supported
    OUTPUT winamp_opus_ipo_error LANGUAGES C CXX)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE "${winamp_opus_ipo_supported}")
if(winamp_opus_ipo_supported)
    message(STATUS "Release IPO/LTO enabled")
else()
    message(WARNING "Release IPO/LTO unavailable: ${winamp_opus_ipo_error}")
endif()

option(WINAMP_OPUS_DEPENDENCIES_ONLY "Build only the static audio libraries" OFF)
option(WINAMP_OPUS_BUILD_TESTS "Build decoder and input-plugin tests" ON)
option(WINAMP_OPUS_BUILD_OGG_INPUT "Also build the independent sibling Ogg input plugin" ON)
set(WINAMP_OPUS_OGG_INPUT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../in_vorbis" CACHE PATH "Independent GPL Ogg input source directory")
set(WINAMP_OPUS_TEST_FILE "" CACHE FILEPATH "Optional local Opus file for CTest")
set(WINAMP_OPUS_MULTICHANNEL_TEST_FILE "" CACHE FILEPATH "Optional 5.1/7.1 Opus file for CTest")

# Enforce the actual CRT choice even for /GL objects whose directives cannot
# be inspected by dumpbin. This adds no runtime code.
add_compile_definitions("$<$<CONFIG:Debug>:WINAMP_OPUS_DEBUG_CRT>")
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/FI${PROJECT_SOURCE_DIR}/cmake/dynamic_crt.h>")

include(cmake/Dependencies.cmake)
if(WINAMP_OPUS_DEPENDENCIES_ONLY)
    return()
endif()

add_library(opus_decoder STATIC src/opus_decoder.cpp src/opus_decoder.hpp
    src/channel_layout.cpp src/channel_layout.hpp src/http_stream.cpp src/http_stream.hpp
    src/pcm_pipeline.cpp src/pcm_pipeline.hpp src/playback_settings.hpp
    src/native_opus.cpp src/native_opus.hpp src/opus_packet.cpp src/opus_packet.hpp
    src/ambisonics.cpp src/ambisonics.hpp
    src/credentials.cpp src/credentials.hpp src/processing.cpp src/processing.hpp
    src/gain_policy.cpp src/gain_policy.hpp src/tag_writer.cpp src/tag_writer.hpp
    src/host_settings.cpp src/host_settings.hpp src/language.cpp src/language.hpp)
target_link_libraries(opus_decoder PUBLIC opusfile speex_resampler winhttp user32 gdi32 advapi32 comctl32)
target_include_directories(opus_decoder PUBLIC src)
target_compile_definitions(opus_decoder PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(opus_decoder PRIVATE /W4 /permissive- /utf-8)

add_library(in_opus SHARED src/plugin.cpp src/winamp_input.hpp src/in_opus.def src/version.rc
    src/file_info_dialog.cpp src/file_info_dialog.hpp src/file_info_dialog.rc src/resource.h
    src/output_settings.cpp src/output_settings.hpp src/metadata.cpp src/metadata.hpp
    src/tag_edit.cpp src/tag_edit.hpp src/extended_read.cpp)
target_link_libraries(in_opus PRIVATE opus_decoder user32 gdi32 advapi32)
target_compile_options(in_opus PRIVATE /W4 /permissive- /utf-8)
set_target_properties(in_opus PROPERTIES PREFIX "" OUTPUT_NAME "in_opus"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

install(TARGETS in_opus RUNTIME DESTINATION Plugins COMPONENT Runtime)
if(PROJECT_IS_TOP_LEVEL)
    set(opus_documentation_destination .)
else()
    # Keep sibling documentation out of the parent package's root files.
    set(opus_documentation_destination opus-input)
endif()
install(FILES README.md THIRD_PARTY_NOTICES.md LICENSE
    DESTINATION "${opus_documentation_destination}" COMPONENT Runtime)
install(FILES docs/ADVANCED_DECODING.md docs/FEATURE_TRANSFER.md
    DESTINATION "${opus_documentation_destination}/docs" COMPONENT Runtime)
install(DIRECTORY docs/language-pack/ DESTINATION "${opus_documentation_destination}/docs/language-pack" COMPONENT Runtime)
install(DIRECTORY "${CMAKE_BINARY_DIR}/licenses/" DESTINATION licenses COMPONENT Runtime)

if(WINAMP_OPUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Optional independent GPL sibling; it reuses the targets above, not Winamp code.
if(WINAMP_OPUS_BUILD_OGG_INPUT AND NOT TARGET in_vorbis
   AND EXISTS "${WINAMP_OPUS_OGG_INPUT_SOURCE}/CMakeLists.txt")
    set(OGG_INPUT_BUILD_OPUS OFF)
    set(OGG_INPUT_BUILD_TESTS "${WINAMP_OPUS_BUILD_TESTS}")
    add_subdirectory("${WINAMP_OPUS_OGG_INPUT_SOURCE}" "${CMAKE_BINARY_DIR}/ogg-input")
endif()

# Keep checked/debug STL and CRT diagnostics enabled, but never block automated
# test executables on a modal assertion dialog on the user's desktop.
function(opus_noninteractive_test_reports directory)
    get_property(local_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(test_target IN LISTS local_targets)
        get_target_property(target_type "${test_target}" TYPE)
        if(target_type STREQUAL "EXECUTABLE")
            target_compile_options("${test_target}" PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/noninteractive_errors.hpp>")
        endif()
    endforeach()
    get_property(child_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS child_directories)
        opus_noninteractive_test_reports("${child}")
    endforeach()
endfunction()
if(WINAMP_OPUS_BUILD_TESTS)
    opus_noninteractive_test_reports("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
