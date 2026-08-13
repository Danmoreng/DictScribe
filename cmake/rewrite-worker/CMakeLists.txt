cmake_minimum_required(VERSION 3.26)
project(dictscribe_rewrite_worker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

get_filename_component(DICTSCRIBE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)

option(DICTSCRIBE_ENABLE_CUDA "Enable CUDA for the rewrite worker" OFF)
set(GGML_CUDA ${DICTSCRIBE_ENABLE_CUDA} CACHE BOOL "" FORCE)

add_subdirectory("${DICTSCRIBE_ROOT}/third_party/llama.cpp" llama.cpp EXCLUDE_FROM_ALL)
if(MSVC)
    target_compile_options(llama PRIVATE /Zc:__cplusplus)
endif()

add_executable(dictscribe-rewrite-worker
    "${DICTSCRIBE_ROOT}/src/protocol/jsonl_protocol.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/rewrite/llama_rewriter.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/rewrite/main.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/rewrite/rewrite_tail_contract.cpp"
    "${DICTSCRIBE_ROOT}/src/workers/rewrite/technical_literals.cpp"
)
target_include_directories(dictscribe-rewrite-worker PRIVATE
    "${DICTSCRIBE_ROOT}/include"
    "${DICTSCRIBE_ROOT}/src/workers/rewrite"
    "${DICTSCRIBE_ROOT}/third_party/llama.cpp/include"
    "${DICTSCRIBE_ROOT}/third_party/nlohmann-json/single_include"
)
target_link_libraries(dictscribe-rewrite-worker PRIVATE llama)
target_compile_definitions(dictscribe-rewrite-worker PRIVATE
    DICTSCRIBE_RUNTIME_VERSION="${PROJECT_VERSION}"
)

install(TARGETS dictscribe-rewrite-worker RUNTIME DESTINATION bin)
