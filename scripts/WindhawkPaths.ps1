function Get-WindhawkPaths {
    param([string]$Directory)
    if (-not $Directory) {
        $command = Get-Command windhawk -ErrorAction Stop
        $shim = [IO.Path]::ChangeExtension($command.Source, '.shim')
        if (Test-Path -LiteralPath $shim) {
            $match = [regex]::Match((Get-Content -LiteralPath $shim -Raw), '(?m)^path\s*=\s*"([^"]+)"')
            $Directory = Split-Path -Parent $match.Groups[1].Value
        } else {
            $Directory = Split-Path -Parent $command.Source
        }
    }
    $Directory = [IO.Path]::GetFullPath($Directory)
    $configuration = Get-Content -LiteralPath (Join-Path $Directory 'windhawk.ini') -Raw
    $values = @{}
    foreach ($match in [regex]::Matches($configuration, '(?m)^(\w+)=(.*)$')) {
        $values[$match.Groups[1].Value] = $match.Groups[2].Value.Trim()
    }
    if ($values.Portable -ne '1') { throw 'The installer currently supports portable Windhawk.' }
    [pscustomobject]@{
        Root = $Directory
        Compiler = Join-Path $Directory $values.CompilerPath
        Engine = Join-Path $Directory $values.EnginePath
        Data = Join-Path $Directory $values.AppDataPath
    }
}
