project(nitrortmp)
cmake_minimum_required(VERSION 3.9.0)

set(PACKAGE_NAME nitrortmp)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_STANDARD 20)

# Define C++ library and add all sources
add_library(${PACKAGE_NAME} SHARED
        src/main/cpp/cpp-adapter.cpp
        # JNI bridge for NativePublisher.kt (calls the C facade below)
        src/main/cpp/NativePublisher.cpp
        # Platform independent core (cpp/nitrortmp), tested on the host by scripts/test-cpp.sh
        ../cpp/nitrortmp/RtmpUrl.cpp
        ../cpp/nitrortmp/RtmpPublisher.cpp
        ../cpp/nitrortmp/nitrortmp_c.cpp
        # Encoder output helpers (AVCC -> Annex-B, ADTS header) and their JNI bridge
        ../cpp/nitrortmp/AnnexB.cpp
        ../cpp/nitrortmp/Adts.cpp
        src/main/cpp/NativeCodec.cpp
)

# Add Nitrogen specs :)
include(${CMAKE_SOURCE_DIR}/../nitrogen/generated/android/nitrortmp+autolinking.cmake)

# Set up local includes
include_directories("src/main/cpp" "../cpp" "../cpp/nitrortmp")

# Vendored ireader/media-server (MIT): RTMP client, FLV muxer, MPEG-TS muxer.
# Synced by scripts/sync-media-server.sh, see cpp/third_party/media-server/UPSTREAM.md.
set(MEDIA_SERVER_ROOT "${CMAKE_SOURCE_DIR}/../cpp/third_party/media-server")
file(GLOB MEDIA_SERVER_SOURCES CONFIGURE_DEPENDS
        "${MEDIA_SERVER_ROOT}/librtmp/source/*.c"
        "${MEDIA_SERVER_ROOT}/libflv/source/*.c"
        "${MEDIA_SERVER_ROOT}/libmpeg/source/*.c"
        "${MEDIA_SERVER_ROOT}/libmov/source/*.c"
        "${MEDIA_SERVER_ROOT}/sdk/source/*.c"
)
add_library(mediaserver STATIC ${MEDIA_SERVER_SOURCES})
target_include_directories(mediaserver PUBLIC
        "${MEDIA_SERVER_ROOT}/librtmp/include"
        "${MEDIA_SERVER_ROOT}/libflv/include"
        "${MEDIA_SERVER_ROOT}/libmpeg/include"
        "${MEDIA_SERVER_ROOT}/libmov/include"
        "${MEDIA_SERVER_ROOT}/sdk/include"
)
# Flags every consumer of the vendored code sets (see UPSTREAM.md): NDEBUG because
# upstream asserts fire on server input, _IETF_HMAC_ for the digest handshake.
target_compile_definitions(mediaserver PRIVATE NDEBUG _IETF_HMAC_)
# Upstream C code is not warning-clean; keep its warnings out of our build log.
target_compile_options(mediaserver PRIVATE -w)
set_target_properties(mediaserver PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_library(LOG_LIB log)
# VisionCamera's prefab: the CameraOutput spec `CameraLayer.output` returns
# (<VisionCamera/HybridCameraOutputSpec.hpp>, <VisionCamera/JHybridCameraOutputSpec.hpp>).
find_package(react-native-vision-camera REQUIRED)

# Link all libraries together
target_link_libraries(
        ${PACKAGE_NAME}
        ${LOG_LIB}
        android # <-- Android core
        mediaserver # <-- vendored ireader/media-server
        react-native-vision-camera::VisionCamera # <-- CameraOutput base spec
)
