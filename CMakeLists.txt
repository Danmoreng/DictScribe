cmake_minimum_required(VERSION 3.26)
project(DictScribe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DICTSCRIBE_BUILD_UI "Build the Skia desktop UI" OFF)

enable_testing()

add_library(dictscribe_protocol STATIC
    src/protocol/jsonl_protocol.cpp
)
target_include_directories(dictscribe_protocol
    PUBLIC include
    PRIVATE third_party/nlohmann-json/single_include
)

add_executable(dictscribe-protocol-tests
    tests/jsonl_protocol_test.cpp
)
target_link_libraries(dictscribe-protocol-tests PRIVATE dictscribe_protocol)
target_include_directories(dictscribe-protocol-tests PRIVATE
    third_party/nlohmann-json/single_include
)
add_test(NAME dictscribe_protocol COMMAND dictscribe-protocol-tests)

add_executable(dictscribe-language-guard-tests
    tests/language_guard_test.cpp
    src/workers/rewrite/language_guard.cpp
)
target_include_directories(dictscribe-language-guard-tests PRIVATE src)
add_test(NAME dictscribe_language_guard COMMAND dictscribe-language-guard-tests)

add_executable(dictscribe-dictation-normalizer-tests
    tests/dictation_normalizer_test.cpp
    src/workers/rewrite/dictation_normalizer.cpp
)
target_include_directories(dictscribe-dictation-normalizer-tests PRIVATE src)
add_test(NAME dictscribe_dictation_normalizer COMMAND dictscribe-dictation-normalizer-tests)

add_executable(dictscribe-technical-literal-tests
    tests/technical_literals_test.cpp
    src/workers/rewrite/technical_literals.cpp
)
target_include_directories(dictscribe-technical-literal-tests PRIVATE src)
add_test(NAME dictscribe_technical_literals COMMAND dictscribe-technical-literal-tests)

find_package(Threads REQUIRED)

add_executable(dictscribe-fake-asr-worker tests/fake_asr_worker.cpp)
add_executable(dictscribe-fake-rewrite-worker tests/fake_rewrite_worker.cpp)
foreach(fake_worker dictscribe-fake-asr-worker dictscribe-fake-rewrite-worker)
    target_include_directories(${fake_worker} PRIVATE third_party/nlohmann-json/single_include)
endforeach()

add_executable(dictscribe-app-controller-tests
    tests/app_controller_live_test.cpp
    src/app/app_controller.cpp
    src/app/worker_process.cpp
)
target_include_directories(dictscribe-app-controller-tests PRIVATE
    src
    third_party/nlohmann-json/single_include
)
target_link_libraries(dictscribe-app-controller-tests PRIVATE dictscribe_protocol Threads::Threads)
target_compile_definitions(dictscribe-app-controller-tests PRIVATE
    DICTSCRIBE_FAKE_ASR_WORKER="$<TARGET_FILE:dictscribe-fake-asr-worker>"
    DICTSCRIBE_FAKE_REWRITE_WORKER="$<TARGET_FILE:dictscribe-fake-rewrite-worker>"
)
add_test(NAME dictscribe_app_controller COMMAND dictscribe-app-controller-tests)

if(DICTSCRIBE_BUILD_UI)
    set(SKIA_DIR "" CACHE PATH "Path to the Skia source tree")
    set(SKIA_OUT_DIR "" CACHE PATH "Path to the Skia static build output")
    if(NOT EXISTS "${SKIA_DIR}/include/core/SkCanvas.h")
        message(FATAL_ERROR "SKIA_DIR does not point to a Skia source tree")
    endif()
    if(WIN32)
        if(NOT EXISTS "${SKIA_OUT_DIR}/skia.lib")
            message(FATAL_ERROR "SKIA_OUT_DIR does not contain skia.lib")
        endif()
        add_executable(dictscribe WIN32
            src/platform/win/win_main.cpp
            src/platform/win/win_overlay.cpp
            src/platform/win/win_text_injector.cpp
            src/protocol/jsonl_protocol.cpp
            src/app/app_controller.cpp
            src/app/model_discovery.cpp
            src/app/worker_process.cpp
        )
    elseif(UNIX AND NOT APPLE)
        if(NOT EXISTS "${SKIA_OUT_DIR}/libskia.a")
            message(FATAL_ERROR "SKIA_OUT_DIR does not contain libskia.a")
        endif()

        include(FetchContent)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
        )
        FetchContent_MakeAvailable(glfw)

        find_package(OpenGL REQUIRED)
        find_package(X11 REQUIRED)
        find_package(Freetype REQUIRED)
        find_package(Fontconfig REQUIRED)

        add_executable(dictscribe
            src/app/main.cpp
            src/ui/dictation_window.cpp
            src/ui/skia_surface.cpp
            src/protocol/jsonl_protocol.cpp
            src/app/app_controller.cpp
            src/app/model_discovery.cpp
            src/app/worker_process.cpp
        )
    else()
        message(FATAL_ERROR "DictScribe UI supports Windows and Linux/X11")
    endif()

    target_compile_definitions(dictscribe PRIVATE
        DICTSCRIBE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        DICTSCRIBE_RUNTIME_VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(dictscribe PRIVATE
        src
        include
        third_party/nlohmann-json/single_include
        "${SKIA_DIR}"
        "${SKIA_DIR}/include/core"
        "${SKIA_DIR}/include/config"
        "${SKIA_DIR}/include/gpu"
        "${SKIA_DIR}/include/gpu/ganesh"
        "${SKIA_DIR}/include/gpu/ganesh/gl"
        "${SKIA_DIR}/include/ports"
    )

    if(WIN32)
        target_compile_definitions(dictscribe PRIVATE UNICODE _UNICODE)
        if(MSVC)
            target_compile_options(dictscribe PRIVATE /utf-8)
            set_property(TARGET dictscribe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
        target_link_libraries(dictscribe PRIVATE
            "${SKIA_OUT_DIR}/skia.lib"
            windowscodecs.lib
            dwrite.lib
            usp10.lib
            ole32.lib
            user32.lib
            gdi32.lib
            shell32.lib
            dwmapi.lib
        )
    else()
        target_link_libraries(dictscribe PRIVATE
            glfw
            "${SKIA_OUT_DIR}/libskia.a"
            Fontconfig::Fontconfig
            Freetype::Freetype
            OpenGL::GL
            X11::X11
            Threads::Threads
            ${CMAKE_DL_LIBS}
        )
    endif()
endif()
