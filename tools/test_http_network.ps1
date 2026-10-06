param([string]$Compiler, [string[]]$Modes, [string]$IdfPath = $env:IDF_PATH)

# Native Windows UI/network checks using the actual pinned ESP-IDF HTTP sources.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/http-network'
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
if ($env:OS -ne 'Windows_NT') { throw 'This focused service adapter requires Windows.' }
if (-not $IdfPath -and (Test-Path -LiteralPath (Join-Path $root '.idf-path'))) {
    $IdfPath = (Get-Content -LiteralPath (Join-Path $root '.idf-path') -Raw).Trim()
}
if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath 'components/esp_http_client/esp_http_client.c'))) {
    throw 'Set IDF_PATH or .idf-path to the pinned ESP-IDF 5.4.2 source checkout.'
}
$versionHeader = Get-Content -LiteralPath (Join-Path $IdfPath 'components/esp_common/include/esp_idf_version.h') -Raw
if ($versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MAJOR\s+5\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MINOR\s+4\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_PATCH\s+2\s*$') {
    throw 'These regression checks require the pinned ESP-IDF 5.4.2 source checkout.'
}
$lvgl = Join-Path $root 'build/offline-ui/out/lvgl_host.lib'
if (-not (Test-Path -LiteralPath $lvgl)) { throw 'Run tools/test_offline_ui.ps1 once to build the cached Debug LVGL host library.' }
$cmake = Find-Tool 'cmake' 'CMAKE_COMMAND'
$ninja = Find-Tool 'ninja' 'CMAKE_MAKE_PROGRAM'
$python = (Get-Command python -ErrorAction Stop).Source
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
New-Item -ItemType Directory -Force -Path $build | Out-Null
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5HttpNetwork C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${TAB5_ROOT}/tests/http_network")
set(SDK "${TAB5_IDF}/components")
set(HTTP "${SDK}/esp_http_client")
set(TRANSPORT "${SDK}/tcp_transport")
set(SDK_SOURCES "${HTTP}/esp_http_client.c" "${HTTP}/lib/http_header.c" "${HTTP}/lib/http_utils.c"
                "${SDK}/http_parser/http_parser.c" "${TRANSPORT}/transport.c" "${TRANSPORT}/transport_internal.c")
add_executable(http_test "${TESTS}/ui_test.c" "${TESTS}/adapter.c" "${TESTS}/storage.c" "${TESTS}/dns_adapter.c" "${TAB5_ROOT}/tests/modbus_network/host.c"
               "${TAB5_ROOT}/main/storage_io.c"
               "${TAB5_ROOT}/main/http_tool.c" "${TAB5_ROOT}/main/http_transport.c" "${TAB5_ROOT}/main/network_resolver.c" ${SDK_SOURCES})
target_include_directories(http_test PRIVATE "${TESTS}/stubs" "${TESTS}" "${TAB5_ROOT}/tests/modbus_network/stubs"
    "${TAB5_ROOT}/tests/modbus_network" "${TAB5_ROOT}/main" "${TAB5_ROOT}/managed_components/lvgl__lvgl"
    "${HTTP}/include" "${HTTP}/lib/include" "${SDK}/http_parser" "${TRANSPORT}/include"
    "${TRANSPORT}/private_include" "${SDK}/mqtt/esp-mqtt/host_test/mocks/include")
target_compile_definitions(http_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304
    LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
target_compile_options(http_test PRIVATE -Wall -Wextra -Werror -include "${TESTS}/compat.h")
set_source_files_properties(${SDK_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-enum-conversion;-include;${TESTS}/track.h")
set_source_files_properties("${TAB5_ROOT}/main/http_tool.c" "${TAB5_ROOT}/main/storage_io.c"
    PROPERTIES COMPILE_OPTIONS "-include;${TESTS}/storage_redirect.h")
set_property(SOURCE "${TAB5_ROOT}/main/storage_io.c" APPEND PROPERTY COMPILE_OPTIONS -Wno-macro-redefined)
target_link_libraries(http_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build 'CMakeLists.txt')
& $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DTAB5_IDF=$($IdfPath.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE) { throw 'HTTP host configuration failed.' }
& $cmake --build "$build/out" -j 1
if ($LASTEXITCODE) { throw 'HTTP host build failed.' }
& $python (Join-Path $root 'tests/http_network/run.py') --executable "$build/out/http_test.exe" --output $build @Modes
if ($LASTEXITCODE) { throw 'HTTP network checks failed.' }
