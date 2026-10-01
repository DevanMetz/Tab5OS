param(
    [string]$Compiler,
    [switch]$Snapshots,
    [switch]$SpiOnly,
    [switch]$I2cOnly
)

# Optional host integration test. Requires downloaded managed LVGL, a native C
# compiler, CMake, and Ninja; no device, network, SDK activation, or window needed.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/offline-ui"
$cache = Join-Path $root "build/CMakeCache.txt"
if ($SpiOnly -and $I2cOnly) { throw 'Choose either -SpiOnly or -I2cOnly.' }
if (($SpiOnly -or $I2cOnly) -and -not ((Test-Path -LiteralPath "$build/out/lvgl_host.lib") -or
                      (Test-Path -LiteralPath "$build/out/liblvgl_host.a"))) {
    throw "Focused UI checks require the cached LVGL library. Run the full UI build when appropriate."
}

function Find-Tool([string]$Name, [string]$CacheKey) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    if (Test-Path -LiteralPath $cache) {
        $line = Get-Content -LiteralPath $cache | Where-Object { $_ -match "^${CacheKey}:[^=]+=" } | Select-Object -First 1
        if ($line) {
            $path = ($line -split '=', 2)[1]
            if (Test-Path -LiteralPath $path) { return $path }
        }
    }
    throw "$Name not found. Add it to PATH or build firmware once to populate build/CMakeCache.txt."
}

$cmake = Find-Tool "cmake" "CMAKE_COMMAND"
$ninja = Find-Tool "ninja" "CMAKE_MAKE_PROGRAM"
if (-not $Compiler) {
    $command = Get-Command clang, gcc, cc -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) { throw "Install a native C compiler or pass -Compiler <path>." }
    $Compiler = $command.Source
}
if (-not (Test-Path -LiteralPath (Join-Path $root "managed_components/lvgl__lvgl/lvgl.h"))) {
    throw "LVGL sources are missing. Run the normal firmware build to fetch pinned dependencies first."
}
New-Item -ItemType Directory -Force -Path $build | Out-Null
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5OfflineUI C)
set(CMAKE_C_STANDARD 11)
file(GLOB_RECURSE LVGL_SOURCES "${TAB5_ROOT}/managed_components/lvgl__lvgl/src/*.c")
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_host PUBLIC "${TAB5_ROOT}/managed_components/lvgl__lvgl" "${TAB5_ROOT}/main")
# Match the firmware allocator, fonts, and builtin formatting (no float support).
target_compile_definitions(lvgl_host PUBLIC LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS "LV_ASSERT_HANDLER={abort()\;}" "LV_ASSERT_HANDLER_INCLUDE=<stdlib.h>")
add_executable(offline_ui_test "${TAB5_ROOT}/tests/offline_ui_test.c" "${TAB5_ROOT}/main/electronics_tool.c" "${TAB5_ROOT}/main/electronics_math.c" "${TAB5_ROOT}/main/byte_tool.c" "${TAB5_ROOT}/main/byte_data.c" "${TAB5_ROOT}/main/subnet_tool.c" "${TAB5_ROOT}/main/subnet_data.c" "${TAB5_ROOT}/main/resistor_tool.c" "${TAB5_ROOT}/main/resistor_data.c" "${TAB5_ROOT}/main/ipv4_data.c")
target_compile_options(offline_ui_test PRIVATE -Wall -Wextra -Werror)
target_sources(offline_ui_test PRIVATE "${TAB5_ROOT}/main/payload_clipboard.c")
target_sources(offline_ui_test PRIVATE "${TAB5_ROOT}/main/spi_tool.c" "${TAB5_ROOT}/tests/spi_host/spi_adapter.c")
target_sources(offline_ui_test PRIVATE "${TAB5_ROOT}/main/i2c_result.c")
target_include_directories(offline_ui_test PRIVATE "${TAB5_ROOT}/tests/spi_host")
target_link_libraries(offline_ui_test PRIVATE lvgl_host)
'@ | Set-Content -LiteralPath (Join-Path $build "CMakeLists.txt")

& $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE) { throw "Host UI configuration failed." }
if ($SpiOnly -or $I2cOnly) {
    $planned = & $ninja -C "$build/out" -n offline_ui_test
    if ($LASTEXITCODE) { throw "Could not inspect the focused UI build plan." }
    if ($planned -match 'lvgl_host') {
        throw "The cached LVGL library needs rebuilding. Defer this check until a library build is appropriate."
    }
}
& $cmake --build "$build/out" -j 1
if ($LASTEXITCODE) { throw "Host UI build failed." }
$executable = Join-Path $build "out/offline_ui_test.exe"
if (-not (Test-Path -LiteralPath $executable)) { $executable = Join-Path $build "out/offline_ui_test" }
$testArgs = @()
if ($SpiOnly) { $testArgs += '--spi-only' }
if ($I2cOnly) { $testArgs += '--i2c-only' }
if ($Snapshots) { $testArgs += $build }
& $executable @testArgs
if ($LASTEXITCODE) { throw "Host UI checks failed with exit code $LASTEXITCODE." }
if ($Snapshots) { Write-Host "PPM screenshots: $build" }
