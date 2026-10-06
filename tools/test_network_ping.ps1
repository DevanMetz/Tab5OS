param([string]$Compiler)

# Compile the actual ping worker with deterministic socket/clock adapters.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/network-ping'
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$pingTemporaryPath = Join-Path $build 'temp'
New-Item -ItemType Directory -Force -Path $pingTemporaryPath | Out-Null
$savedPingTemp = $env:TEMP
$savedPingTmp = $env:TMP
try {
    # Keep compiler intermediates in the workspace, including under a managed sandbox.
    $env:TEMP = $pingTemporaryPath
    $env:TMP = $pingTemporaryPath
    foreach ($ipv6 in @(0, 1)) {
        $executable = Join-Path $build "ping-ipv6-$ipv6.exe"
        & $Compiler -std=c11 -Wall -Wextra -Werror -pedantic "-DLWIP_IPV6=$ipv6" `
            -I (Join-Path $root 'tests/network_ping_host/stubs') -I (Join-Path $root 'main') `
            (Join-Path $root 'main/network_ping.c') (Join-Path $root 'tests/network_ping_host/ping_test.c') `
            -o $executable
        if ($LASTEXITCODE) { throw "Network ping compilation failed (IPv6=$ipv6)." }
        & $executable | Tee-Object -FilePath (Join-Path $build "ipv6-$ipv6.log")
        if ($LASTEXITCODE) { throw "Network ping checks failed (IPv6=$ipv6)." }
    }
} finally {
    $env:TEMP = $savedPingTemp
    $env:TMP = $savedPingTmp
}
