param([string]$Compiler, [string[]]$Modes, [string]$IdfPath = $env:IDF_PATH)

# Actual MQTT app/LVGL/storage with controlled SDK and file stalls; no broker.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/mqtt-ui'
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
if (-not $IdfPath -and (Test-Path -LiteralPath (Join-Path $root '.idf-path'))) {
    $IdfPath = (Get-Content -LiteralPath (Join-Path $root '.idf-path') -Raw).Trim()
}
if (-not $IdfPath -or -not (Test-Path -LiteralPath (Join-Path $IdfPath 'components/http_parser/http_parser.c'))) {
    throw 'Set IDF_PATH or .idf-path to the pinned ESP-IDF 5.4.2 source checkout.'
}
foreach ($source in @('lib/mqtt_msg.c', 'lib/include/mqtt_msg.h', 'lib/include/mqtt_config.h', 'lib/include/platform.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $IdfPath "components/mqtt/esp-mqtt/$source"))) {
        throw "Missing pinned MQTT codec source: $source. Use the ESP-IDF 5.4.2 checkout or the current native CI source artifact."
    }
}
$versionHeader = Get-Content -LiteralPath (Join-Path $IdfPath 'components/esp_common/include/esp_idf_version.h') -Raw
if ($versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MAJOR\s+5\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MINOR\s+4\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_PATCH\s+2\s*$') {
    throw 'These regression checks require the pinned ESP-IDF 5.4.2 source checkout.'
}
if (-not (Test-Path -LiteralPath (Join-Path $root 'build/offline-ui/out/lvgl_host.lib'))) {
    throw 'Run tools/test_offline_ui.ps1 once to build the cached Debug LVGL host library.'
}
$cmake = Find-Tool 'cmake' 'CMAKE_COMMAND'
$ninja = Find-Tool 'ninja' 'CMAKE_MAKE_PROGRAM'
$python = (Get-Command python -ErrorAction Stop).Source
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$mqttTemporaryPath = Join-Path $build 'temp'
New-Item -ItemType Directory -Force -Path $mqttTemporaryPath | Out-Null
$savedMqttTemp = $env:TEMP
$savedMqttTmp = $env:TMP
try {
    $env:TEMP = $mqttTemporaryPath
    $env:TMP = $mqttTemporaryPath
    @'
cmake_minimum_required(VERSION 3.20)
project(Tab5MqttUi C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${TAB5_ROOT}/tests/mqtt_ui")
set(HOST "${TAB5_ROOT}/tests/modbus_network")
add_executable(mqtt_test "${TESTS}/ui_test.c" "${TESTS}/adapter.c" "${HOST}/host.c"
    "${TAB5_ROOT}/main/mqtt_tool.c" "${TAB5_ROOT}/main/storage_io.c" "${TAB5_ROOT}/main/payload_clipboard.c"
    "${TAB5_ROOT}/main/byte_tool.c" "${TAB5_ROOT}/main/byte_data.c"
    "${IDF_ROOT}/components/http_parser/http_parser.c"
    "${IDF_ROOT}/components/mqtt/esp-mqtt/lib/mqtt_msg.c")
target_include_directories(mqtt_test PRIVATE "${TESTS}/stubs" "${TESTS}"
    "${TAB5_ROOT}/tests/http_network/stubs" "${HOST}/stubs" "${HOST}"
    "${TAB5_ROOT}/main" "${TAB5_ROOT}/managed_components/lvgl__lvgl"
    "${IDF_ROOT}/components/http_parser" "${IDF_ROOT}/components/mqtt/esp-mqtt/lib/include")
target_compile_definitions(mqtt_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304
    LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS
    CONFIG_MQTT_PROTOCOL_311=1 CONFIG_MQTT_MSG_ID_INCREMENTAL=0)
target_compile_options(mqtt_test PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${TAB5_ROOT}/main/mqtt_tool.c" "${TAB5_ROOT}/main/storage_io.c"
    PROPERTIES COMPILE_OPTIONS "-include;${TESTS}/storage_redirect.h")
set_property(SOURCE "${TAB5_ROOT}/main/storage_io.c" APPEND PROPERTY COMPILE_OPTIONS -Wno-macro-redefined)
set_source_files_properties("${IDF_ROOT}/components/mqtt/esp-mqtt/lib/mqtt_msg.c"
    PROPERTIES COMPILE_OPTIONS "-include;${TESTS}/codec_platform.h;-Wno-sign-compare")
target_link_libraries(mqtt_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build 'CMakeLists.txt')
    & $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" `
        "-DIDF_ROOT=$($IdfPath.Replace('\', '/'))" `
        "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE) { throw 'MQTT UI configuration failed.' }
    & $cmake --build "$build/out" -j 1
    if ($LASTEXITCODE) { throw 'MQTT UI compilation failed.' }
    & $python (Join-Path $root 'tests/mqtt_ui/run.py') --executable "$build/out/mqtt_test.exe" --output $build @Modes
    if ($LASTEXITCODE) { throw 'MQTT UI checks failed.' }
} finally {
    $env:TEMP = $savedMqttTemp
    $env:TMP = $savedMqttTmp
}
