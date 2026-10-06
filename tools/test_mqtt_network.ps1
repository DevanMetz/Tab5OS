param([string]$Compiler, [string[]]$Modes, [string]$IdfPath = $env:IDF_PATH, [switch]$BuildOnly)

# Unmodified pinned ESP-MQTT client plus the actual console/storage over loopback TCP.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/mqtt-network'
if ($env:OS -ne 'Windows_NT') { throw 'This native service adapter requires Windows.' }
if (-not $IdfPath -and (Test-Path -LiteralPath (Join-Path $root '.idf-path'))) {
    $IdfPath = (Get-Content -LiteralPath (Join-Path $root '.idf-path') -Raw).Trim()
}
$sources = @('mqtt/esp-mqtt/mqtt_client.c', 'mqtt/esp-mqtt/lib/mqtt_msg.c',
    'mqtt/esp-mqtt/lib/mqtt_outbox.c', 'mqtt/esp-mqtt/include/mqtt_client.h',
    'mqtt/esp-mqtt/include/mqtt_supported_features.h', 'mqtt/esp-mqtt/lib/include/mqtt_client_priv.h',
    'mqtt/esp-mqtt/lib/include/mqtt_outbox.h', 'mqtt/esp-mqtt/lib/include/mqtt_msg.h',
    'mqtt/esp-mqtt/lib/include/mqtt_config.h', 'mqtt/esp-mqtt/lib/include/platform.h',
    'mqtt/esp-mqtt/host_test/mocks/include/sys/queue.h', 'tcp_transport/transport.c',
    'tcp_transport/transport_internal.c', 'tcp_transport/include/esp_transport.h',
    'tcp_transport/include/esp_transport_tcp.h', 'tcp_transport/include/esp_transport_ssl.h',
    'tcp_transport/include/esp_transport_ws.h', 'tcp_transport/private_include/esp_transport_internal.h',
    'http_parser/http_parser.c', 'http_parser/http_parser.h', 'esp_common/include/esp_idf_version.h')
foreach ($source in $sources) {
    if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath "components/$source"))) {
        throw "Missing pinned MQTT client source: $source. Set IDF_PATH or .idf-path to ESP-IDF 5.4.2."
    }
}
$version = Get-Content -LiteralPath (Join-Path $IdfPath 'components/esp_common/include/esp_idf_version.h') -Raw
foreach ($part in @{ MAJOR = 5; MINOR = 4; PATCH = 2 }.GetEnumerator()) {
    if ($version -notmatch "(?m)^#define ESP_IDF_VERSION_$($part.Key)\s+$($part.Value)\s*$") {
        throw 'These native checks require ESP-IDF 5.4.2.'
    }
}
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ninja = (Get-Command ninja -ErrorAction Stop).Source
$python = (Get-Command python -ErrorAction Stop).Source
if (-not (Test-Path -LiteralPath (Join-Path $root 'build/offline-ui/out/lvgl_host.lib'))) {
    throw 'Run tools/test_offline_ui.ps1 once to build the cached Debug LVGL host library.'
}
$temporary = Join-Path $build 'temp'
New-Item -ItemType Directory -Force -Path $temporary | Out-Null
$savedTemp = $env:TEMP; $savedTmp = $env:TMP
try {
    $env:TEMP = $temporary; $env:TMP = $temporary
    @'
cmake_minimum_required(VERSION 3.20)
project(Tab5MqttNetwork C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${TAB5_ROOT}/tests/mqtt_network")
set(SDK "${IDF_ROOT}/components")
set(MQTT "${SDK}/mqtt/esp-mqtt")
set(TRANSPORT "${SDK}/tcp_transport")
set(SDK_SOURCES "${MQTT}/mqtt_client.c" "${MQTT}/lib/mqtt_msg.c" "${MQTT}/lib/mqtt_outbox.c"
    "${SDK}/http_parser/http_parser.c" "${TRANSPORT}/transport.c" "${TRANSPORT}/transport_internal.c")
add_library(mqtt_native STATIC "${TESTS}/adapter.c"
    "${TAB5_ROOT}/tests/modbus_network/host.c" ${SDK_SOURCES})
target_include_directories(mqtt_native PUBLIC "${TESTS}/stubs" "${TESTS}"
    "${TAB5_ROOT}/tests/http_network/stubs" "${TAB5_ROOT}/tests/modbus_network"
    "${MQTT}/include" "${MQTT}/lib/include" "${MQTT}/host_test/mocks/include"
    "${SDK}/esp_common/include" "${SDK}/http_parser"
    "${TRANSPORT}/include" "${TRANSPORT}/private_include"
    "${TAB5_ROOT}/tests/mqtt_ui/stubs" "${TAB5_ROOT}/tests/modbus_network/stubs"
    "${TAB5_ROOT}/main" "${TAB5_ROOT}/managed_components/lvgl__lvgl")
target_compile_definitions(mqtt_native PUBLIC _CRT_SECURE_NO_WARNINGS
    LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304
    LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0)
target_compile_options(mqtt_native PUBLIC -Wall -Wextra -Werror -include "${TESTS}/compat.h")
set_source_files_properties(${SDK_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-sign-compare;-Wno-unused-parameter;-Wno-enum-conversion;-include;${TESTS}/track.h")
target_link_libraries(mqtt_native PUBLIC ws2_32)
add_executable(mqtt_network "${TESTS}/client_test.c")
target_link_libraries(mqtt_network PRIVATE mqtt_native)
add_executable(mqtt_app_network "${TESTS}/app_test.c" "${TESTS}/app_services.c"
    "${TAB5_ROOT}/main/mqtt_tool.c" "${TAB5_ROOT}/main/storage_io.c" "${TAB5_ROOT}/main/payload_clipboard.c"
    "${TAB5_ROOT}/main/byte_tool.c" "${TAB5_ROOT}/main/byte_data.c")
set_source_files_properties("${TAB5_ROOT}/main/mqtt_tool.c" "${TAB5_ROOT}/main/storage_io.c"
    PROPERTIES COMPILE_OPTIONS "-include;${TESTS}/storage_redirect.h")
set_property(SOURCE "${TAB5_ROOT}/main/storage_io.c" APPEND PROPERTY COMPILE_OPTIONS -Wno-macro-redefined)
target_link_libraries(mqtt_app_network PRIVATE mqtt_native "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib")
'@ | Set-Content -LiteralPath (Join-Path $build 'CMakeLists.txt')
    & $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" `
        "-DIDF_ROOT=$($IdfPath.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" `
        "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE) { throw 'MQTT client native configuration failed.' }
    & $cmake --build "$build/out" -j 1
    if ($LASTEXITCODE) { throw 'MQTT client native compilation failed.' }
    if (-not $BuildOnly) {
        & $python (Join-Path $root 'tests/mqtt_network/run.py') --executable "$build/out/mqtt_network.exe" `
            --app-executable "$build/out/mqtt_app_network.exe" --output $build @Modes
        if ($LASTEXITCODE) { throw 'MQTT client network checks failed.' }
    }
} finally { $env:TEMP = $savedTemp; $env:TMP = $savedTmp }
