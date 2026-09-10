#Requires -Version 5.1
<#
.SYNOPSIS
    Instala el complemento de Excel (XLL) del motor XVA para el usuario actual.
.DESCRIPTION
    Copia engine_excel.xll y las DLL de runtime que lo acompanan (ver README.md de esta
    carpeta) a una ubicacion estable del usuario y lo registra en Excel escribiendo
    directamente la clave del registro que Excel usa para complementos persistentes por
    usuario (HKCU\...\Excel\Options, valores OPEN/OPEN1/OPEN2/... con formato
    `/R "ruta\al\complemento"`) -- el mismo mecanismo que usa el propio dialogo
    Archivo > Opciones > Complementos > Ir... al marcar un XLL, verificado en este equipo
    contra un complemento ya instalado por otra via. Se prefiere a automatizar Excel por COM
    (Application.AddIns) porque no depende de lanzar una instancia real de Excel (mas rapido,
    sin ventanas ni cuadros de dialogo, y sin la fragilidad de la automatizacion COM con Excel
    oculto).

    Rust queda enlazado estaticamente dentro del propio .xll (PLAN.md Fase 4/5.4); la unica
    dependencia externa es el runtime de C++ (MSVCP140.dll), que viaja junto al .xll en el
    paquete de la release (ver .github/workflows/release.yml) -- no hace falta tener Visual
    Studio ni Rust instalados para usarlo.

    Pensado para ejecutarse desde la carpeta descomprimida del asset de la release
    (engine_excel.xll y *.dll en el mismo directorio que este script).
.PARAMETER SourceDir
    Carpeta que contiene engine_excel.xll y sus DLL. Por defecto, la carpeta de este script.
.PARAMETER InstallDir
    Carpeta estable donde se copian engine_excel.xll y sus DLL antes de registrarlo (Excel
    resuelve las DLL de un XLL buscando primero en la carpeta del propio XLL). Por defecto,
    %LOCALAPPDATA%\engine_quant\excel.
.EXAMPLE
    .\Install-EngineExcelAddin.ps1
#>
[CmdletBinding()]
param(
    [string]$SourceDir,
    [string]$InstallDir
)

$ErrorActionPreference = "Stop"

# Valores por defecto resueltos aqui (no en el bloque param()): $PSScriptRoot puede llegar
# vacio ahi segun como se invoque el script (p.ej. `powershell -File ...` directo).
if ([string]::IsNullOrEmpty($SourceDir)) {
    $SourceDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
}
if ([string]::IsNullOrEmpty($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA "engine_quant\excel"
}

$xllName = "engine_excel.xll"
$sourceXll = Join-Path $SourceDir $xllName
if (-not (Test-Path $sourceXll)) {
    throw "No se encuentra $xllName en '$SourceDir'. Ejecuta este script desde la carpeta descomprimida del asset de la release (junto a engine_excel.xll)."
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Write-Host "Copiando $xllName y dependencias a $InstallDir ..."
Get-ChildItem -Path $SourceDir -Filter "*.dll" -ErrorAction SilentlyContinue | Copy-Item -Destination $InstallDir -Force
Copy-Item -Path $sourceXll -Destination $InstallDir -Force

$targetXll = (Resolve-Path (Join-Path $InstallDir $xllName)).Path
$openValue = "/R `"$targetXll`""

# Cada version de Office instalada (Excel 2016/2019/2021/365 comparten "16.0") con Excel ya
# ejecutado alguna vez tiene su propia clave HKCU\...\Office\<version>\Excel\Options: se
# registra en todas las que existan (normalmente una sola).
$optionsKeys = Get-ChildItem "HKCU:\Software\Microsoft\Office" -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.PSPath "Excel\Options" } |
    Where-Object { Test-Path $_ }

if (-not $optionsKeys) {
    throw "No se encontro ninguna clave HKCU\Software\Microsoft\Office\<version>\Excel\Options. ¿Esta Excel instalado y se ha ejecutado al menos una vez en esta cuenta?"
}

$registered = $false
foreach ($optionsKey in $optionsKeys) {
    $openNames = (Get-Item -Path $optionsKey).Property | Where-Object { $_ -match '^OPEN\d*$' }

    $alreadyThere = $false
    foreach ($name in $openNames) {
        if ((Get-ItemProperty -Path $optionsKey -Name $name).$name -eq $openValue) {
            $alreadyThere = $true
            break
        }
    }
    if ($alreadyThere) {
        Write-Host "Ya estaba registrado en $optionsKey."
        $registered = $true
        continue
    }

    $slot = "OPEN"
    $n = 1
    while ($openNames -contains $slot) {
        $slot = "OPEN$n"
        $n++
    }

    New-ItemProperty -Path $optionsKey -Name $slot -PropertyType String -Value $openValue -Force | Out-Null
    Write-Host "Registrado en $optionsKey (valor $slot)."
    $registered = $true
}

if (-not $registered) {
    throw "No se pudo registrar el complemento en ninguna clave de Excel encontrada."
}

Write-Host ""
Write-Host "Listo. Abre Excel (una instancia nueva; una ya abierta no lo recoge) y prueba, por ejemplo: =ENGINE.LIST_MODELS()"
Write-Host "Para quitarlo: .\Uninstall-EngineExcelAddin.ps1"
