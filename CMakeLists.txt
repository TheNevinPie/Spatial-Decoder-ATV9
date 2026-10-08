cmake_minimum_required(VERSION 3.16)
project(SpatialDecoder VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTS "Build tests" ON)
option(ANDROID_BUILD "Build for Android" OFF)
option(BUILD_OMX_ADAPTER "Build OMX adapter libraries" ON)

if(ANDROID_BUILD)
    set(CMAKE_SYSTEM_NAME Android)
    set(CMAKE_ANDROID_ARCH_ABI armv7-a)
    set(CMAKE_ANDROID_NDK /opt/android/ndk)
    set(CMAKE_ANDROID_NATIVE_API_LEVEL 28)
endif()

# Support explicit FFmpeg prefix (for Android cross-compile without pkg-config)
if(FFMPEG_PREFIX)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PREFIX}/include")
    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREFIX}/lib/libavcodec.so"
        "${FFMPEG_PREFIX}/lib/libavutil.so"
        "${FFMPEG_PREFIX}/lib/libavformat.so"
        "${FFMPEG_PREFIX}/lib/libswresample.so"
    )
    message(STATUS "Using FFmpeg from prefix: ${FFMPEG_PREFIX}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libavformat libswresample)
endif()

# Filtered FFmpeg set (no libavformat). VERIFIED via llvm-nm: no spatial
# static library references any avformat_* symbol, and no libavformat.so
# ships in the device's system/vendor lib dumps, so it must not appear in
# any DT_NEEDED. Tests keep the full set.
set(FFMPEG_CORE_LIBRARIES "")
foreach(_fflib IN LISTS FFMPEG_LIBRARIES)
    if(NOT _fflib MATCHES "avformat")
        list(APPEND FFMPEG_CORE_LIBRARIES ${_fflib})
    endif()
endforeach()

# Core static libraries
add_library(spatial_pcm STATIC
    src/pcm/spatial_pcm.c
    src/pcm/spatial_channel_layout.c
)
target_include_directories(spatial_pcm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(spatial_pcm PUBLIC ${FFMPEG_CORE_LIBRARIES})

add_library(spatial_config STATIC
    src/config/spatial_config.c
    src/config/property_bridge.c
)
target_include_directories(spatial_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
)
if(NOT ANDROID_BUILD)
    target_link_libraries(spatial_config PUBLIC rt)
else()
    target_link_libraries(spatial_config PUBLIC log android)
endif()

add_library(spatial_downmix STATIC
    src/downmix/spatial_downmix.c
)
target_include_directories(spatial_downmix PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/downmix
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
)
target_link_libraries(spatial_downmix PUBLIC spatial_config spatial_pcm)

add_library(spatial_decoder STATIC
    src/decoder/spatial_decoder.c
)
target_include_directories(spatial_decoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
)
target_link_libraries(spatial_decoder PUBLIC spatial_pcm ${FFMPEG_CORE_LIBRARIES})

# Main shared library (libspatialdecoder.so)
add_library(spatial_all SHARED
    $<TARGET_OBJECTS:spatial_pcm>
    $<TARGET_OBJECTS:spatial_config>
    $<TARGET_OBJECTS:spatial_downmix>
    $<TARGET_OBJECTS:spatial_decoder>
)
target_include_directories(spatial_all PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/downmix
    ${CMAKE_CURRENT_SOURCE_DIR}/src/omx
)
target_link_libraries(spatial_all PUBLIC ${FFMPEG_CORE_LIBRARIES} log android)
set_target_properties(spatial_all PROPERTIES OUTPUT_NAME "spatialdecoder")

# OMX Adapter Libraries
# Built only for Android: the adapter subclasses the framework's
# SimpleSoftOMXComponent (headers vendored under src/omx/aosp_include from
# Android 9 / lineage-16.0) and resolves the base implementation at runtime
# from the media.codec process's libstagefright_omx.so.
if(BUILD_OMX_ADAPTER AND ANDROID_BUILD AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/omx/SpatialOmxAdapter.cpp")
    enable_language(CXX)

    # Common OMX adapter sources (adapter only; framework base classes come
    # from the device's libstagefright_omx.so at runtime).
    set(OMX_ADAPTER_SOURCES
        src/omx/SpatialOmxAdapter.cpp
    )

    set(OMX_ADAPTER_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/omx/aosp_include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/omx
        ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/src/config
        ${CMAKE_CURRENT_SOURCE_DIR}/src/downmix
        ${FFMPEG_INCLUDE_DIRS}
    )

    # Device link inputs for framework/system libs (ARMv7-A prebuilts pulled
    # from the MT5862 device; used for DT_NEEDED + build-time symbol checks).
    set(OMX_SYSROOT_LIBS "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/reference/system_lib_vndk28/libstagefright_omx.so")
        set(OMX_SYSROOT_LIBS
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/system_lib_vndk28/libstagefright_omx.so
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/system_lib_vndk28/libstagefright_foundation.so
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/system_lib/libutils.so
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/system_lib/libcutils.so
            ${CMAKE_CURRENT_SOURCE_DIR}/reference/system_lib/liblog.so
        )
    endif()

    # NOTE: no libavformat (reference NEEDED has avcodec/avutil/swresample
    # only; VERIFIED unused); no spatial_all (would duplicate the static
    # spatial_* objects); no libandroid (UNUSED baggage: no AMedia*/ALooper
    # C-API refs in adapters; libandroid.so is NOT loadable in the vendor
    # media.codec linker namespace, so its DT_NEEDED breaks service-side
    # dlopen (0x80001003). --as-needed below drops any other unused NEEDED.
    set(OMX_ADAPTER_LINK_LIBS
        spatial_decoder
        spatial_pcm
        spatial_downmix
        spatial_config
        ${OMX_SYSROOT_LIBS}
        ${FFMPEG_CORE_LIBRARIES}
        log
        dl
        m
        c
    )

    # Build four separate libraries with s_ output names. OUTPUT_NAME +
    # default "lib" PREFIX yields libstagefright_soft_s_*.so, matching both
    # the filenames SoftOMXPlugin dlopens AND the DT_SONAME (Bionic dedups
    # dlopen by SONAME: an ac3dec-style SONAME would collide with the stock
    # vndk-28 libs already loaded in media.codec and return the WRONG
    # handle). Unique s_* SONAMEs force a fresh load of our files.
    function(add_omx_adapter name output_name codec_name)
        add_library(${name} SHARED ${OMX_ADAPTER_SOURCES})
        target_include_directories(${name} PRIVATE ${OMX_ADAPTER_INCLUDES})
        target_compile_definitions(${name} PRIVATE DEFAULT_CODEC_NAME="${codec_name}")
        # Platform convention (VERIFIED: device libs export no typeinfo):
        # in-tree soft components build with -fno-rtti -fno-exceptions.
        target_compile_options(${name} PRIVATE -fno-rtti -fno-exceptions)
        target_link_libraries(${name} PRIVATE ${OMX_ADAPTER_LINK_LIBS})
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME "${output_name}"
            SUFFIX ".so"
            POSITION_INDEPENDENT_CODE ON
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/omx/omx_adapter.version -Wl,--as-needed"
        )
    endfunction()

    add_omx_adapter(spatial_soft_ac3 stagefright_soft_s_ac3 ac3dec)
    add_omx_adapter(spatial_soft_eac3 stagefright_soft_s_eac3 eac3dec)
    add_omx_adapter(spatial_soft_dts stagefright_soft_s_dts dtsdec)
    add_omx_adapter(spatial_soft_thd stagefright_soft_s_thd truehddec)
endif()

if(BUILD_TESTS)
    enable_testing()

    add_executable(test_decoder tests/decoder_test.c)
    target_link_libraries(test_decoder spatial_decoder spatial_pcm spatial_downmix ${FFMPEG_LIBRARIES})

    add_executable(test_downmix tests/downmix_test.c)
    target_link_libraries(test_downmix spatial_downmix spatial_config spatial_pcm ${FFMPEG_LIBRARIES})
    if(NOT ANDROID_BUILD)
        # Host-only: Android property/log headers backed by environment
        # variables (tests/host_shim) so property_bridge.c compiles and
        # runs off-device. Never used in device builds.
        target_include_directories(spatial_config PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/host_shim)
        target_sources(test_downmix PRIVATE tests/host_shim/shim.c)
    endif()

    add_executable(matrix_inspector src/downmix/matrix_inspector.c)
    target_link_libraries(matrix_inspector spatial_config spatial_pcm)

    if(ANDROID_BUILD)
        # Device-side standalone factory/lifecycle harness (ARM binary run
        # via adb; dlopen-only, no framework link).
        add_executable(omx_harness tests/omx_harness.c)
        target_include_directories(omx_harness PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/omx/aosp_include)
        target_link_libraries(omx_harness PRIVATE dl log)
        # Device-side Phase B decode harness: real OMX buffer interface.
        add_executable(omx_decode_test tests/omx_decode_test.c)
        target_include_directories(omx_decode_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/omx/aosp_include)
        target_link_libraries(omx_decode_test PRIVATE dl log aaudio)
        # Device-side production-feed probe: NDK AMediaExtractor dump of
        # what stagefright yields per sample (sizes, alignment).
        add_executable(extractor_probe tests/extractor_probe.cpp)
        target_link_libraries(extractor_probe PRIVATE mediandk log)
        # Device-side OMX name-resolution probe: app-level (AMediaCodec)
        # + plugin-level (SoftOMXPlugin exact OMX_ERRORTYPE).
        add_executable(omx_probe tests/omx_probe.cpp)
        target_include_directories(omx_probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/omx/aosp_include)
        target_link_libraries(omx_probe PRIVATE mediandk log dl aaudio)
        # Device-side BL/LR bit0 probe (diagnostic for PIC cave work).
        add_executable(lr_probe tests/lr_probe.c)
        target_link_libraries(lr_probe PRIVATE log)
        # Device-side mapping scanner: finds patched cave/data bytes at
        # runtime with zero address-frame assumptions (/proc/self/maps).
        add_executable(mapscan tests/mapscan.cpp)
        target_link_libraries(mapscan PRIVATE log dl)
    endif()

    add_test(NAME decoder_test COMMAND test_decoder)
    add_test(NAME downmix_test COMMAND test_downmix)
endif()

install(TARGETS spatial_all
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES
    src/decoder/spatial_decoder.h
    src/pcm/spatial_pcm.h
    src/pcm/spatial_channel_layout.h
    src/config/spatial_config.h
    src/config/property_bridge.h
    src/downmix/spatial_downmix.h
    DESTINATION include
)

install(DIRECTORY src/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)