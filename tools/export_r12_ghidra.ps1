$ErrorActionPreference = 'Stop'

$install = 'C:\Users\metzd\AppData\Local\Programs\R12ReverseEngineering'
$ghidra = Join-Path $install 'ghidra_12.1.2_PUBLIC'
$env:JAVA_HOME = Join-Path $install 'jdk-21.0.12+8'
$projectRoot = 'C:\Users\metzd\AppData\Local\R12GhidraProjects'
$output = Join-Path (Split-Path $PSScriptRoot) 'reverse-engineering\r12\decompiled'

& (Join-Path $ghidra 'support\analyzeHeadless.bat') `
    $projectRoot COLMI_R12 `
    -process RT11CR_1.00.09_260424.bin `
    -noanalysis `
    -scriptPath (Join-Path $PSScriptRoot 'ghidra') `
    -postScript R12AnnotateAndExport.java $output

if ($LASTEXITCODE -ne 0) {
    throw "Ghidra export failed with exit code $LASTEXITCODE"
}
