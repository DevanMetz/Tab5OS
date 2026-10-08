param([string]$Compiler, [string[]]$Modes, [string]$ToolSource, [string]$Output)

# Windows-only loopback integration test; no tablet, LAN peer, or SDK activation.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/modbus-network"
if ($Output) {
    $build = if ([System.IO.Path]::IsPathRooted($Output)) { [System.IO.Path]::GetFullPath($Output) }
             else { [System.IO.Path]::GetFullPath((Join-Path $root $Output)) }
}
if (-not $ToolSource) { $ToolSource = Join-Path $root 'main/modbus_tool.c' }
$ToolSource = (Resolve-Path -LiteralPath $ToolSource).Path
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
if ($env:OS -ne "Windows_NT") { throw "This focused adapter requires Windows; use the pure Modbus parser tests on other hosts." }
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
$inputs = @('main/modbus_tool.c', 'main/modbus_tool.h', 'main/modbus_data.c', 'main/modbus_data.h',
            'main/ipv4_data.c', 'main/ipv4_data.h', 'main/byte_data.c', 'main/byte_data.h',
            'tests/modbus_network/network_test.c', 'tests/modbus_network/host.c', 'tests/modbus_network/host.h',
            'tests/modbus_network/run.py', 'tools/modbus_test_server.py', 'tools/test_modbus_network.ps1')
$inputs += Get-ChildItem -LiteralPath "$root/tests/modbus_network/stubs" -Recurse -File |
    ForEach-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') }
$hashes = [ordered]@{}
foreach ($inputName in $inputs) {
    $inputPath = if ($inputName -eq 'main/modbus_tool.c') { $ToolSource } else { Join-Path $root $inputName }
    $snapshot = Join-Path $sourceRoot $inputName
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $snapshot) | Out-Null
    $sourceText = [System.IO.File]::ReadAllText($inputPath).Replace("`r`n", "`n")
    [System.IO.File]::WriteAllText($snapshot, $sourceText, [System.Text.UTF8Encoding]::new($false))
    $hashes[$inputName] = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
}
$metadata = [ordered]@{ compiler = $Compiler; toolSource = $ToolSource; normalizedSourceSha256 = $hashes;
    lvglLibrarySha256 = (Get-FileHash -LiteralPath $lvgl -Algorithm SHA256).Hash.ToLowerInvariant();
    lvglPoolBytes = 98304; actualAppSourceCompiled = $false; actualLvglLinked = $false;
    nativeLoopbackSocketsAndThreads = $true;
    workerStartAndMonotonicClockControlled = $true; actualSdkSchedulerOrPhysicalWifiVerified = $false;
    executableSha256 = $null; exitCode = 1 }
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5ModbusNetwork C)
set(CMAKE_C_STANDARD 11)
set(TESTS "${SOURCE_ROOT}/tests/modbus_network")
add_executable(network_test "${TESTS}/network_test.c" "${TESTS}/host.c" "${SOURCE_ROOT}/main/modbus_tool.c" "${SOURCE_ROOT}/main/modbus_data.c" "${SOURCE_ROOT}/main/ipv4_data.c" "${SOURCE_ROOT}/main/byte_data.c")
target_compile_options(network_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(network_test PRIVATE "${TAB5_ROOT}/managed_components/lvgl__lvgl" "${SOURCE_ROOT}/main" "${TESTS}/stubs" "${TESTS}")
target_compile_definitions(network_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(network_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib" ws2_32)
'@ | Set-Content -LiteralPath (Join-Path $build "CMakeLists.txt")
$suiteLog = Join-Path $build 'compile.log'
try {
    & $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DSOURCE_ROOT=$($sourceRoot.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug 2>&1 | Tee-Object -FilePath $suiteLog
    if ($LASTEXITCODE) { throw "Modbus host configuration failed." }
    & $cmake --build "$build/out" -j 4 2>&1 | Tee-Object -FilePath $suiteLog -Append
    if ($LASTEXITCODE) { throw "Modbus host build failed." }
    $metadata.actualAppSourceCompiled = $true
    $metadata.actualLvglLinked = $true
    $metadata.executableSha256 = (Get-FileHash -LiteralPath "$build/out/network_test.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
    $suiteLog = Join-Path $build 'result.log'
    & $python (Join-Path $sourceRoot "tests/modbus_network/run.py") --executable "$build/out/network_test.exe" --output $build @Modes 2>&1 | Tee-Object -FilePath $suiteLog
    if ($LASTEXITCODE) { throw "Modbus loopback checks failed." }
    $metadata.exitCode = 0
} catch {
    $_ | Out-String | Add-Content -LiteralPath $suiteLog
    throw
} finally {
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build 'source.json') -Encoding UTF8
}
