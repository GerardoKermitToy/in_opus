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
dependency_url(speexdsp_url "speexdsp-1.2.1-git/speexdsp-1.2.1.tar.gz"
    "https://codeload.github.com/xiph/speexdsp/tar.gz/1b28a0f61bc31162979e1f26f3981fc3637095c8")
FetchContent_Declare(speexdsp URL "${speexdsp_url}"
    URL_HASH SHA256=59a53188f6d6ca2b48752f533523986a0604d2526eb50789b8fb09aa6b12645f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/speexdsp")
FetchContent_Declare(ogg URL "${ogg_url}"
    URL_HASH SHA256=5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/ogg")
FetchContent_Declare(opus URL "${opus_url}"
    URL_HASH SHA256=3267aa969b2b26542417f57819a5bc25d5079355300307dd2dddc7c704aed418
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/opus"
    SOURCE_SUBDIR _populate_only)
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
# Neural weights are a separate, upstream checksum-pinned data archive. Only
# generated C/header tables are consumed; no training code or runtime download.
dependency_url(opus_models_url
    "opus-models-1.6.1/opus_data-a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e.tar.gz"
    "https://media.xiph.org/opus/models/opus_data-a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e.tar.gz")
FetchContent_Declare(opus_models URL "${opus_models_url}"
    URL_HASH SHA256=a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE TLS_VERIFY TRUE
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/opus-models"
    SOURCE_SUBDIR _populate_only)
# Standard decoding stays unenhanced by default. OSCE, QEXT and DRED are built
# for explicit runtime use; none makes HTTP/TCP transport recovery automatic.
# OSCE/DRED also pull upstream's shared DNN/PLC support into the static library.
set(OPUS_FIXED_POINT OFF CACHE BOOL "Use floating point for the plugin" FORCE)
set(OPUS_CUSTOM_MODES OFF CACHE BOOL "Disable nonstandard CELT modes" FORCE)
set(OPUS_DRED ON CACHE BOOL "Compile optional, bounded packet-loss recovery" FORCE)
set(OPUS_OSCE ON CACHE BOOL "Compile optional speech enhancement" FORCE)
set(OPUS_DEEP_PLC OFF CACHE BOOL "Do not request standalone neural PLC" FORCE)
set(OPUS_HARDENING ON CACHE BOOL "" FORCE)
set(OPUS_PACKAGE_VERSION "1.6.1")
FetchContent_MakeAvailable(ogg opus_models opus opusfile speexdsp)
set(opus_model_directory "${opus_models_SOURCE_DIR}")
if(EXISTS "${opus_model_directory}/dnn")
    string(APPEND opus_model_directory "/dnn")
endif()
foreach(model_file lace_data.c lace_data.h nolace_data.c nolace_data.h
        bbwenet_data.c bbwenet_data.h fargan_data.c fargan_data.h
        plc_data.c plc_data.h pitchdnn_data.c pitchdnn_data.h
        dred_rdovae_constants.h dred_rdovae_dec_data.h dred_rdovae_enc_data.h
        dred_rdovae_stats_data.h dred_rdovae_dec_data.c dred_rdovae_enc_data.c
        dred_rdovae_stats_data.c lossgen_data.c lossgen_data.h)
    if(NOT EXISTS "${opus_model_directory}/${model_file}")
        message(FATAL_ERROR "Missing checksum-pinned Opus neural table: ${model_file}")
    endif()
    file(COPY "${opus_model_directory}/${model_file}" DESTINATION "${opus_SOURCE_DIR}/dnn")
endforeach()
add_subdirectory("${opus_SOURCE_DIR}" "${opus_BINARY_DIR}")
# Upstream 1.6.1 CMake omits these configure.ac options. Enable the exact same
# source guards without patching downloaded implementation files.
target_compile_definitions(opus PRIVATE ENABLE_QEXT=1 ENABLE_OSCE_BWE=1)
if(MSVC AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    # MSVC accepts these SSE4.1 intrinsics without a GCC-style ISA switch.
    get_target_property(opus_compile_sources opus SOURCES)
    foreach(opus_compile_source IN LISTS opus_compile_sources)
        if(opus_compile_source MATCHES "sse4_1\\.c$")
            if(NOT IS_ABSOLUTE "${opus_compile_source}")
                set(opus_compile_source "${opus_SOURCE_DIR}/${opus_compile_source}")
            endif()
            set_property(SOURCE "${opus_compile_source}" DIRECTORY "${opus_SOURCE_DIR}"
                PROPERTY COMPILE_FLAGS "")
        endif()
    endforeach()
endif()

# Standalone, namespaced SpeexDSP resampler; no echo canceller or codec.
add_library(speex_resampler STATIC "${speexdsp_SOURCE_DIR}/libspeexdsp/resample.c")
target_include_directories(speex_resampler PUBLIC "${speexdsp_SOURCE_DIR}/include/speex"
    PRIVATE "${speexdsp_SOURCE_DIR}/libspeexdsp")
target_compile_definitions(speex_resampler PUBLIC OUTSIDE_SPEEX RANDOM_PREFIX=winamp_opus
    PRIVATE FLOATING_POINT)

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

foreach(dependency IN ITEMS ogg opus opusfile speex_resampler)
    set_target_properties(${dependency} PROPERTIES
        FOLDER "Dependencies"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
endforeach()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses")
configure_file("${ogg_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/libogg.txt" COPYONLY)
configure_file("${opus_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/libopus.txt" COPYONLY)
configure_file("${opusfile_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/libopusfile.txt" COPYONLY)
configure_file("${speexdsp_SOURCE_DIR}/COPYING" "${CMAKE_BINARY_DIR}/licenses/speexdsp.txt" COPYONLY)
