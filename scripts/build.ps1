param(
    [switch]$Clean,
    [switch]$Cuda,
    [switch]$SkipUi,
    [ValidateRange(1, 64)]
    [int]$Jobs = [Math]::Min([Environment]::ProcessorCount, 8),
    [string]$CudaArch = "native",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$SkiaDir = $env:DICTSCRIBE_SKIA_DIR,
    [string]$SkiaOutDir = $env:DICTSCRIBE_SKIA_OUT_DIR
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $ProjectRoot "build"
$NemoBuild = Join-Path $BuildRoot "nemo-sdk"
$NemoInstall = Join-Path $BuildRoot "nemo-install"

function Assert-NativeSuccess([string]$Action) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Action failed with exit code $LASTEXITCODE."
    }
}

$vsInstall = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
}

if (-not (Get-Command cl -ErrorAction SilentlyContinue) -and $vsInstall) {
    $devCommand = Join-Path $vsInstall "Common7\Tools\VsDevCmd.bat"
    $environmentCommand = '"' + $devCommand + '" -no_logo -arch=amd64 && set'
    & $env:ComSpec /d /s /c $environmentCommand | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot) -and $vsInstall) {
    $bundledVcpkg = Join-Path $vsInstall "VC\vcpkg"
    if (Test-Path (Join-Path $bundledVcpkg "scripts\buildsystems\vcpkg.cmake")) {
        $VcpkgRoot = $bundledVcpkg
    }
}

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

$coreArgs = @("-DCMAKE_BUILD_TYPE=Debug")
if ($SkipUi) {
    $coreArgs += "-DDICTSCRIBE_BUILD_UI=OFF"
} else {
    if ([string]::IsNullOrWhiteSpace($SkiaDir)) {
        $SkiaDir = Join-Path (Split-Path -Parent $ProjectRoot) `
            "simple-markdown-viewer\third_party\skia"
    }
    if ([string]::IsNullOrWhiteSpace($SkiaOutDir)) {
        $direct3DOutput = Join-Path $SkiaDir "out\Direct3D"
        $SkiaOutDir = if (Test-Path (Join-Path $direct3DOutput "skia.lib")) {
            $direct3DOutput
        } else {
            Join-Path $SkiaDir "out\Static"
        }
    }
    if (-not (Test-Path (Join-Path $SkiaDir "include\core\SkCanvas.h")) -or
        -not (Test-Path (Join-Path $SkiaOutDir "skia.lib"))) {
        throw "A built Windows Skia checkout is required for the desktop UI. Set DICTSCRIBE_SKIA_DIR and DICTSCRIBE_SKIA_OUT_DIR, run the neighboring simple-markdown-viewer build, or pass -SkipUi."
    }
    $coreArgs += "-DDICTSCRIBE_BUILD_UI=ON"
    $coreArgs += "-DSKIA_DIR=$SkiaDir"
    $coreArgs += "-DSKIA_OUT_DIR=$SkiaOutDir"
    $skiaArgs = Join-Path $SkiaOutDir "args.gn"
    $direct3DEnabled = Test-Path $skiaArgs -and
        (Select-String -Path $skiaArgs -Pattern '^skia_use_direct3d\s*=\s*true\s*$' -Quiet)
    $direct3DCMakeValue = if ($direct3DEnabled) { "ON" } else { "OFF" }
    $coreArgs += "-DDICTSCRIBE_SKIA_DIRECT3D=$direct3DCMakeValue"
}

cmake -S $ProjectRoot -B (Join-Path $BuildRoot "core") @generator `
    @coreArgs
Assert-NativeSuccess "Core configuration"
cmake --build (Join-Path $BuildRoot "core") --parallel $Jobs
Assert-NativeSuccess "Core build"

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
    $nemoArgs += "-DVCPKG_MANIFEST_DIR=$ProjectRoot"
}

if ($Cuda) {
    $patchScript = Join-Path $ProjectRoot `
        "third_party\NeMo-Speech.cpp\scripts\windows\apply-ggml-patches.ps1"
    & $patchScript
    Assert-NativeSuccess "NeMo GGML patch application"
    $nemoArgs += "-DGGML_CUDA=ON", "-DNEMO_SPEECH_GGML_PATCHED=ON",
        "-DCMAKE_CUDA_ARCHITECTURES=$CudaArch"
} else {
    $nemoArgs += "-DGGML_CUDA=OFF", "-DNEMO_SPEECH_GGML_PATCHED=OFF"
}

cmake -S (Join-Path $ProjectRoot "third_party\NeMo-Speech.cpp") -B $NemoBuild `
    @generator @nemoArgs
Assert-NativeSuccess "NeMo SDK configuration"
cmake --build $NemoBuild --config Release --target nemo_speech_asr_c --parallel $Jobs
Assert-NativeSuccess "NeMo SDK build"
cmake --install $NemoBuild --config Release
Assert-NativeSuccess "NeMo SDK install"

cmake -S (Join-Path $ProjectRoot "cmake\asr-worker") `
    -B (Join-Path $BuildRoot "asr-worker") @generator `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$NemoInstall"
Assert-NativeSuccess "ASR worker configuration"
cmake --build (Join-Path $BuildRoot "asr-worker") --config Release --parallel $Jobs
Assert-NativeSuccess "ASR worker build"

$cudaFlag = if ($Cuda) { "ON" } else { "OFF" }
$rewriteCudaArgs = @()
if ($Cuda) {
    $rewriteCudaArgs += "-DCMAKE_CUDA_ARCHITECTURES=$CudaArch"
}
cmake -S (Join-Path $ProjectRoot "cmake\rewrite-worker") `
    -B (Join-Path $BuildRoot "rewrite-worker") @generator `
    -DCMAKE_BUILD_TYPE=Release `
    "-DDICTSCRIBE_ENABLE_CUDA=$cudaFlag" @rewriteCudaArgs
Assert-NativeSuccess "Rewrite worker configuration"
cmake --build (Join-Path $BuildRoot "rewrite-worker") --config Release --parallel $Jobs
Assert-NativeSuccess "Rewrite worker build"

Write-Host "DictScribe native bootstrap build completed."
