param([string]$Compiler, [switch]$Snapshots)

# Small, serial native build using the already compiled Debug LVGL library.
# Deliberately does not rebuild LVGL, start workers, or open hardware/network.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/modbus-rtu-ui'
$library = Join-Path $root 'build/offline-ui/out/lvgl_host.lib'
if ($env:OS -ne 'Windows_NT') { throw 'This cached-library UI runner requires Windows.' }
if (-not (Test-Path -LiteralPath $library)) {
    throw 'Cached LVGL is missing. Run test_offline_ui.ps1 when a library build is appropriate.'
}
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
New-Item -ItemType Directory -Force -Path $build | Out-Null
$sources = @('tests/modbus_rtu_ui_test.c', 'main/modbus_rtu_tool.c', 'main/modbus_data.c', 'main/byte_data.c', 'main/payload_clipboard.c') |
    ForEach-Object { Join-Path $root $_ }
$executable = Join-Path $build 'modbus_rtu_ui_test.exe'
& $Compiler -std=c11 -Wall -Wextra -Werror -fms-runtime-lib=dll_dbg -I (Join-Path $root 'main') -I (Join-Path $root 'managed_components/lvgl__lvgl') `
    -DLV_CONF_SKIP -DLV_KCONFIG_IGNORE -DLV_USE_OS=0 -DLV_MEM_SIZE=98304 -DLV_FONT_MONTSERRAT_28=1 `
    -DLV_FONT_MONTSERRAT_48=1 -DLV_USE_FLOAT=0 -D_CRT_SECURE_NO_WARNINGS $sources $library `
    -Xlinker /nodefaultlib:libcmt -o $executable
if ($LASTEXITCODE) { throw 'RTU UI compilation failed.' }
if ($Snapshots) { & $executable $build } else { & $executable }
if ($LASTEXITCODE) { throw 'RTU UI checks failed.' }
if ($Snapshots) { Write-Host "PPM screenshots: $build" }
