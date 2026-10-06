[CmdletBinding()]
param([string]$WindhawkDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'scripts\WindhawkPaths.ps1')
$paths = Get-WindhawkPaths $WindhawkDirectory
$source = Join-Path $PSScriptRoot 'src\taskbar-file-location.wh.cpp'
$version = [regex]::Match((Get-Content -LiteralPath $source -Raw), '(?m)^// @version\s+(\S+)').Groups[1].Value
$module = Join-Path $PSScriptRoot 'AppData\TaskbarFileLocation.dll'
if (-not (Test-Path -LiteralPath $module)) {
    $module = Join-Path $PSScriptRoot 'publish\AppData\TaskbarFileLocation.dll'
}
if (-not (Test-Path -LiteralPath $module)) {
    & (Join-Path $PSScriptRoot 'Build.ps1') -WindhawkDirectory $paths.Root | Out-Null
}
$modId = 'local@taskbar-file-location'
$engineDirectory = Join-Path $paths.Data 'Engine'
$modsDirectory = Join-Path $engineDirectory 'Mods'
$libraryDirectory = Join-Path $modsDirectory '64'
$libraryName = $modId + '_' + $version + '_' + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() + '.dll'
$installedLibrary = Join-Path $libraryDirectory $libraryName
$configurationFile = Join-Path $modsDirectory ($modId + '.ini')
$language = 'tr'
if (Test-Path -LiteralPath $configurationFile) {
    $previousConfiguration = Get-Content -LiteralPath $configurationFile -Raw
    $previousLanguage = [regex]::Match($previousConfiguration, '(?m)^language=(tr|en)\s*$')
    if ($previousLanguage.Success) { $language = $previousLanguage.Groups[1].Value }
}
Copy-Item -LiteralPath $module -Destination $installedLibrary
$modSource = Get-Content -LiteralPath $source -Raw
$menuHeader = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'include\TaskbarMenuButton.hpp') -Raw
$menuHeader = $menuHeader.Replace('#pragma once', '')
$modSource = $modSource.Replace('#include "../include/TaskbarMenuButton.hpp"', $menuHeader)
Set-Content -LiteralPath (Join-Path $paths.Data ('ModsSource\' + $modId + '.wh.cpp')) -Value $modSource -Encoding utf8
$configuration = @"
[Mod]
LoggingEnabled=1
LibraryFileName=$libraryName
Disabled=0
Include=explorer.exe|ShellExperienceHost.exe
Exclude=
Architecture=x86-64
Version=$version
SettingsChangeTime=$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())

[Settings]
language=$language
"@
Set-Content -LiteralPath $configurationFile -Value $configuration -Encoding unicode
$readableFiles = @(
    $installedLibrary,
    (Join-Path $libraryDirectory 'libc++.whl'),
    (Join-Path $libraryDirectory 'libunwind.whl'),
    (Join-Path $paths.Engine '64\windhawk.dll')
)
foreach ($file in $readableFiles) {
    & icacls $file /grant '*S-1-15-2-1:(RX)' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not grant shell read access to $file." }
}
foreach ($folder in @($engineDirectory, $modsDirectory)) {
    & icacls $folder /grant '*S-1-15-2-1:(RX)' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not grant shell directory access to $folder." }
}
$engineSettings = Join-Path $engineDirectory 'settings.ini'
foreach ($file in @($engineSettings, $configurationFile, (Join-Path $paths.Engine 'engine.ini'))) {
    & icacls $file /grant '*S-1-15-2-1:(R)' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not grant shell configuration access to $file." }
}
$settings = Get-Content -LiteralPath $engineSettings -Raw
$include = [regex]::Match($settings, '(?m)^Include=(.*)$')
$targets = @($include.Groups[1].Value.Trim() -split '\|' | Where-Object { $_ })
if ($targets -notcontains 'ShellExperienceHost.exe') {
    $targets += 'ShellExperienceHost.exe'
    $settings = [regex]::Replace($settings, '(?m)^Include=.*$', 'Include=' + ($targets -join '|'))
    Set-Content -LiteralPath $engineSettings -Value $settings -Encoding unicode
}
Write-Output "Installed TaskbarFileLocation $version in Windhawk."
