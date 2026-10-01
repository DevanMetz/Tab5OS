param([string]$Compiler, [string[]]$Modes)

# Windows-only loopback integration test; no tablet, LAN peer, or SDK activation.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/modbus-network"
$cache = Join-Path $root "build/CMakeCache.txt"
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
if ($env:OS -ne "Windows_NT") { throw "This focused adapter requires Windows; use the pure Modbus parser tests on other hosts." }
$cmake = Find-Tool "cmake" "CMAKE_COMMAND"
$ninja = Find-Tool "ninja" "CMAKE_MAKE_PROGRAM"
$python = (Get-Command python -ErrorAction Stop).Source
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$lvgl = Join-Path $root "build/offline-ui/out/lvgl_host.lib"
if (-not (Test-Path -LiteralPath $lvgl)) {
    & (Join-Path $PSScriptRoot "test_offline_ui.ps1") -Compiler $Compiler
}
New-Item -ItemType Directory -Force -Path $build | Out-Null
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5ModbusNetwork C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${TAB5_ROOT}/tests/modbus_network")
add_executable(network_test "${TESTS}/network_test.c" "${TESTS}/host.c" "${TAB5_ROOT}/main/modbus_tool.c" "${TAB5_ROOT}/main/modbus_data.c" "${TAB5_ROOT}/main/ipv4_data.c" "${TAB5_ROOT}/main/byte_data.c")
target_compile_options(network_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(network_test PRIVATE "${TAB5_ROOT}/managed_components/lvgl__lvgl" "${TAB5_ROOT}/main" "${TESTS}/stubs" "${TESTS}")
target_compile_definitions(network_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(network_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build "CMakeLists.txt")
& $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE) { throw "Modbus host configuration failed." }
& $cmake --build "$build/out" -j 4
if ($LASTEXITCODE) { throw "Modbus host build failed." }
& $python (Join-Path $root "tests/modbus_network/run.py") --executable "$build/out/network_test.exe" @Modes
if ($LASTEXITCODE) { throw "Modbus loopback checks failed." }
