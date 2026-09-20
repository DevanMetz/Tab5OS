$ErrorActionPreference = 'Stop'

$install = 'C:\Users\metzd\AppData\Local\Programs\R12ReverseEngineering'
$ghidra = Join-Path $install 'ghidra_12.1.2_PUBLIC'
$env:JAVA_HOME = Join-Path $install 'jdk-21.0.12+8'
$projectRoot = 'C:\Users\metzd\AppData\Local\R12GhidraProjects'
$projectName = 'COLMI_R12'
$firmware = 'C:\Users\metzd\AppData\Local\Temp\colmi-r12-firmware\RT11CR_1.00.09_260424.bin'
$scripts = Join-Path $PSScriptRoot 'ghidra'

New-Item -ItemType Directory -Force -Path $projectRoot | Out-Null

if (-not (Test-Path (Join-Path $projectRoot "$projectName.gpr"))) {
    & (Join-Path $ghidra 'support\analyzeHeadless.bat') `
        $projectRoot $projectName `
        -import $firmware `
        -loader BinaryLoader `
        -loader-baseAddr 0x826400 `
        -loader-fileOffset 0x450 `
        -loader-blockName R12_CODE `
        -processor 'ARM:LE:32:Cortex' `
        -scriptPath $scripts `
        -preScript R12Setup.java
    if ($LASTEXITCODE -ne 0) {
        throw "Ghidra import failed with exit code $LASTEXITCODE"
    }
}

& (Join-Path $ghidra 'ghidraRun.bat') (Join-Path $projectRoot "$projectName.gpr")
