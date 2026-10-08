param([string]$Compiler, [string[]]$Modes, [string]$SourceDirectory, [string]$Output)

# Actual UI/workers, native Windows threads, real UDP exchanges with loopback peers.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/device-network"
if ($Output) {
    $build = if ([System.IO.Path]::IsPathRooted($Output)) { [System.IO.Path]::GetFullPath($Output) }
             else { [System.IO.Path]::GetFullPath((Join-Path $root $Output)) }
}
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $root 'main' }
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$cache = Join-Path $root "build/CMakeCache.txt"
function Find-Tool([string]$Name, [string]$CacheKey) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    if (Test-Path -LiteralPath $cache) {
        $line = Get-Content -LiteralPath $cache | Where-Object { $_ -match "^${CacheKey}:[^=]+=" } | Select-Object -First 1
        if ($line) {
            $path = ($line -split '=', 2)[1]
            if (Test-Path -LiteralPath $path) { return $path }
        }
    }
    throw "$Name not found. Add it to PATH or build firmware once to populate build/CMakeCache.txt."
}
if ($env:OS -ne "Windows_NT") { throw "This focused adapter requires Windows; pure NTP/WoL tests run on other hosts." }
$cmake = Find-Tool "cmake" "CMAKE_COMMAND"
$ninja = Find-Tool "ninja" "CMAKE_MAKE_PROGRAM"
$python = (Get-Command python -ErrorAction Stop).Source
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
$lvgl = Join-Path $root "build/offline-ui/out/lvgl_host.lib"
if (-not (Test-Path -LiteralPath $lvgl)) {
    & (Join-Path $PSScriptRoot "test_offline_ui.ps1") -Compiler $Compiler
}
New-Item -ItemType Directory -Force -Path $build | Out-Null
$sourceRoot = Join-Path $build 'sources'
$apps = @('main/ntp_tool.c', 'main/wol_tool.c', 'main/udp_tool.c')
$inputs = @('main/ntp_tool.c', 'main/ntp_tool.h', 'main/ntp_data.c', 'main/ntp_data.h',
            'main/wol_tool.c', 'main/wol_tool.h', 'main/wol_data.c', 'main/wol_data.h',
            'main/udp_tool.c', 'main/udp_tool.h', 'main/ipv4_data.c', 'main/ipv4_data.h',
            'main/byte_data.c', 'main/byte_data.h', 'main/byte_tool.c', 'main/byte_tool.h',
            'main/payload_clipboard.c', 'main/payload_clipboard.h', 'tests/device_network/device_test.c',
            'tests/device_network/run.py', 'tools/udp_device_fixture.py', 'tools/test_device_network.ps1',
            'tests/modbus_network/host.c', 'tests/modbus_network/host.h')
foreach ($stubs in @('tests/device_network/stubs', 'tests/modbus_network/stubs')) {
    $inputs += Get-ChildItem -LiteralPath (Join-Path $root $stubs) -Recurse -File |
        ForEach-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') }
}
$hashes = [ordered]@{}
foreach ($inputName in $inputs) {
    $inputPath = if ($apps -contains $inputName) { Join-Path $SourceDirectory (Split-Path -Leaf $inputName) }
                 else { Join-Path $root $inputName }
    $snapshot = Join-Path $sourceRoot $inputName
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $snapshot) | Out-Null
    [System.IO.File]::WriteAllText($snapshot, [System.IO.File]::ReadAllText($inputPath).Replace("`r`n", "`n"), [System.Text.UTF8Encoding]::new($false))
    $hashes[$inputName] = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
}
$metadata = [ordered]@{ compiler = $Compiler; appSourceDirectory = $SourceDirectory; normalizedSourceSha256 = $hashes;
    lvglLibrarySha256 = (Get-FileHash -LiteralPath $lvgl -Algorithm SHA256).Hash.ToLowerInvariant();
    lvglPoolBytes = 98304; actualAppSourceCompiled = $false; actualLvglLinked = $false;
    nativeLoopbackSocketsAndThreads = $true; workerStartAndMonotonicClockControlled = $true;
    actualSdkSchedulerOrPhysicalWifiVerified = $false; executableSha256 = $null; exitCode = 1 }
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5DeviceNetwork C)
set(CMAKE_C_STANDARD 11)
set(HOST "${SOURCE_ROOT}/tests/modbus_network")
set(TESTS "${SOURCE_ROOT}/tests/device_network")
add_executable(device_test "${TESTS}/device_test.c" "${HOST}/host.c" "${SOURCE_ROOT}/main/ntp_tool.c" "${SOURCE_ROOT}/main/ntp_data.c" "${SOURCE_ROOT}/main/wol_tool.c" "${SOURCE_ROOT}/main/wol_data.c" "${SOURCE_ROOT}/main/udp_tool.c" "${SOURCE_ROOT}/main/byte_data.c" "${SOURCE_ROOT}/main/ipv4_data.c")
target_include_directories(device_test PRIVATE "${TAB5_ROOT}/managed_components/lvgl__lvgl" "${SOURCE_ROOT}/main" "${TESTS}/stubs" "${HOST}/stubs" "${HOST}")
target_compile_definitions(device_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
target_compile_options(device_test PRIVATE -Wall -Wextra -Werror)
target_sources(device_test PRIVATE "${SOURCE_ROOT}/main/payload_clipboard.c" "${SOURCE_ROOT}/main/byte_tool.c")
target_link_libraries(device_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build "CMakeLists.txt")
$suiteLog = Join-Path $build 'compile.log'
try {
    & $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DSOURCE_ROOT=$($sourceRoot.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug 2>&1 | Tee-Object -FilePath $suiteLog
    if ($LASTEXITCODE) { throw "Device host configuration failed." }
    & $cmake --build "$build/out" -j 4 2>&1 | Tee-Object -FilePath $suiteLog -Append
    if ($LASTEXITCODE) { throw "Device host build failed." }
    $metadata.actualAppSourceCompiled = $true
    $metadata.actualLvglLinked = $true
    $metadata.executableSha256 = (Get-FileHash -LiteralPath "$build/out/device_test.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
    $suiteLog = Join-Path $build 'result.log'
    & $python (Join-Path $sourceRoot "tests/device_network/run.py") --executable "$build/out/device_test.exe" --output $build @Modes 2>&1 | Tee-Object -FilePath $suiteLog
    if ($LASTEXITCODE) { throw "UDP loopback checks failed." }
    $metadata.exitCode = 0
} catch {
    $_ | Out-String | Add-Content -LiteralPath $suiteLog
    throw
} finally {
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build 'source.json') -Encoding UTF8
}
