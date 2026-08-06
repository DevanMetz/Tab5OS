param(
    [Parameter(Mandatory = $true)]
    [string]$RelayUrl
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bytes = New-Object byte[] 32
$rng = New-Object Security.Cryptography.RNGCryptoServiceProvider
$rng.GetBytes($bytes)
$rng.Dispose()
$token = [BitConverter]::ToString($bytes).Replace("-", "").ToLowerInvariant()

$token | & npx wrangler secret put DEVICE_TOKEN --config (Join-Path $root "relay\wrangler.jsonc")
if ($LASTEXITCODE) { throw "Could not set the Cloudflare device token" }

$header = @"
#pragma once

#define CHAT_RELAY_URL "$RelayUrl"
#define CHAT_DEVICE_TOKEN "$token"
"@
[IO.File]::WriteAllText((Join-Path $root "main\chat_secrets.h"), $header, [Text.UTF8Encoding]::new($false))
Write-Host "Device token provisioned without printing or committing it."
