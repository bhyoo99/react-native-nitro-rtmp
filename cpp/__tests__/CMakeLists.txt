# Host build for the platform independent core (cpp/nitrortmp) and its tests.
# Not used by the iOS pod or the Android module; see scripts/test-cpp.sh.
cmake_minimum_required(VERSION 3.16)
project(nitrortmp_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(NITRORTMP_SANITIZE "Build the core, the vendored code and the tests with ASan + UBSan" ON)

get_filename_component(CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MEDIA_SERVER_ROOT "${CPP_ROOT}/third_party/media-server")

if(NITRORTMP_SANITIZE AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- vendored ireader/media-server (same file set as android/CMakeLists.txt) ---
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
# NDEBUG: upstream asserts fire on server input (unknown onStatus codes, bad S0),
# which must not abort a publisher. _IETF_HMAC_: digest (Flash) handshake via the
# vendored RFC 6234 HMAC, the variant CDNs expect. -w: upstream is not warning-clean.
target_compile_definitions(mediaserver PRIVATE NDEBUG _IETF_HMAC_)
target_compile_options(mediaserver PRIVATE -w)
if(NITRORTMP_SANITIZE)
  # Upstream has benign UB (misaligned int stores in the handshake, 255 << 24 in
  # be_read_uint32) that would only clutter the log; ASan stays on for it.
  target_compile_options(mediaserver PRIVATE -fno-sanitize=undefined)
endif()

# --- the core -------------------------------------------------------------------
add_library(nitrortmp_core STATIC
  "${CPP_ROOT}/nitrortmp/RtmpUrl.cpp"
  "${CPP_ROOT}/nitrortmp/RtmpPublisher.cpp"
  "${CPP_ROOT}/nitrortmp/nitrortmp_c.cpp"
  # Encoder output helpers (AVCC -> Annex-B, ADTS header)
  "${CPP_ROOT}/nitrortmp/AnnexB.cpp"
  "${CPP_ROOT}/nitrortmp/Adts.cpp"
)
target_include_directories(nitrortmp_core PUBLIC "${CPP_ROOT}/nitrortmp")
target_link_libraries(nitrortmp_core PRIVATE mediaserver)
target_compile_options(nitrortmp_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Public headers must compile without any vendor include path,
# and the C facade header must compile as plain C (Swift and JNI include it).
add_library(nitrortmp_header_check OBJECT support/HeaderCheck.cpp support/CHeaderCheck.c)
target_include_directories(nitrortmp_header_check PRIVATE "${CPP_ROOT}/nitrortmp")
target_compile_options(nitrortmp_header_check PRIVATE -Wall -Wextra -Wpedantic -Werror)

# --- tests ------------------------------------------------------------------------
add_library(nitrortmp_test_support STATIC
  support/TestHarness.cpp
  support/Fixtures.cpp
  support/Loopback.cpp
)
target_include_directories(nitrortmp_test_support PUBLIC support)
target_link_libraries(nitrortmp_test_support PUBLIC nitrortmp_core mediaserver)
target_compile_definitions(nitrortmp_test_support PUBLIC
  NITRORTMP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  NITRORTMP_SPEC_PATH="${CPP_ROOT}/../src/specs/RtmpPublisher.nitro.ts"
)
target_compile_options(nitrortmp_test_support PRIVATE -Wall -Wextra)

enable_testing()

function(nitrortmp_add_test name source)
  add_executable(${name} ${source} support/TestMain.cpp)
  target_link_libraries(${name} PRIVATE nitrortmp_test_support)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  add_test(NAME ${name} COMMAND ${name})
  # 77 = every test in the binary was skipped (e.g. ffmpeg missing), shown as "Skipped" by ctest.
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endfunction()

nitrortmp_add_test(rtmp_url_test RtmpUrl.test.cpp)
nitrortmp_add_test(rtmp_publisher_test RtmpPublisher.test.cpp)
nitrortmp_add_test(loopback_test Loopback.test.cpp)
nitrortmp_add_test(golden_test Golden.test.cpp)
nitrortmp_add_test(interop_test Interop.test.cpp)
nitrortmp_add_test(capi_test CApi.test.cpp)
nitrortmp_add_test(annexb_test AnnexB.test.cpp)
nitrortmp_add_test(adts_test Adts.test.cpp)

# --- tools ------------------------------------------------------------------------
# flv_compare <actual.flv> <reference.flv>: used by scripts/verify-flv.sh.
add_executable(flv_compare tools/flv_compare.cpp)
target_link_libraries(flv_compare PRIVATE nitrortmp_test_support)
target_compile_options(flv_compare PRIVATE -Wall -Wextra)
