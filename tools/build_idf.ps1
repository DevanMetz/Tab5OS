param(
    [switch]$Full,
    [string]$IdfPath = $env:IDF_PATH
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$localIdfPath = Join-Path $root ".idf-path"
if (-not $IdfPath -and (Test-Path $localIdfPath)) {
    $IdfPath = (Get-Content $localIdfPath -Raw).Trim()
}
if ($IdfPath) {
    & (Join-Path $IdfPath "export.ps1")
} elseif (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "ESP-IDF is not configured. Set IDF_PATH or put its path in .idf-path."
}

$env:CCACHE_DIR = Join-Path $root ".ccache"
Push-Location $root
try {
    if ($Full -or -not (Test-Path "build\build.ninja")) {
        & idf.py build
    } else {
        & ninja -C build -j 4 app
    }
    if ($LASTEXITCODE) { throw "Build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
