$ErrorActionPreference = 'Stop'

$toolchain = 'C:\Users\metzd\AppData\Local\Programs\R12ReverseEngineering\arm-gnu-toolchain-15.2.rel1\bin'
$root = Split-Path $PSScriptRoot
$source = Join-Path $root 'reverse-engineering\r12\mod'
$build = Join-Path $root 'artifacts\r12\zwift-build'
$fragments = Join-Path $build 'fragments'
$object = Join-Path $build 'zwift_hr.o'
$elf = Join-Path $build 'zwift_hr.elf'
$firmware = Join-Path $root 'artifacts\r12\RT11CR_1.00.99_zwift-hr-tap-hold.bin'
$recovery = Join-Path $root 'artifacts\r12\RT11CR_1.01.00_stock-recovery.bin'

New-Item -ItemType Directory -Force -Path $fragments | Out-Null
& (Join-Path $toolchain 'arm-none-eabi-as.exe') -mcpu=cortex-m4 -mthumb -o $object (Join-Path $source 'zwift_hr.S')
if ($LASTEXITCODE) { throw 'ARM assembly failed' }
& (Join-Path $toolchain 'arm-none-eabi-ld.exe') -T (Join-Path $source 'zwift_hr.ld') -o $elf $object
if ($LASTEXITCODE) { throw 'ARM link failed' }

$sections = 'setter', 'clear_callback', 'clear_init', 'screen_gate', 'start_delay', 'restart', 'commit', 'render', 'tap_next_call', 'hrs_size', 'hrs_notify', 'hrs_service_id', 'hrs_repeat', 'tap_next', 'adv_uuid_lo', 'adv_uuid_hi', 'hrs_table'
foreach ($section in $sections) {
    & (Join-Path $toolchain 'arm-none-eabi-objcopy.exe') --dump-section ".patch.$section=$(Join-Path $fragments "$section.bin")" $elf
    if ($LASTEXITCODE) { throw "Could not extract $section" }
}

python (Join-Path $PSScriptRoot 'r12_patch.py') `
    'C:\Users\metzd\AppData\Local\Temp\colmi-r12-firmware\RT11CR_1.00.09_260424.bin' `
    $firmware --compiled-dir $fragments --zwift
if ($LASTEXITCODE) { throw 'Firmware packaging failed' }

python (Join-Path $PSScriptRoot 'r12_patch.py') `
    'C:\Users\metzd\AppData\Local\Temp\colmi-r12-firmware\RT11CR_1.00.09_260424.bin' `
    $recovery --recovery --zwift
if ($LASTEXITCODE) { throw 'Recovery packaging failed' }

Write-Host "Built Zwift test firmware: $firmware"
Write-Host "Built exact-stock recovery: $recovery"
