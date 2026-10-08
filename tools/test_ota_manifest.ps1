param([string]$Compiler, [string]$IdfPath = $env:IDF_PATH)

# Actual OTA manifest fetch/check with pinned cJSON and controlled HTTP events.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/ota-manifest'
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
if (-not $IdfPath -and (Test-Path -LiteralPath (Join-Path $root '.idf-path'))) {
    $IdfPath = (Get-Content -LiteralPath (Join-Path $root '.idf-path') -Raw).Trim()
}
if (-not $IdfPath) { throw 'Set IDF_PATH, -IdfPath or .idf-path to ESP-IDF 5.4.2 sources.' }
foreach ($source in @('components/json/cJSON/cJSON.c', 'components/json/cJSON/cJSON.h',
                     'components/esp_common/include/esp_idf_version.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $IdfPath $source))) {
        throw "Missing pinned OTA manifest source: $source."
    }
}
$versionHeader = Get-Content -LiteralPath (Join-Path $IdfPath 'components/esp_common/include/esp_idf_version.h') -Raw
if ($versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MAJOR\s+5\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_MINOR\s+4\s*$' -or
    $versionHeader -notmatch '(?m)^#define ESP_IDF_VERSION_PATCH\s+2\s*$') {
    throw 'These manifest checks require the pinned ESP-IDF 5.4.2 sources.'
}
$otaTemporaryPath = Join-Path $build 'temp'
New-Item -ItemType Directory -Force -Path $otaTemporaryPath | Out-Null
$savedOtaTemp = $env:TEMP
$savedOtaTmp = $env:TMP
try {
    $env:TEMP = $otaTemporaryPath
    $env:TMP = $otaTemporaryPath
    $executable = Join-Path $build 'manifest_test.exe'
    & $Compiler -std=c11 -Wall -Wextra -Werror -pedantic -D_CRT_SECURE_NO_WARNINGS `
        -DCONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=1 -DCONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=1 `
        -I (Join-Path $root 'tests/ota_manifest/stubs') -I (Join-Path $root 'main') `
        -I (Join-Path $IdfPath 'components/json/cJSON') `
        (Join-Path $root 'main/ota_manifest.c') (Join-Path $IdfPath 'components/json/cJSON/cJSON.c') `
        (Join-Path $root 'tests/ota_manifest/manifest_test.c') -o $executable 2>&1 |
        Tee-Object -FilePath (Join-Path $build 'compile.log')
    if ($LASTEXITCODE) { throw 'OTA manifest compilation failed.' }
    & $executable | Tee-Object -FilePath (Join-Path $build 'manifest.log')
    if ($LASTEXITCODE) { throw 'OTA manifest checks failed.' }
} finally {
    $env:TEMP = $savedOtaTemp
    $env:TMP = $savedOtaTmp
}
