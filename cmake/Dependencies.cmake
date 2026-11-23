include(FetchContent)

# Every archive is checksum-pinned, downloaded into its own directory, and
# extracted by CMake into its own fresh source tree. Never run from a source tree.
function(dependency_url result local_path remote_url)
    if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/downloads/${local_path}")
        set(${result} "${PROJECT_SOURCE_DIR}/third_party/downloads/${local_path}" PARENT_SCOPE)
    else()
        set(${result} "${remote_url}" PARENT_SCOPE)
    endif()
endfunction()

dependency_url(ogg_url "ogg-1.3.6/libogg-1.3.6.tar.xz"
    "https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.xz")
dependency_url(opus_url "opus-1.6.1/opus-1.6.1.tar.gz"
    "https://codeload.github.com/xiph/opus/tar.gz/22244de5a79bd1d6d623c32e72bf1954b56235be")
dependency_url(opusfile_url "opusfile-0.12/opusfile-0.12.tar.gz"
    "https://github.com/xiph/opusfile/releases/download/v0.12/opusfile-0.12.tar.gz")
FetchContent_Declare(ogg URL "${ogg_url}"
    URL_HASH SHA256=5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/ogg")
FetchContent_Declare(opus URL "${opus_url}"
    URL_HASH SHA256=3267aa969b2b26542417f57819a5bc25d5079355300307dd2dddc7c704aed418
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/opus")
FetchContent_Declare(opusfile URL "${opusfile_url}"
    URL_HASH SHA256=118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/opusfile")

# Static audio archives still use the shared DLL CRT, just like the plugin.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static dependencies" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Do not build upstream tests" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME OFF CACHE BOOL "Use the same dynamic CRT as the plugin" FORCE)
# The plugin uses standard Ogg Opus decoding. Optional experimental modes and
# neural/redundancy paths are not part of ordinary local-file playback.
set(OPUS_FIXED_POINT OFF CACHE BOOL "Use floating point for the plugin" FORCE)
set(OPUS_CUSTOM_MODES OFF CACHE BOOL "Disable nonstandard CELT modes" FORCE)
set(OPUS_DRED OFF CACHE BOOL "Disable Deep Redundancy" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "Disable Opus Speech Coding Enhancement" FORCE)
set(OPUS_DEEP_PLC OFF CACHE BOOL "Disable neural packet loss concealment" FORCE)
set(OPUS_HARDENING ON CACHE BOOL "" FORCE)
set(OPUS_PACKAGE_VERSION "1.6.1")
FetchContent_MakeAvailable(ogg opus opusfile)

# opusfile 0.12 has no CMake build. These are exactly its core libopusfile
# sources from Makefile.am; omit opusurl/HTTP/TLS and their dependencies.
add_library(opusfile STATIC
    "${opusfile_SOURCE_DIR}/src/info.c"
    "${opusfile_SOURCE_DIR}/src/internal.c"
    "${opusfile_SOURCE_DIR}/src/opusfile.c"
    "${opusfile_SOURCE_DIR}/src/stream.c")
target_include_directories(opusfile PUBLIC "${opusfile_SOURCE_DIR}/include")
target_compile_definitions(opusfile PRIVATE _CRT_SECURE_NO_WARNINGS OP_HAVE_LRINTF)
target_link_libraries(opusfile PUBLIC Ogg::ogg opus)

foreach(dependency IN ITEMS ogg opus opusfile)
    set_target_properties(${dependency} PROPERTIES
        FOLDER "Dependencies"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
endforeach()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses")
configure_file("${ogg_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/libogg.txt" COPYONLY)
configure_file("${opus_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/libopus.txt" COPYONLY)
configure_file("${opusfile_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/libopusfile.txt" COPYONLY)
