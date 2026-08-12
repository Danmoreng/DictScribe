param(
    [switch]$Clean,
    [switch]$Cuda,
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot "build"
$NemoBuild = Join-Path $BuildRoot "nemo-sdk"
$NemoInstall = Join-Path $BuildRoot "nemo-install"

if ($Clean -and (Test-Path $BuildRoot)) {
    Remove-Item -Recurse -Force $BuildRoot
}

$required = @(
    "third_party\NeMo-Speech.cpp\CMakeLists.txt",
    "third_party\NeMo-Speech.cpp\ggml\CMakeLists.txt",
    "third_party\llama.cpp\CMakeLists.txt",
    "third_party\miniaudio\miniaudio.c",
    "third_party\nlohmann-json\single_include\nlohmann\json.hpp"
)
foreach ($relative in $required) {
    if (-not (Test-Path (Join-Path $ProjectRoot $relative))) {
        throw "Missing dependency file: $relative. Initialize the Git submodules first."
    }
}

$generator = @()
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $generator = @("-G", "Ninja")
}

cmake -S $ProjectRoot -B (Join-Path $BuildRoot "core") @generator `
    -DCMAKE_BUILD_TYPE=Debug
cmake --build (Join-Path $BuildRoot "core") --parallel

$nemoArgs = @(
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_INSTALL_PREFIX=$NemoInstall",
    "-DNEMO_SPEECH_BUILD_ASR=ON",
    "-DNEMO_SPEECH_BUILD_DIAR=OFF",
    "-DNEMO_SPEECH_BUILD_TTS=OFF",
    "-DNEMO_SPEECH_BUILD_NMT=OFF",
    "-DNEMO_SPEECH_BUILD_CLI=OFF",
    "-DNEMO_SPEECH_BUILD_HTTP=OFF",
    "-DNEMO_SPEECH_BUILD_GRPC=OFF",
    "-DNEMO_SPEECH_BUILD_TESTS=OFF",
    "-DBUILD_TESTING=OFF",
    "-DNEMO_SPEECH_BUILD_EXAMPLES=OFF",
    "-DNEMO_SPEECH_BUILD_TOOLS=OFF",
    "-DNEMO_SPEECH_WITH_FLASHLIGHT=OFF",
    "-DGGML_NATIVE=OFF",
    "-DGGML_OPENMP=OFF"
)

if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain)) {
        throw "vcpkg toolchain not found at $toolchain"
    }
    $nemoArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    $nemoArgs += "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"
}

if ($Cuda) {
    $bash = Get-Command bash -ErrorAction Stop
    & $bash.Source (Join-Path $ProjectRoot "third_party\NeMo-Speech.cpp\scripts\apply-ggml-patches.sh")
    $nemoArgs += "-DGGML_CUDA=ON", "-DNEMO_SPEECH_GGML_PATCHED=ON"
} else {
    $nemoArgs += "-DGGML_CUDA=OFF", "-DNEMO_SPEECH_GGML_PATCHED=OFF"
}

cmake -S (Join-Path $ProjectRoot "third_party\NeMo-Speech.cpp") -B $NemoBuild `
    @generator @nemoArgs
cmake --build $NemoBuild --config Release --target nemo_speech_asr_c --parallel
cmake --install $NemoBuild --config Release

cmake -S (Join-Path $ProjectRoot "cmake\asr-worker") `
    -B (Join-Path $BuildRoot "asr-worker") @generator `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$NemoInstall"
cmake --build (Join-Path $BuildRoot "asr-worker") --config Release --parallel

$cudaFlag = if ($Cuda) { "ON" } else { "OFF" }
cmake -S (Join-Path $ProjectRoot "cmake\rewrite-worker") `
    -B (Join-Path $BuildRoot "rewrite-worker") @generator `
    -DCMAKE_BUILD_TYPE=Release `
    "-DDICTSCRIBE_ENABLE_CUDA=$cudaFlag"
cmake --build (Join-Path $BuildRoot "rewrite-worker") --config Release --parallel

Write-Host "DictScribe native bootstrap build completed."
