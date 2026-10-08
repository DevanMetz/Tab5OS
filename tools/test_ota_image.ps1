param([string]$Compiler, [string[]]$Modes, [string]$IdfPath = $env:IDF_PATH)

# Unmodified pinned SDK OTA/HTTP/SHA code against loopback and a RAM partition.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/ota-image'
$cache = Join-Path $root 'build/CMakeCache.txt'
function Find-Tool([string]$Name, [string]$CacheKey) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    if (Test-Path -LiteralPath $cache) {
        $line = Get-Content -LiteralPath $cache | Where-Object { $_ -match "^${CacheKey}:[^=]+=" } | Select-Object -First 1
        if ($line) { return ($line -split '=', 2)[1] }
    }
    throw "$Name not found. Add it to PATH."
}
if ($env:OS -ne 'Windows_NT') { throw 'The OTA image socket adapter requires Windows.' }
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
if (-not $IdfPath -and (Test-Path -LiteralPath (Join-Path $root '.idf-path'))) {
    $IdfPath = (Get-Content -LiteralPath (Join-Path $root '.idf-path') -Raw).Trim()
}
if (-not $IdfPath) { throw 'Set IDF_PATH, -IdfPath or .idf-path to ESP-IDF 5.4.2 sources.' }
foreach ($source in @('components/esp_common/include/esp_idf_version.h',
                     'components/esp_https_ota/src/esp_https_ota.c', 'components/esp_https_ota/include/esp_https_ota.h',
                     'components/esp_app_format/include/esp_app_desc.h', 'components/bootloader_support/include/esp_app_format.h',
                     'components/esp_http_client/esp_http_client.c', 'components/esp_http_client/include/esp_http_client.h',
                     'components/esp_http_client/lib/http_header.c', 'components/esp_http_client/lib/http_utils.c',
                     'components/esp_http_client/lib/include/http_header.h', 'components/esp_http_client/lib/include/http_utils.h',
                     'components/esp_http_client/lib/include/http_auth.h',
                     'components/http_parser/http_parser.c', 'components/http_parser/http_parser.h',
                     'components/tcp_transport/transport.c', 'components/tcp_transport/transport_internal.c',
                     'components/tcp_transport/include/esp_transport.h', 'components/tcp_transport/include/esp_transport_tcp.h',
                     'components/tcp_transport/include/esp_transport_ssl.h', 'components/tcp_transport/include/esp_transport_ws.h',
                     'components/tcp_transport/private_include/esp_transport_internal.h',
                     'components/mqtt/esp-mqtt/host_test/mocks/include/sys/queue.h',
                     'components/json/cJSON/cJSON.c', 'components/json/cJSON/cJSON.h',
                     'components/mbedtls/mbedtls/library/sha256.c', 'components/mbedtls/mbedtls/library/common.h',
                     'components/mbedtls/mbedtls/library/alignment.h',
                     'components/mbedtls/mbedtls/include/mbedtls/sha256.h',
                     'components/mbedtls/mbedtls/include/mbedtls/build_info.h',
                     'components/mbedtls/mbedtls/include/mbedtls/config_adjust_legacy_crypto.h',
                     'components/mbedtls/mbedtls/include/mbedtls/config_adjust_x509.h',
                     'components/mbedtls/mbedtls/include/mbedtls/config_adjust_ssl.h',
                     'components/mbedtls/mbedtls/include/mbedtls/check_config.h',
                     'components/mbedtls/mbedtls/include/mbedtls/private_access.h',
                     'components/mbedtls/mbedtls/include/mbedtls/platform_util.h',
                     'components/mbedtls/mbedtls/include/mbedtls/error.h',
                     'components/mbedtls/mbedtls/include/mbedtls/platform.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $IdfPath $source))) { throw "Missing pinned OTA image source: $source." }
}
$versionHeader = Get-Content -LiteralPath (Join-Path $IdfPath 'components/esp_common/include/esp_idf_version.h') -Raw
if ($versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MAJOR\s+5\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MINOR\s+4\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_PATCH\s+2\s*$') { throw 'OTA image checks require ESP-IDF 5.4.2.' }
$cmake = Find-Tool 'cmake' 'CMAKE_COMMAND'
$ninja = Find-Tool 'ninja' 'CMAKE_MAKE_PROGRAM'
$python = (Get-Command python -ErrorAction Stop).Source
$otaImageTemporaryPath = Join-Path $build 'temp'
New-Item -ItemType Directory -Force -Path $otaImageTemporaryPath | Out-Null
$savedOtaImageTemp = $env:TEMP
$savedOtaImageTmp = $env:TMP
try {
    $env:TEMP = $otaImageTemporaryPath
    $env:TMP = $otaImageTemporaryPath
    @'
cmake_minimum_required(VERSION 3.20)
project(Tab5OtaImage C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${TAB5_ROOT}/tests/ota_image")
set(HTTP_TESTS "${TAB5_ROOT}/tests/http_network")
set(SDK "${TAB5_IDF}/components")
set(HTTP "${SDK}/esp_http_client")
set(TRANSPORT "${SDK}/tcp_transport")
set(MBEDTLS "${SDK}/mbedtls/mbedtls")
set(SDK_SOURCES "${HTTP}/esp_http_client.c" "${HTTP}/lib/http_header.c" "${HTTP}/lib/http_utils.c"
    "${SDK}/http_parser/http_parser.c" "${TRANSPORT}/transport.c" "${TRANSPORT}/transport_internal.c"
    "${SDK}/esp_https_ota/src/esp_https_ota.c" "${SDK}/json/cJSON/cJSON.c" "${MBEDTLS}/library/sha256.c")
add_executable(image_test "${TESTS}/image_test.c" "${HTTP_TESTS}/adapter.c" "${HTTP_TESTS}/dns_adapter.c"
    "${TAB5_ROOT}/tests/modbus_network/host.c" "${TAB5_ROOT}/main/ota_manifest.c"
    "${TAB5_ROOT}/main/http_transport.c" "${TAB5_ROOT}/main/network_resolver.c" ${SDK_SOURCES})
target_include_directories(image_test PRIVATE "${TESTS}/stubs" "${HTTP_TESTS}/stubs" "${HTTP_TESTS}"
    "${TAB5_ROOT}/tests/modbus_network/stubs" "${TAB5_ROOT}/tests/modbus_network" "${TAB5_ROOT}/main"
    "${HTTP}/include" "${HTTP}/lib/include" "${SDK}/http_parser" "${TRANSPORT}/include"
    "${TRANSPORT}/private_include" "${SDK}/mqtt/esp-mqtt/host_test/mocks/include"
    "${SDK}/esp_https_ota/include" "${SDK}/esp_app_format/include" "${SDK}/bootloader_support/include"
    "${SDK}/json/cJSON" "${MBEDTLS}/include" "${MBEDTLS}/library")
target_compile_definitions(image_test PRIVATE _CRT_SECURE_NO_WARNINGS MBEDTLS_CONFIG_FILE="sha_config.h" HTTP_HOST_BLOCKING=1)
target_compile_options(image_test PRIVATE -Wall -Wextra -Werror -include "${HTTP_TESTS}/compat.h")
set_source_files_properties(${SDK_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-parameter;-Wno-sign-compare;-Wno-enum-conversion;-include;${HTTP_TESTS}/track.h")
set_source_files_properties("${TAB5_ROOT}/tests/modbus_network/host.c" PROPERTIES
    COMPILE_DEFINITIONS "esp_timer_get_time=ota_fixture_real_time")
set_source_files_properties("${MBEDTLS}/library/sha256.c" PROPERTIES COMPILE_DEFINITIONS
    "mbedtls_sha256_starts=ota_fixture_sha256_starts;mbedtls_sha256_update=ota_fixture_sha256_update;mbedtls_sha256_finish=ota_fixture_sha256_finish")
target_link_libraries(image_test PRIVATE ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build 'CMakeLists.txt')
    & $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DTAB5_IDF=$($IdfPath.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
    if ($LASTEXITCODE) { throw 'OTA image configuration failed.' }
    & $cmake --build "$build/out" -j 1 2>&1 |
        Tee-Object -FilePath (Join-Path $build 'compile.log')
    if ($LASTEXITCODE) { throw 'OTA image compilation failed.' }
    & $python (Join-Path $root 'tests/ota_image/run.py') --executable "$build/out/image_test.exe" --output $build @Modes
    if ($LASTEXITCODE) { throw 'OTA image checks failed.' }
} finally {
    $env:TEMP = $savedOtaImageTemp
    $env:TMP = $savedOtaImageTmp
}
