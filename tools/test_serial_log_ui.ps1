param([string]$Compiler, [switch]$Snapshots)

# Real file reader/viewer and LVGL, with only heap and CRT portability adapters.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/serial-log-ui'
$library = Join-Path $root 'build/offline-ui/out/lvgl_host.lib'
if ($env:OS -ne 'Windows_NT') { throw 'This cached-library UI runner requires Windows.' }
if (-not (Test-Path -LiteralPath $library)) {
    throw 'Cached LVGL is missing. Run test_offline_ui.ps1 when a library build is appropriate.'
}
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
New-Item -ItemType Directory -Force -Path $build | Out-Null
$adapter = Join-Path $root 'tests/serial_log_host'
$sources = @('tests/serial_log_ui_test.c', 'main/serial_log_viewer.c', 'main/serial_log_data.c',
    'main/modbus_rtu_tool.c', 'main/modbus_data.c', 'main/byte_tool.c', 'main/byte_data.c', 'main/payload_clipboard.c') |
    ForEach-Object { Join-Path $root $_ }
$executable = Join-Path $build 'serial_log_ui_test.exe'
& $Compiler -std=c11 -Wall -Wextra -Werror -fms-runtime-lib=dll_dbg -I $adapter `
    -include (Join-Path $adapter 'compat.h') -I (Join-Path $root 'main') -I (Join-Path $root 'managed_components/lvgl__lvgl') `
    -DLV_CONF_SKIP -DLV_KCONFIG_IGNORE -DLV_USE_OS=0 -DLV_MEM_SIZE=98304 -DLV_FONT_MONTSERRAT_28=1 `
    -DLV_FONT_MONTSERRAT_48=1 -DLV_USE_FLOAT=0 -D_CRT_SECURE_NO_WARNINGS $sources $library `
    -Xlinker /nodefaultlib:libcmt -o $executable
if ($LASTEXITCODE) { throw 'Serial log UI compilation failed.' }
$testArgs = @($build.Replace('\', '/'))
if ($Snapshots) { $testArgs += '--snapshots' }
& $executable @testArgs
if ($LASTEXITCODE) { throw 'Serial log UI checks failed.' }
if ($Snapshots) { Write-Host "PPM screenshots: $build" }
