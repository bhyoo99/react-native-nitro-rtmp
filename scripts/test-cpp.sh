#!/usr/bin/env bash
# Builds the C++ core and runs its host tests (cpp/__tests__).
#
# cmake is looked up on PATH first, then inside the Android SDK (the SDK ships
# cmake + ninja under $ANDROID_HOME/cmake/<version>/bin), so a React Native
# development machine needs nothing extra. ffmpeg is optional: the interop test
# is skipped without it.
#
#   scripts/test-cpp.sh                # configure + build + ctest
#   scripts/test-cpp.sh -DNITRORTMP_SANITIZE=OFF   # extra args go to cmake configure
#   NITRORTMP_BUILD_TYPE=Release scripts/test-cpp.sh
#   NITRORTMP_BUILD_ONLY=1 NITRORTMP_TARGET=flv_compare scripts/test-cpp.sh   # one target, no ctest
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${NITRORTMP_BUILD_DIR:-$ROOT/build/cpp-tests}"
BUILD_TYPE="${NITRORTMP_BUILD_TYPE:-Debug}"

find_cmake() {
  if command -v cmake >/dev/null 2>&1; then
    command -v cmake
    return 0
  fi
  local sdk
  for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk" "/usr/local/lib/android/sdk"; do
    [ -n "$sdk" ] && [ -d "$sdk/cmake" ] || continue
    local found
    found="$(ls -d "$sdk"/cmake/*/bin/cmake 2>/dev/null | sort -V | tail -1 || true)"
    if [ -n "$found" ]; then
      echo "$found"
      return 0
    fi
  done
  return 1
}

CMAKE="$(find_cmake)" || { echo "cmake not found on PATH or in an Android SDK" >&2; exit 1; }
CMAKE_BIN="$(dirname "$CMAKE")"
CTEST="$CMAKE_BIN/ctest"
[ -x "$CTEST" ] || CTEST="$(command -v ctest)"

GENERATOR=()
if [ -x "$CMAKE_BIN/ninja" ]; then
  GENERATOR=(-G Ninja "-DCMAKE_MAKE_PROGRAM=$CMAKE_BIN/ninja")
elif command -v ninja >/dev/null 2>&1; then
  GENERATOR=(-G Ninja)
fi

echo "cmake: $CMAKE"
"$CMAKE" -S "$ROOT/cpp/__tests__" -B "$BUILD_DIR" ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"
TARGET_ARGS=()
if [ -n "${NITRORTMP_TARGET:-}" ]; then
  TARGET_ARGS=(--target "$NITRORTMP_TARGET")
fi
"$CMAKE" --build "$BUILD_DIR" --parallel ${TARGET_ARGS[@]+"${TARGET_ARGS[@]}"}
if [ "${NITRORTMP_BUILD_ONLY:-0}" = "1" ]; then
  exit 0
fi
(cd "$BUILD_DIR" && "$CTEST" --output-on-failure "${CTEST_ARGS[@]:-}")
