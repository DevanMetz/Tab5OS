param([string]$Compiler, [string[]]$Modes)

# Actual UI/workers, native Windows threads, real UDP exchanges with loopback peers.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/device-network"
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
if ($env:OS -ne "Windows_NT") { throw "This focused adapter requires Windows; pure NTP/WoL tests run on other hosts." }
$cmake = Find-Tool "cmake" "CMAKE_COMMAND"
$ninja = Find-Tool "ninja" "CMAKE_MAKE_PROGRAM"
$python = (Get-Command python -ErrorAction Stop).Source
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
if (-not (Test-Path -LiteralPath (Join-Path $root "build/offline-ui/out/lvgl_host.lib"))) {
    & (Join-Path $PSScriptRoot "test_offline_ui.ps1") -Compiler $Compiler
}
New-Item -ItemType Directory -Force -Path $build | Out-Null
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5DeviceNetwork C)
set(CMAKE_C_STANDARD 11)
set(HOST "${TAB5_ROOT}/tests/modbus_network")
set(TESTS "${TAB5_ROOT}/tests/device_network")
add_executable(device_test "${TESTS}/device_test.c" "${HOST}/host.c" "${TAB5_ROOT}/main/ntp_tool.c" "${TAB5_ROOT}/main/ntp_data.c" "${TAB5_ROOT}/main/wol_tool.c" "${TAB5_ROOT}/main/wol_data.c" "${TAB5_ROOT}/main/udp_tool.c" "${TAB5_ROOT}/main/byte_data.c" "${TAB5_ROOT}/main/ipv4_data.c")
target_include_directories(device_test PRIVATE "${TAB5_ROOT}/managed_components/lvgl__lvgl" "${TAB5_ROOT}/main" "${TESTS}/stubs" "${HOST}/stubs" "${HOST}")
target_compile_definitions(device_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
target_compile_options(device_test PRIVATE -Wall -Wextra -Werror)
target_sources(device_test PRIVATE "${TAB5_ROOT}/main/payload_clipboard.c" "${TAB5_ROOT}/main/byte_tool.c")
target_link_libraries(device_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build "CMakeLists.txt")
& $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE) { throw "Device host configuration failed." }
& $cmake --build "$build/out" -j 4
if ($LASTEXITCODE) { throw "Device host build failed." }
& $python (Join-Path $root "tests/device_network/run.py") --executable "$build/out/device_test.exe" --output $build @Modes
if ($LASTEXITCODE) { throw "UDP loopback checks failed." }
