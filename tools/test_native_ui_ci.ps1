param([string]$Compiler)

# Uses the source artifact from the Linux firmware job; no SDK activation.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($env:OS -ne 'Windows_NT') { throw 'Native UI CI requires Windows.' }
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$nativeIdf = Join-Path $root 'build/native-ui-idf'
$nativeTemp = Join-Path $root 'build/native-ui-temp'
foreach ($directory in @($nativeTemp, "$root/build/offline-ui", "$root/build/mqtt-ui", "$root/build/mqtt-network",
                         "$root/build/http-network", "$root/build/network-ui", "$root/build/ota-manifest")) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$savedNativeTemp = $env:TEMP
$savedNativeTmp = $env:TMP
$suiteLog = "$root/build/native-ui.log"
try {
    $env:TEMP = $nativeTemp
    $env:TMP = $nativeTemp
    & $Compiler --version
    if ($LASTEXITCODE) { throw 'Native compiler is unavailable.' }
    $suiteLog = "$root/build/offline-ui/ci.log"
    & "$PSScriptRoot/test_offline_ui.ps1" -Compiler $Compiler 2>&1 |
        Tee-Object -FilePath $suiteLog
    $suiteLog = "$root/build/mqtt-ui/ci.log"
    & "$PSScriptRoot/test_mqtt_ui.ps1" -Compiler $Compiler -IdfPath $nativeIdf 2>&1 |
        Tee-Object -FilePath $suiteLog
    $suiteLog = "$root/build/mqtt-network/ci.log"
    & "$PSScriptRoot/test_mqtt_network.ps1" -Compiler $Compiler -IdfPath $nativeIdf 2>&1 |
        Tee-Object -FilePath $suiteLog
    $suiteLog = "$root/build/http-network/ci.log"
    & "$PSScriptRoot/test_http_network.ps1" -Compiler $Compiler -IdfPath $nativeIdf 2>&1 |
        Tee-Object -FilePath $suiteLog
    $suiteLog = "$root/build/network-ui/ci.log"
    & "$PSScriptRoot/test_network_ui.ps1" -Compiler $Compiler 2>&1 |
        Tee-Object -FilePath $suiteLog
    $suiteLog = "$root/build/ota-manifest/ci.log"
    & "$PSScriptRoot/test_ota_manifest.ps1" -Compiler $Compiler -IdfPath $nativeIdf 2>&1 |
        Tee-Object -FilePath $suiteLog
} catch {
    # A terminating error can precede Tee-Object's first output/file creation.
    $_ | Out-String | Add-Content -LiteralPath $suiteLog
    throw
} finally {
    $env:TEMP = $savedNativeTemp
    $env:TMP = $savedNativeTmp
}
