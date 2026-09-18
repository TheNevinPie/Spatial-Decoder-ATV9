cmake_minimum_required(VERSION 3.16)
project(SpatialDecoder VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_TESTS "Build tests" ON)
option(ANDROID_BUILD "Build for Android" OFF)

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

add_library(spatial_pcm STATIC
    src/pcm/spatial_pcm.c
    src/pcm/spatial_channel_layout.c
)
target_include_directories(spatial_pcm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(spatial_pcm PUBLIC ${FFMPEG_LIBRARIES})

add_library(spatial_config STATIC
    src/config/spatial_config.c
    src/config/property_bridge.c
)
target_include_directories(spatial_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
)
if(NOT ANDROID_BUILD)
    target_link_libraries(spatial_config PUBLIC rt)
endif()

add_library(spatial_downmix STATIC
    src/downmix/spatial_downmix.c
)
target_include_directories(spatial_downmix PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/downmix
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
)
target_link_libraries(spatial_downmix PUBLIC spatial_config spatial_pcm)

add_library(spatial_decoder STATIC
    src/decoder/spatial_decoder.c
)
target_include_directories(spatial_decoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pcm
)
target_link_libraries(spatial_decoder PUBLIC spatial_pcm ${FFMPEG_LIBRARIES})

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
)
target_link_libraries(spatial_all PUBLIC ${FFMPEG_LIBRARIES})
set_target_properties(spatial_all PROPERTIES OUTPUT_NAME "spatialdecoder")

if(BUILD_TESTS)
    enable_testing()
    
    add_executable(test_decoder tests/decoder_test.c)
    target_link_libraries(test_decoder spatial_decoder spatial_pcm spatial_downmix ${FFMPEG_LIBRARIES})
    
    add_executable(test_downmix tests/downmix_test.c)
    target_link_libraries(test_downmix spatial_downmix spatial_config spatial_pcm ${FFMPEG_LIBRARIES})
    
    add_executable(matrix_inspector src/downmix/matrix_inspector.c)
    target_link_libraries(matrix_inspector spatial_config spatial_pcm)
    
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