[CmdletBinding()]
param([string]$WindhawkDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\WindhawkPaths.ps1')
$paths = Get-WindhawkPaths $WindhawkDirectory
$source = Join-Path $PSScriptRoot 'src\taskbar-file-location.wh.cpp'
$sourceText = Get-Content -LiteralPath $source -Raw
$version = [regex]::Match($sourceText, '(?m)^// @version\s+(\S+)').Groups[1].Value
$outputDirectory = Join-Path $PSScriptRoot 'publish\AppData'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$output = Join-Path $outputDirectory 'TaskbarFileLocation.dll'
$temporaryDirectory = (& NewTemp TaskbarFileLocationBuild | Select-Object -Last 1).Trim()
if (-not (Test-Path -LiteralPath $temporaryDirectory -PathType Container)) {
    throw 'NewTemp did not return a directory.'
}
$previousTemporaryDirectory = $env:TEMP
$previousTemporaryAlias = $env:TMP
$arguments = @(
    '-std=c++23', '-O2', '-shared', '-DUNICODE', '-D_UNICODE',
    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00',
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
    '-DWH_MOD_ID=L"local@taskbar-file-location"',
    ('-DWH_MOD_VERSION=L"' + $version + '"'),
    (Join-Path $paths.Engine '64\windhawk.lib'), $source,
    '-include', 'windhawk_api.h', '-target', 'x86_64-w64-mingw32',
    '-Wl,--export-all-symbols', '-o', $output,
    '-lcomctl32', '-lshell32', '-lshlwapi', '-luuid',
    '-lole32', '-loleaut32', '-lruntimeobject'
)
try {
    $env:TEMP = $temporaryDirectory
    $env:TMP = $temporaryDirectory
    Push-Location $paths.Compiler
    try {
        & (Join-Path $paths.Compiler 'bin\clang++.exe') @arguments
        if ($LASTEXITCODE -ne 0) { throw "C++ compilation failed with exit code $LASTEXITCODE." }
    } finally { Pop-Location }
} finally {
    $env:TEMP = $previousTemporaryDirectory
    $env:TMP = $previousTemporaryAlias
}
Get-Item -LiteralPath $output
