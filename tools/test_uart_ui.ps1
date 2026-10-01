param([string]$Compiler, [switch]$Snapshots, [switch]$Quick)

# Real serial app/LVGL with bounded UART service adapters; no physical pins/SD.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/uart-ui"
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
if ($env:OS -ne "Windows_NT") { throw "This UI adapter requires Windows; pure uart_data tests run on other hosts." }
$cmake = Find-Tool "cmake" "CMAKE_COMMAND"
$ninja = Find-Tool "ninja" "CMAKE_MAKE_PROGRAM"
if (-not $Compiler) { $Compiler = (Get-Command clang -ErrorAction Stop).Source }
if (-not (Test-Path -LiteralPath (Join-Path $root "build/offline-ui/out/lvgl_host.lib"))) {
    throw "Cached LVGL is missing. Run test_offline_ui.ps1 when a library build is appropriate."
}
New-Item -ItemType Directory -Force -Path $build | Out-Null
@'
cmake_minimum_required(VERSION 3.20)
project(Tab5SerialUI C)
set(CMAKE_C_STANDARD 11)
set(HOST "${TAB5_ROOT}/tests/uart_host")
add_executable(uart_ui_test "${HOST}/ui_test.c" "${HOST}/host.c" "${TAB5_ROOT}/main/uart_tool.c" "${TAB5_ROOT}/main/uart_data.c" "${TAB5_ROOT}/main/payload_clipboard.c")
target_sources(uart_ui_test PRIVATE "${TAB5_ROOT}/main/modbus_rtu_tool.c" "${TAB5_ROOT}/main/modbus_data.c" "${TAB5_ROOT}/main/byte_tool.c" "${TAB5_ROOT}/main/byte_data.c")
target_include_directories(uart_ui_test PRIVATE "${HOST}" "${TAB5_ROOT}/main" "${TAB5_ROOT}/managed_components/lvgl__lvgl")
target_compile_options(uart_ui_test PRIVATE -Wall -Wextra -Werror -include "${HOST}/compat.h")
target_compile_definitions(uart_ui_test PRIVATE LV_CONF_SKIP LV_KCONFIG_IGNORE LV_USE_OS=0 LV_MEM_SIZE=98304 LV_FONT_MONTSERRAT_28=1 LV_FONT_MONTSERRAT_48=1 LV_USE_FLOAT=0 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(uart_ui_test PRIVATE "${TAB5_ROOT}/build/offline-ui/out/lvgl_host.lib")
'@ | Set-Content -LiteralPath (Join-Path $build "CMakeLists.txt")
& $cmake -S $build -B "$build/out" -G Ninja "-DTAB5_ROOT=$($root.Replace('\', '/'))" "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE) { throw "Serial UI configuration failed." }
& $cmake --build "$build/out" -j 1
if ($LASTEXITCODE) { throw "Serial UI build failed." }
$testArgs = @()
if ($Snapshots) { $testArgs += $build }
if ($Quick) { $testArgs += '--quick' }
& "$build/out/uart_ui_test.exe" @testArgs
if ($LASTEXITCODE) { throw "Serial UI checks failed." }
if ($Snapshots) { Write-Host "PPM screenshots: $build" }
