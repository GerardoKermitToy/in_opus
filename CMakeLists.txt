cmake_minimum_required(VERSION 3.24...4.4)
project(WinampOpus VERSION 1.2.5 LANGUAGES C CXX)

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

option(WINAMP_OPUS_DEPENDENCIES_ONLY "Build only the three static libraries" OFF)
option(WINAMP_OPUS_BUILD_TESTS "Build decoder and input-plugin tests" ON)
set(WINAMP_OPUS_TEST_FILE "" CACHE FILEPATH "Optional local Opus file for CTest")
set(WINAMP_OPUS_MULTICHANNEL_TEST_FILE "" CACHE FILEPATH "Optional 5.1/7.1 Opus file for CTest")

include(cmake/Dependencies.cmake)
if(WINAMP_OPUS_DEPENDENCIES_ONLY)
    return()
endif()

add_library(opus_decoder STATIC src/opus_decoder.cpp src/opus_decoder.hpp
    src/channel_layout.cpp src/channel_layout.hpp)
target_link_libraries(opus_decoder PUBLIC opusfile)
target_include_directories(opus_decoder PUBLIC src)
target_compile_definitions(opus_decoder PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(opus_decoder PRIVATE /W4 /permissive- /utf-8)

add_library(in_opus SHARED src/plugin.cpp src/winamp_input.hpp src/in_opus.def src/version.rc
    src/file_info_dialog.cpp src/file_info_dialog.hpp src/file_info_dialog.rc src/resource.h
    src/output_settings.cpp src/output_settings.hpp)
target_link_libraries(in_opus PRIVATE opus_decoder user32 gdi32 advapi32)
target_compile_options(in_opus PRIVATE /W4 /permissive- /utf-8)
set_target_properties(in_opus PROPERTIES PREFIX "" OUTPUT_NAME "in_opus"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

install(TARGETS in_opus RUNTIME DESTINATION Plugins COMPONENT Runtime)
install(FILES README.md THIRD_PARTY_NOTICES.md LICENSE DESTINATION . COMPONENT Runtime)
install(DIRECTORY "${CMAKE_BINARY_DIR}/licenses/" DESTINATION licenses COMPONENT Runtime)

if(WINAMP_OPUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
