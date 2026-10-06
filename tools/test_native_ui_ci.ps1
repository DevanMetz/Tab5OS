param([string]$Compiler)

# Uses the source artifact from the Linux firmware job; no SDK activation.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($env:OS -ne 'Windows_NT') { throw 'Native UI CI requires Windows.' }
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$nativeIdf = Join-Path $root 'build/native-ui-idf'
$nativeTemp = Join-Path $root 'build/native-ui-temp'
foreach ($directory in @($nativeTemp, "$root/build/offline-ui", "$root/build/mqtt-ui", "$root/build/mqtt-network")) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$savedNativeTemp = $env:TEMP
$savedNativeTmp = $env:TMP
try {
    $env:TEMP = $nativeTemp
    $env:TMP = $nativeTemp
    & $Compiler --version
    if ($LASTEXITCODE) { throw 'Native compiler is unavailable.' }
    & "$PSScriptRoot/test_offline_ui.ps1" -Compiler $Compiler 2>&1 |
        Tee-Object -FilePath "$root/build/offline-ui/ci.log"
    & "$PSScriptRoot/test_mqtt_ui.ps1" -Compiler $Compiler -IdfPath $nativeIdf 2>&1 |
        Tee-Object -FilePath "$root/build/mqtt-ui/ci.log"
    & "$PSScriptRoot/test_mqtt_network.ps1" -Compiler $Compiler -IdfPath $nativeIdf 2>&1 |
        Tee-Object -FilePath "$root/build/mqtt-network/ci.log"
} finally {
    $env:TEMP = $savedNativeTemp
    $env:TMP = $savedNativeTmp
}
