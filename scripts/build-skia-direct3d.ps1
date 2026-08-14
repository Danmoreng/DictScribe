param(
    [ValidateRange(1, 64)]
    [int]$Jobs = [Math]::Min([Environment]::ProcessorCount, 8),
    [string]$SkiaDir = $env:DICTSCRIBE_SKIA_DIR
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SkiaDir)) {
    $SkiaDir = Join-Path (Split-Path -Parent $ProjectRoot) `
        "simple-markdown-viewer\third_party\skia"
}

$gn = Join-Path $SkiaDir "bin\gn.exe"
$output = Join-Path $SkiaDir "out\Direct3D"
if (-not (Test-Path $gn) -or
    -not (Test-Path (Join-Path $SkiaDir "include\core\SkCanvas.h"))) {
    throw "Skia was not found at $SkiaDir. Set DICTSCRIBE_SKIA_DIR or pass -SkiaDir."
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw "ninja is required to build Skia."
}

$gnArgs = @(
    "is_official_build=true",
    "is_debug=false",
    "skia_enable_ganesh=true",
    "skia_use_direct3d=true",
    "skia_use_gl=false",
    "skia_use_vulkan=false",
    "skia_use_metal=false",
    "skia_use_system_libpng=false",
    "skia_use_system_libwebp=false",
    "skia_use_system_libjpeg_turbo=false",
    "skia_use_system_zlib=false",
    "skia_use_system_icu=false",
    "skia_use_system_harfbuzz=false",
    "skia_use_expat=true",
    "skia_use_system_expat=false",
    "skia_use_libpng_encode=false",
    "skia_use_libjpeg_turbo_encode=false",
    "skia_use_libwebp_encode=false",
    "skia_enable_pdf=true",
    "skia_enable_skottie=false",
    "skia_use_icu=true",
    "skia_enable_skunicode=true",
    "skia_use_harfbuzz=true",
    "skia_enable_skshaper=true",
    "skia_enable_svg=true",
    "skia_use_piex=false"
) -join " "

Push-Location $SkiaDir
try {
    & $gn gen $output "--args=$gnArgs"
    if ($LASTEXITCODE -ne 0) {
        throw "Skia GN configuration failed with exit code $LASTEXITCODE."
    }
    & ninja -C $output -j $Jobs skia
    if ($LASTEXITCODE -ne 0) {
        throw "Skia Direct3D build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

Write-Host "Skia Direct3D build is ready at $output"
