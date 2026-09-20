$ErrorActionPreference = 'Stop'

$toolchain = 'C:\Users\metzd\AppData\Local\Programs\R12ReverseEngineering\arm-gnu-toolchain-15.2.rel1\bin'
$stock = 'C:\Users\metzd\AppData\Local\Temp\colmi-r12-firmware\RT11CR_1.00.09_260424.bin'
$root = Split-Path $PSScriptRoot
$source = Join-Path $root 'reverse-engineering\r12\mod'
$build = Join-Path $root 'artifacts\r12\endurance-build'
$fragments = Join-Path $build 'fragments'
$object = Join-Path $build 'hr_endurance.o'
$elf = Join-Path $build 'hr_endurance.elf'
$firmware = Join-Path $root 'artifacts\r12\RT11CR_1.01.05_hr-endurance.bin'
$recovery = Join-Path $root 'artifacts\r12\RT11CR_1.01.06_stock-recovery.bin'

New-Item -ItemType Directory -Force -Path $fragments | Out-Null

& (Join-Path $toolchain 'arm-none-eabi-as.exe') `
    -mcpu=cortex-m4 -mthumb `
    -o $object `
    (Join-Path $source 'hr_endurance.S')
if ($LASTEXITCODE -ne 0) { throw 'ARM assembly failed' }

& (Join-Path $toolchain 'arm-none-eabi-ld.exe') `
    -T (Join-Path $source 'hr_endurance.ld') `
    -o $elf $object
if ($LASTEXITCODE -ne 0) { throw 'ARM link failed' }

$sections = @(
    'setter_trampoline', 'fake_retained_call', 'clear_callback',
    'disable_fake_start', 'clear_init', 'periodic_callback', 'validator',
    'spot_callback', 'screen_fallback', 'commit', 'render'
)
foreach ($section in $sections) {
    & (Join-Path $toolchain 'arm-none-eabi-objcopy.exe') `
        --dump-section ".patch.$section=$(Join-Path $fragments "$section.bin")" `
        $elf
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $section" }
}

python (Join-Path $PSScriptRoot 'r12_patch.py') `
    $stock $recovery --recovery --endurance
if ($LASTEXITCODE -ne 0) { throw 'Recovery firmware packaging failed' }

python (Join-Path $PSScriptRoot 'r12_patch.py') `
    $stock $firmware --compiled-dir $fragments --endurance
if ($LASTEXITCODE -ne 0) { throw 'Firmware packaging failed' }

$stockBytes = [IO.File]::ReadAllBytes($stock)
$firmwareBytes = [IO.File]::ReadAllBytes($firmware)
$allowed = @(
    @(0x0c, 4), @(0x52, 2), @(0x56, 2), @(0xb0, 4),
    @(0xe178, 4), @(0xe23e, 4), @(0xe244, 8), @(0xe24c, 2),
    @(0xe278, 2), @(0xe468, 72), @(0xe4b0, 80), @(0xe770, 56),
    @(0x151f6, 20), @(0x1520a, 8), @(0x1523c, 12)
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

Write-Host "Verified HR-endurance candidate: $((Get-FileHash $firmware -Algorithm SHA256).Hash.ToLower())"
Write-Host "Verified higher-version stock recovery: $((Get-FileHash $recovery -Algorithm SHA256).Hash.ToLower())"
