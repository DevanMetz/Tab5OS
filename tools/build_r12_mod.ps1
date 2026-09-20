$ErrorActionPreference = 'Stop'

$toolchain = 'C:\Users\metzd\AppData\Local\Programs\R12ReverseEngineering\arm-gnu-toolchain-15.2.rel1\bin'
$stock = 'C:\Users\metzd\AppData\Local\Temp\colmi-r12-firmware\RT11CR_1.00.09_260424.bin'
$source = Join-Path (Split-Path $PSScriptRoot) 'reverse-engineering\r12\mod'
$build = Join-Path (Split-Path $PSScriptRoot) 'artifacts\r12\mod-build'
$fragments = Join-Path $build 'fragments'
$object = Join-Path $build 'persistent_hr.o'
$elf = Join-Path $build 'persistent_hr.elf'
$firmware = Join-Path (Split-Path $PSScriptRoot) 'artifacts\r12\RT11CR_1.01.03_immediate-retained-hr.bin'
$recovery = Join-Path (Split-Path $PSScriptRoot) 'artifacts\r12\RT11CR_1.01.04_stock-recovery.bin'

New-Item -ItemType Directory -Force -Path $fragments | Out-Null

& (Join-Path $toolchain 'arm-none-eabi-as.exe') `
    -mcpu=cortex-m4 -mthumb `
    -o $object `
    (Join-Path $source 'persistent_hr.S')
if ($LASTEXITCODE -ne 0) { throw 'ARM assembly failed' }

& (Join-Path $toolchain 'arm-none-eabi-ld.exe') `
    -T (Join-Path $source 'persistent_hr.ld') `
    -o $elf $object
if ($LASTEXITCODE -ne 0) { throw 'ARM link failed' }

$sections = 'setter', 'clear_callback', 'clear_init', 'commit', 'render'
foreach ($section in $sections) {
    & (Join-Path $toolchain 'arm-none-eabi-objcopy.exe') `
        --dump-section ".patch.$section=$(Join-Path $fragments "$section.bin")" `
        $elf
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $section" }
}

python (Join-Path $PSScriptRoot 'r12_patch.py') `
    $stock $recovery --recovery
if ($LASTEXITCODE -ne 0) { throw 'Recovery firmware packaging failed' }

python (Join-Path $PSScriptRoot 'r12_patch.py') `
    $stock $firmware `
    --compiled-dir $fragments
if ($LASTEXITCODE -ne 0) { throw 'Firmware packaging failed' }

$stockBytes = [IO.File]::ReadAllBytes($stock)
$firmwareBytes = [IO.File]::ReadAllBytes($firmware)
$allowed = @(
    @(0x0c, 4), @(0x52, 2), @(0x56, 2), @(0xb0, 4),
    @(0xe178, 14), @(0xe244, 8), @(0xe278, 2),
    @(0x1520a, 8), @(0x1523c, 12)
)
$stockVersion = [Text.Encoding]::ASCII.GetBytes('1.00.09')
for ($i = 0; $i -le $stockBytes.Length - $stockVersion.Length; $i++) {
    $matches = $true
    for ($j = 0; $j -lt $stockVersion.Length; $j++) {
        if ($stockBytes[$i + $j] -ne $stockVersion[$j]) { $matches = $false; break }
    }
    if ($matches) { $allowed += ,@($i, $stockVersion.Length) }
}
for ($i = 0; $i -lt $stockBytes.Length; $i++) {
    if ($stockBytes[$i] -eq $firmwareBytes[$i]) { continue }
    $isAllowed = $false
    foreach ($range in $allowed) {
        if ($i -ge $range[0] -and $i -lt ($range[0] + $range[1])) {
            $isAllowed = $true
            break
        }
    }
    if (-not $isAllowed) { throw ('Unexpected firmware change at 0x{0:x}' -f $i) }
}

$actual = (Get-FileHash $firmware -Algorithm SHA256).Hash.ToLower()
$recoveryHash = (Get-FileHash $recovery -Algorithm SHA256).Hash.ToLower()
Write-Host "Verified minimal retained-BPM firmware: $actual"
Write-Host "Verified higher-version stock recovery: $recoveryHash"
