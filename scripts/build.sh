#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="$PROJECT_ROOT/build"
ENABLE_CUDA=0
CLEAN=0
BUILD_UI=1

usage() {
  echo "Usage: $0 [--clean] [--cuda] [--skip-ui]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN=1
      ;;
    --cuda)
      ENABLE_CUDA=1
      ;;
    --skip-ui)
      BUILD_UI=0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ "$CLEAN" -eq 1 ]]; then
  rm -rf "$BUILD_ROOT"
fi

for required in \
  "$PROJECT_ROOT/third_party/NeMo-Speech.cpp/CMakeLists.txt" \
  "$PROJECT_ROOT/third_party/NeMo-Speech.cpp/ggml/CMakeLists.txt" \
  "$PROJECT_ROOT/third_party/llama.cpp/CMakeLists.txt" \
  "$PROJECT_ROOT/third_party/miniaudio/miniaudio.c" \
  "$PROJECT_ROOT/third_party/nlohmann-json/single_include/nlohmann/json.hpp"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing dependency file: $required" >&2
    echo "Run: git submodule update --init" >&2
    echo "Then: git -C third_party/NeMo-Speech.cpp submodule update --init ggml" >&2
    exit 1
  fi
done

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARGS=(-G Ninja)
fi

JOBS="${DICTSCRIBE_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
NEMO_BUILD="$BUILD_ROOT/nemo-sdk"
NEMO_INSTALL="$BUILD_ROOT/nemo-install"
NEMO_DEPENDENCIES="$BUILD_ROOT/nemo-dependencies"

CORE_ARGS=(-DCMAKE_BUILD_TYPE=Debug -DDICTSCRIBE_BUILD_UI=OFF)
if [[ "$BUILD_UI" -eq 1 ]]; then
  SKIA_DIR="${DICTSCRIBE_SKIA_DIR:-$PROJECT_ROOT/third_party/skia}"
  if [[ ! -f "$SKIA_DIR/out/Static/libskia.a" ]]; then
    SIBLING_SKIA="$PROJECT_ROOT/../simple-markdown-viewer/third_party/skia"
    if [[ -f "$SIBLING_SKIA/out/Static/libskia.a" ]]; then
      SKIA_DIR="$SIBLING_SKIA"
    else
      echo "A static Skia build is required for the DictScribe UI." >&2
      echo "Set DICTSCRIBE_SKIA_DIR or build the simple-markdown-viewer sibling first." >&2
      exit 1
    fi
  fi
  CORE_ARGS+=(
    -DDICTSCRIBE_BUILD_UI=ON
    -DSKIA_DIR="$SKIA_DIR"
    -DSKIA_OUT_DIR="$SKIA_DIR/out/Static"
  )
fi

cmake -S "$PROJECT_ROOT" -B "$BUILD_ROOT/core" "${GENERATOR_ARGS[@]}" \
  "${CORE_ARGS[@]}"
cmake --build "$BUILD_ROOT/core" --parallel "$JOBS"

NEMO_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="$NEMO_INSTALL"
  -DNEMO_SPEECH_DEPENDENCY_PREFIX="$NEMO_DEPENDENCIES"
  -DNEMO_SPEECH_BUILD_ASR=ON
  -DNEMO_SPEECH_BUILD_DIAR=OFF
  -DNEMO_SPEECH_BUILD_TTS=OFF
  -DNEMO_SPEECH_BUILD_NMT=OFF
  -DNEMO_SPEECH_BUILD_CLI=OFF
  -DNEMO_SPEECH_BUILD_HTTP=OFF
  -DNEMO_SPEECH_BUILD_GRPC=OFF
  -DNEMO_SPEECH_BUILD_TESTS=OFF
  -DBUILD_TESTING=OFF
  -DNEMO_SPEECH_BUILD_EXAMPLES=OFF
  -DNEMO_SPEECH_BUILD_TOOLS=OFF
  -DNEMO_SPEECH_WITH_FLASHLIGHT=OFF
  -DGGML_NATIVE=OFF
  -DGGML_OPENMP=OFF
)

if [[ ! -f "$NEMO_DEPENDENCIES/sentencepiece/lib/libsentencepiece.a" ]]; then
  SENTENCEPIECE_COMMIT="17d7580d6407802f85855d2cc9190634e2c95624"
  SENTENCEPIECE_ROOT="$BUILD_ROOT/sentencepiece-build"
  SENTENCEPIECE_SOURCE="$SENTENCEPIECE_ROOT/source"
  SENTENCEPIECE_BUILD="$SENTENCEPIECE_ROOT/build"
  if [[ ! -d "$SENTENCEPIECE_SOURCE/.git" ]]; then
    git clone --filter=blob:none --no-checkout \
      https://github.com/google/sentencepiece.git "$SENTENCEPIECE_SOURCE"
  fi
  git -C "$SENTENCEPIECE_SOURCE" fetch --depth 1 origin "$SENTENCEPIECE_COMMIT"
  git -C "$SENTENCEPIECE_SOURCE" checkout --quiet "$SENTENCEPIECE_COMMIT"
  cmake -S "$SENTENCEPIECE_SOURCE" -B "$SENTENCEPIECE_BUILD" "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "-DCMAKE_CXX_FLAGS=-include cstdint" \
    -DSPM_BUILD_TEST=OFF \
    -DSPM_ENABLE_SHARED=OFF \
    -DSPM_ENABLE_TCMALLOC=OFF
  cmake --build "$SENTENCEPIECE_BUILD" --target sentencepiece-static --parallel "$JOBS"
  cmake -E make_directory \
    "$NEMO_DEPENDENCIES/sentencepiece/lib" \
    "$NEMO_DEPENDENCIES/sentencepiece/include"
  cmake -E copy "$SENTENCEPIECE_BUILD/src/libsentencepiece.a" \
    "$NEMO_DEPENDENCIES/sentencepiece/lib/libsentencepiece.a"
  cmake -E copy "$SENTENCEPIECE_SOURCE/src/sentencepiece_processor.h" \
    "$NEMO_DEPENDENCIES/sentencepiece/include/sentencepiece_processor.h"
fi

if [[ "$ENABLE_CUDA" -eq 1 ]]; then
  "$PROJECT_ROOT/third_party/NeMo-Speech.cpp/scripts/apply-ggml-patches.sh"
  NEMO_ARGS+=(
    -DGGML_CUDA=ON
    -DNEMO_SPEECH_GGML_PATCHED=ON
  )
else
  NEMO_ARGS+=(
    -DGGML_CUDA=OFF
    -DNEMO_SPEECH_GGML_PATCHED=OFF
  )
fi

cmake -S "$PROJECT_ROOT/third_party/NeMo-Speech.cpp" -B "$NEMO_BUILD" \
  "${GENERATOR_ARGS[@]}" "${NEMO_ARGS[@]}"
cmake --build "$NEMO_BUILD" --target nemo_speech_asr_c --parallel "$JOBS"
cmake --install "$NEMO_BUILD"

cmake -S "$PROJECT_ROOT/cmake/asr-worker" -B "$BUILD_ROOT/asr-worker" \
  "${GENERATOR_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$NEMO_INSTALL"
cmake --build "$BUILD_ROOT/asr-worker" --parallel "$JOBS"

cmake -S "$PROJECT_ROOT/cmake/rewrite-worker" -B "$BUILD_ROOT/rewrite-worker" \
  "${GENERATOR_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDICTSCRIBE_ENABLE_CUDA="$([[ "$ENABLE_CUDA" -eq 1 ]] && echo ON || echo OFF)"
cmake --build "$BUILD_ROOT/rewrite-worker" --parallel "$JOBS"

echo "DictScribe native bootstrap build completed."
echo "ASR worker: $BUILD_ROOT/asr-worker/dictscribe-asr-worker"
echo "Rewrite worker: $BUILD_ROOT/rewrite-worker/bin/dictscribe-rewrite-worker"
if [[ "$BUILD_UI" -eq 1 ]]; then
  echo "Desktop UI: $BUILD_ROOT/core/dictscribe"
fi
