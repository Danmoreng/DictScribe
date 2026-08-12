cmake_minimum_required(VERSION 3.26)
project(dictscribe_asr_worker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(DICTSCRIBE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

find_package(NeMoSpeech REQUIRED COMPONENTS ASR)
find_package(Threads REQUIRED)

add_library(dictscribe_miniaudio STATIC
    "${DICTSCRIBE_ROOT}/third_party/miniaudio/miniaudio.c"
)
target_include_directories(dictscribe_miniaudio PUBLIC
    "${DICTSCRIBE_ROOT}/third_party/miniaudio"
)

add_executable(dictscribe-asr-worker
    "${DICTSCRIBE_ROOT}/src/protocol/jsonl_protocol.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/asr/audio_capture.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/asr/main.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/asr/runtime_controller.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/asr/transcription_engine.cpp"
)
target_include_directories(dictscribe-asr-worker PRIVATE
    "${DICTSCRIBE_ROOT}/include"
    "${DICTSCRIBE_ROOT}/src/workers/asr"
    "${DICTSCRIBE_ROOT}/third_party/nlohmann-json/single_include"
)
target_link_libraries(dictscribe-asr-worker PRIVATE
    NeMoSpeech::ASR
    dictscribe_miniaudio
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
if(UNIX)
    target_link_libraries(dictscribe-asr-worker PRIVATE m)
endif()

target_compile_definitions(dictscribe-asr-worker PRIVATE
    DICTSCRIBE_RUNTIME_VERSION="${PROJECT_VERSION}"
)

if(APPLE)
    set_target_properties(dictscribe-asr-worker PROPERTIES
        BUILD_RPATH "@loader_path/../lib"
        INSTALL_RPATH "@loader_path/../lib"
    )
elseif(UNIX)
    set_target_properties(dictscribe-asr-worker PROPERTIES
        BUILD_RPATH "$ORIGIN/../nemo-install/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

install(TARGETS dictscribe-asr-worker RUNTIME DESTINATION bin)
