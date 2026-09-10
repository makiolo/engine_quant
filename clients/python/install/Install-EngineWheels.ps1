#Requires -Version 5.1
<#
.SYNOPSIS
    Instala `engine-quant` en todos los interpretes de Python de 64 bits detectados que
    tengan una rueda compatible en esta carpeta.
.DESCRIPTION
    Descubre interpretes de Python instalados via el registro (PEP 514,
    HKLM/HKCU\SOFTWARE\Python\PythonCore, incluida la vista de 32 bits en un Windows de 64) y,
    si esta presente, via el lanzador `py` (`py -0p`). No asume una lista fija de versiones:
    para cada interprete encontrado se pregunta directamente su version (`sys.version_info`)
    y se busca una rueda "*-cp<major><minor>-cp<major><minor>-*.whl" en esta carpeta -- una
    version de Python futura que aun no exista al escribir este script funciona igual, en
    cuanto la release incluya su rueda correspondiente (ver .github/workflows/release.yml).

    Pensado para ejecutarse desde la carpeta de la release con los ficheros .whl (uno por
    version de CPython) en el mismo directorio que este script.
.PARAMETER WheelsDir
    Carpeta con los ficheros .whl. Por defecto, la carpeta de este script.
.PARAMETER ManifestPath
    Fichero donde se anota en que interpretes se instalo, para que
    Uninstall-EngineWheels.ps1 sepa de donde quitarlo despues. Por defecto,
    installed_pythons.txt junto a este script.
.EXAMPLE
    .\Install-EngineWheels.ps1
#>
[CmdletBinding()]
param(
    [string]$WheelsDir,
    [string]$ManifestPath
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($WheelsDir)) {
    $WheelsDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
}
if ([string]::IsNullOrEmpty($ManifestPath)) {
    $ManifestPath = Join-Path $WheelsDir "installed_pythons.txt"
}

function Get-CandidateInterpreters {
    $paths = New-Object System.Collections.Generic.List[string]

    # PEP 514: cada interprete registrado (instaladores de python.org, Windows Store,
    # Anaconda/Miniconda de base -- comprobado que Miniconda tambien se registra aqui) deja
    # HKLM|HKCU\SOFTWARE\Python\PythonCore\<version>\InstallPath. En un Windows de 64 bits, un
    # Python de 32 bits registrado por un proceso de 32 bits cae en la vista WOW6432Node.
    $regRoots = @(
        "HKLM:\SOFTWARE\Python\PythonCore",
        "HKLM:\SOFTWARE\WOW6432Node\Python\PythonCore",
        "HKCU:\SOFTWARE\Python\PythonCore"
    )
    foreach ($root in $regRoots) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
            $installPathKey = Join-Path $_.PSPath "InstallPath"
            if (-not (Test-Path $installPathKey)) { return }
            $item = Get-Item -Path $installPathKey
            $exe = $item.GetValue("ExecutablePath")
            if (-not $exe) {
                $installDir = $item.GetValue("")
                if ($installDir) { $exe = Join-Path $installDir "python.exe" }
            }
            if ($exe) { $paths.Add($exe) }
        }
    }

    # Complementa (no sustituye) lo anterior: el lanzador `py` conoce instalaciones que a
    # veces no pasan por PEP 514 tal cual (p.ej. algunas instalaciones "solo para mi").
    $py = Get-Command "py.exe" -ErrorAction SilentlyContinue
    if ($py) {
        try {
            $lines = & $py.Source -0p 2>$null
            foreach ($line in $lines) {
                # Formato tipico: " -V:3.12 *        C:\...\python.exe"
                $match = [regex]::Match($line, '([A-Za-z]:\\.*?python\.exe)\s*$')
                if ($match.Success) { $paths.Add($match.Groups[1].Value.Trim()) }
            }
        } catch {
            Write-Verbose "py -0p fallo (no fatal): $($_.Exception.Message)"
        }
    }

    $paths | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
}

function Get-InterpreterInfo {
    param([string]$PythonExe)
    try {
        $lines = & $PythonExe -c "import sys, struct; print(sys.version_info[0]); print(sys.version_info[1]); print(struct.calcsize('P') * 8)" 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $lines -or $lines.Count -lt 3) { return $null }
        [pscustomobject]@{
            Path  = $PythonExe
            Major = [int]$lines[0]
            Minor = [int]$lines[1]
            Bits  = [int]$lines[2]
        }
    } catch {
        return $null
    }
}

$candidates = Get-CandidateInterpreters
Write-Host "Interpretes de Python detectados: $($candidates.Count)"

$manifestLines = New-Object System.Collections.Generic.List[string]
$installedAny = $false

foreach ($exe in $candidates) {
    $info = Get-InterpreterInfo -PythonExe $exe
    if (-not $info) {
        Write-Host "  [omitido] $exe (no se pudo consultar su version)"
        continue
    }
    if ($info.Bits -ne 64) {
        Write-Host "  [omitido] $exe (Python de $($info.Bits) bits; la rueda es win_amd64)"
        continue
    }
    if ($info.Major -lt 3 -or ($info.Major -eq 3 -and $info.Minor -lt 10)) {
        Write-Host "  [omitido] $exe (Python $($info.Major).$($info.Minor); se requiere >= 3.10)"
        continue
    }

    $tag = "cp$($info.Major)$($info.Minor)"
    $wheel = Get-ChildItem -Path $WheelsDir -Filter "*-$tag-$tag-*.whl" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $wheel) {
        Write-Host "  [omitido] $exe (Python $($info.Major).$($info.Minor): esta release no incluye una rueda $tag)"
        continue
    }

    Write-Host "  Instalando en $exe (Python $($info.Major).$($info.Minor)): $($wheel.Name)"
    & $exe -m pip install --force-reinstall --no-deps "$($wheel.FullName)"
    if ($LASTEXITCODE -eq 0) {
        $manifestLines.Add($exe)
        $installedAny = $true
    } else {
        Write-Host "  [ERROR] 'pip install' fallo en $exe (codigo $LASTEXITCODE)"
    }
}

Set-Content -Path $ManifestPath -Value $manifestLines -Encoding UTF8

if ($installedAny) {
    Write-Host ""
    Write-Host "Listo. Prueba, por ejemplo: python -c ""import engine; print(engine.Engine().list_models())"""
} else {
    Write-Host ""
    Write-Host "No se ha instalado en ningun interprete (ver los mensajes anteriores)."
}
