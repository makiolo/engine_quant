#Requires -Version 5.1
<#
.SYNOPSIS
    Desinstala `engine-quant` de los interpretes de Python donde Install-EngineWheels.ps1 lo
    instalo.
.DESCRIPTION
    Simetrico de Install-EngineWheels.ps1: lee el manifiesto que escribio (un python.exe por
    linea) y ejecuta `pip uninstall -y engine-quant` en cada uno. Un interprete que ya no
    exista (desinstalado, entorno borrado) se omite sin error.
.PARAMETER ManifestPath
    Fichero escrito por Install-EngineWheels.ps1. Por defecto, installed_pythons.txt junto a
    este script.
.EXAMPLE
    .\Uninstall-EngineWheels.ps1
#>
[CmdletBinding()]
param(
    [string]$ManifestPath
)

if ([string]::IsNullOrEmpty($ManifestPath)) {
    $scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
    $ManifestPath = Join-Path $scriptDir "installed_pythons.txt"
}

if (-not (Test-Path $ManifestPath)) {
    Write-Host "No hay manifiesto de instalacion ($ManifestPath); nada que desinstalar."
    exit 0
}

$pythons = Get-Content $ManifestPath | Where-Object { $_.Trim() -ne "" }
foreach ($exe in $pythons) {
    if (-not (Test-Path $exe)) {
        Write-Host "  [omitido] $exe (ya no existe)"
        continue
    }
    Write-Host "  Desinstalando de $exe ..."
    & $exe -m pip uninstall -y engine-quant
}

Remove-Item -Path $ManifestPath -Force -ErrorAction SilentlyContinue
Write-Host "Listo."
