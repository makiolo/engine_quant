#Requires -Version 5.1
<#
.SYNOPSIS
    Instala `quantdesk` en los interpretes de Python de 64 bits que tengan una rueda
    compatible en esta carpeta: por defecto, en todos los detectados; con -TargetPythonsFile,
    solo en los indicados ahi.
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
.PARAMETER TargetPythonsFile
    Fichero con una ruta a python.exe por linea: si se indica, instala EXACTAMENTE en esos
    interpretes (los que el usuario eligio, p.ej. en el asistente del instalador .exe) en vez
    de autodetectar todos los de la maquina. Cada ruta se sigue validando igual (version,
    arquitectura, rueda disponible). Si el fichero no existe o esta vacio, se ignora y se
    autodetecta como siempre.
.PARAMETER DiscoverOnly
    En vez de instalar, solo detecta los interpretes candidatos y escribe una linea por cada
    uno en -DiscoverOutputPath con el formato "ruta|major.minor|bits|version_de_quantdesk_ya_instalada_o_vacio",
    y termina. Pensado para que el asistente del instalador .exe rellene la lista de
    interpretes entre los que elegir (ver EngineQuantSetup.iss) sin duplicar la logica de
    deteccion en Pascal Script. El cuarto campo deja ver, por interprete, si ya tiene
    quantdesk instalado y que version, para saber cuales hace falta actualizar.
.PARAMETER DiscoverOutputPath
    Fichero de salida para -DiscoverOnly.
.PARAMETER ExtraCandidatesFile
    Solo con -DiscoverOnly: fichero adicional con una ruta a python.exe por linea (rutas que
    ya no se autodetectan solas y aun asi interesa ofrecer, p.ej. el manifiesto de una
    instalacion anterior -- ver EngineQuantSetup.iss) que se suma a los autodetectados. Una
    ruta que ya no exista se descarta sin error.
.EXAMPLE
    .\Install-EngineWheels.ps1
#>
[CmdletBinding()]
param(
    [string]$WheelsDir,
    [string]$ManifestPath,
    [string]$TargetPythonsFile,
    [switch]$DiscoverOnly,
    [string]$DiscoverOutputPath,
    [string]$ExtraCandidatesFile
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

# Una sola llamada a Python devuelve tanto version/arquitectura como -- si esta instalado -- la
# version de quantdesk ya instalada en ese interprete (para -DiscoverOnly: saber que
# interpretes hace falta actualizar sin lanzar un segundo proceso de Python por cada uno).
# Comillas simples adentro a proposito (no dobles): al pasar este script multilinea como
# argumento -c de un proceso nativo, PowerShell no escapa comillas dobles embebidas para la
# linea de comandos de Win32 -- se "comen" y rompen la sintaxis de Python (probado: con
# struct.calcsize("P") y print("") aqui, python.exe fallaba con "SyntaxError: unterminated
# string literal" en TODOS los interpretes, incluido uno que si tenia quantdesk instalado,
# y por tanto no se instalaba nada en ningun lado sin ningun error visible, porque el paso se
# ejecuta oculto con runhidden).
$InterpreterInfoScript = @'
import sys, struct
print(sys.version_info[0])
print(sys.version_info[1])
print(struct.calcsize('P') * 8)
try:
    import importlib.metadata as m
    print(m.version('quantdesk'))
except Exception:
    print('')
'@

function Get-InterpreterInfo {
    param([string]$PythonExe)
    try {
        $lines = & $PythonExe -c $InterpreterInfoScript 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $lines -or $lines.Count -lt 3) { return $null }
        [pscustomobject]@{
            Path            = $PythonExe
            Major           = [int]$lines[0]
            Minor           = [int]$lines[1]
            Bits            = [int]$lines[2]
            InstalledVersion = if ($lines.Count -ge 4) { $lines[3] } else { "" }
        }
    } catch {
        return $null
    }
}

if ($DiscoverOnly) {
    if ([string]::IsNullOrEmpty($DiscoverOutputPath)) {
        throw "Falta -DiscoverOutputPath con -DiscoverOnly"
    }
    $candidateExes = New-Object System.Collections.Generic.List[string]
    Get-CandidateInterpreters | ForEach-Object { $candidateExes.Add($_) }
    if (-not [string]::IsNullOrEmpty($ExtraCandidatesFile) -and (Test-Path $ExtraCandidatesFile)) {
        Get-Content $ExtraCandidatesFile | ForEach-Object { $_.Trim() } |
            Where-Object { $_ -ne "" -and (Test-Path $_) } |
            ForEach-Object { $candidateExes.Add($_) }
    }

    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($exe in ($candidateExes | Select-Object -Unique)) {
        $info = Get-InterpreterInfo -PythonExe $exe
        if (-not $info) { continue }
        $lines.Add("$($info.Path)|$($info.Major).$($info.Minor)|$($info.Bits)|$($info.InstalledVersion)")
    }
    Set-Content -Path $DiscoverOutputPath -Value $lines -Encoding UTF8
    return
}

if (-not [string]::IsNullOrEmpty($TargetPythonsFile) -and (Test-Path $TargetPythonsFile)) {
    $candidates = Get-Content $TargetPythonsFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
    Write-Host "Interpretes elegidos: $($candidates.Count)"
} else {
    $candidates = Get-CandidateInterpreters
    Write-Host "Interpretes de Python detectados: $($candidates.Count)"
}

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
