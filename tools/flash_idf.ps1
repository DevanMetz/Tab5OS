param(
    [string]$Port = "COM7",
    [switch]$Full,
    [string]$IdfPath = $env:IDF_PATH
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "build_idf.ps1") -Full:$Full -IdfPath $IdfPath
Push-Location $root
try {
    if ($Full) {
        & idf.py -p $Port flash
    } else {
        & python -m esptool --chip esp32p4 -p $Port -b 460800 --before default_reset --after hard_reset `
            write_flash 0x20000 build\tab5_os.bin
    }
    if ($LASTEXITCODE) { throw "Flash failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
