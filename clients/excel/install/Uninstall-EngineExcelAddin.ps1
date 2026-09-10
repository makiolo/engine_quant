#Requires -Version 5.1
<#
.SYNOPSIS
    Desinstala el complemento de Excel (XLL) del motor XVA para el usuario actual.
.DESCRIPTION
    Simetrico de Install-EngineExcelAddin.ps1: borra el valor OPEN/OPEN1/OPEN2/... que
    apuntaba a este complemento en HKCU\...\Excel\Options y, opcionalmente, los ficheros
    copiados en su momento a la carpeta de instalacion.
.PARAMETER InstallDir
    Carpeta donde Install-EngineExcelAddin.ps1 copio engine_excel.xll y sus DLL. Por defecto,
    %LOCALAPPDATA%\engine_quant\excel.
.PARAMETER RemoveFiles
    Si se indica, borra tambien InstallDir despues de desregistrar el complemento.
.EXAMPLE
    .\Uninstall-EngineExcelAddin.ps1 -RemoveFiles
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [switch]$RemoveFiles
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrEmpty($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA "engine_quant\excel"
}

$xllName = "engine_excel.xll"
$targetXll = Join-Path $InstallDir $xllName
$openValue = "/R `"$targetXll`""

$optionsKeys = Get-ChildItem "HKCU:\Software\Microsoft\Office" -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.PSPath "Excel\Options" } |
    Where-Object { Test-Path $_ }

$removedAny = $false
foreach ($optionsKey in $optionsKeys) {
    $openNames = (Get-Item -Path $optionsKey).Property | Where-Object { $_ -match '^OPEN\d*$' }
    foreach ($name in $openNames) {
        if ((Get-ItemProperty -Path $optionsKey -Name $name).$name -eq $openValue) {
            Remove-ItemProperty -Path $optionsKey -Name $name -Force
            Write-Host "Quitado de $optionsKey (valor $name)."
            $removedAny = $true
        }
    }
}

if (-not $removedAny) {
    Write-Host "El complemento no estaba registrado (o ya se habia quitado antes)."
}

if ($RemoveFiles -and (Test-Path $InstallDir)) {
    Write-Host "Borrando $InstallDir ..."
    Remove-Item -Path $InstallDir -Recurse -Force
}

Write-Host "Listo. Los cambios se aplican la proxima vez que abras Excel."
