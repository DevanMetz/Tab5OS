param([string]$Compiler, [string[]]$Modes)

# Actual Diagnostics/resolver/LVGL with native threads and synthetic services.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/network-ui'
$cache = Join-Path $root 'build/CMakeCache.txt'
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
if ($env:OS -ne 'Windows_NT') { throw 'This service adapter requires Windows.' }
if (-not (Test-Path -LiteralPath (Join-Path $root 'build/offline-ui/out/lvgl_host.lib'))) {
    throw 'Run tools/test_offline_ui.ps1 once to build the cached Debug LVGL host library.'
}
$cmake = Find-Tool 'cmake' 'CMAKE_COMMAND'
$ninja = Find-Tool 'ninja' 'CMAKE_MAKE_PROGRAM'
$python = (Get-Command python -ErrorAction Stop).Source
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$networkTemporaryPath = Join-Path $build 'temp'
New-Item -ItemType Directory -Force -Path $networkTemporaryPath | Out-Null
$savedNetworkTemp = $env:TEMP
$savedNetworkTmp = $env:TMP
try {
    $env:TEMP = $networkTemporaryPath
    $env:TMP = $networkTemporaryPath
    @'
cmake_minimum_required(VERSION 3.20)
project(Tab5NetworkUi C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${TAB5_ROOT}/tests/network_ui")
set(HTTP "${TAB5_ROOT}/tests/http_network")
set(HOST "${TAB5_ROOT}/tests/modbus_network")
foreach(capacity 1 4)
    add_executable(network_test_${capacity} "${TESTS}/ui_test.c" "${HOST}/host.c" "${HTTP}/dns_adapter.c"
        "${TAB5_ROOT}/main/network_tool.c" "${TAB5_ROOT}/main/network_resolver.c")
    target_include_directories(network_test_${capacity} PRIVATE "${TESTS}/stubs" "${HTTP}/stubs"
        "${HTTP}" "${HOST}/stubs" "${HOST}" "${TAB5_ROOT}/main" "${TAB5_ROOT}/managed_components/lvgl__lvgl")
    target_compile_definitions(network_test_${capacity} PRIVATE DNS_MAX_HOST_IP=${capacity}
        LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1
        LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
    target_compile_options(network_test_${capacity} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(network_test_${capacity} PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
endforeach()
'@ | Set-Content -LiteralPath (Join-Path $build 'CMakeLists.txt')
    & $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" `
        "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE) { throw 'Network UI configuration failed.' }
    & $cmake --build "$build/out" -j 1
    if ($LASTEXITCODE) { throw 'Network UI compilation failed.' }
    foreach ($capacity in @(1, 4)) {
        & $python (Join-Path $root 'tests/network_ui/run.py') --executable "$build/out/network_test_$capacity.exe" `
            --output "$build/dns-$capacity" @Modes
        if ($LASTEXITCODE) { throw "Network UI checks failed (DNS capacity=$capacity)." }
    }
} finally {
    $env:TEMP = $savedNetworkTemp
    $env:TMP = $savedNetworkTmp
}
