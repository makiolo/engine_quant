; Instalador grafico (wizard) del motor XVA (PLAN.md Fase 4/§7.9): registra el complemento de
; Excel y/o instala la rueda de Python en los interpretes detectados, sin que el usuario tenga
; que descomprimir nada ni ejecutar PowerShell a mano. Compilado con Inno Setup 6
; (ISCC.exe EngineQuantSetup.iss, ver .github/workflows/release.yml, job build-installer).
;
; Layout de "payload\" que este script espera (montado por el job build-installer a partir de
; los artefactos de build-wheels/build-xll, ver ese job para el detalle exacto):
;   payload\xll\      engine_excel.xll, msvcp140.dll, Install-/Uninstall-EngineExcelAddin.ps1
;   payload\wheels\   *.whl (una por version de CPython) + Install-/Uninstall-EngineWheels.ps1
;
; AppId fijo (generado una vez, no cambiar nunca): con el mismo AppId, instalar una version mas
; nueva sobre una ya instalada la detecta como "la misma app" y actualiza en el mismo sitio en
; vez de crear una segunda entrada en Programas y caracteristicas. No hace falta desinstalar la
; version anterior a mano primero: los propios pasos de instalacion (Install-EngineExcelAddin.ps1
; -- idempotente, ver ese script -- y Install-EngineWheels.ps1 -- `pip install
; --force-reinstall`) sobrescriben correctamente el complemento/la rueda de una version anterior
; al volver a ejecutarse sobre los ficheros nuevos ya copiados a {app}.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif

[Setup]
AppId={{C3C5CE8D-CE38-461E-A633-81B65EE77AE3}
AppName=Motor XVA (engine-quant)
AppVersion={#MyAppVersion}
AppPublisher=engine_quant
DefaultDirName={autopf}\engine_quant
DisableProgramGroupPage=yes
UninstallDisplayName=Motor XVA (engine-quant)
OutputDir=..\dist_installer
OutputBaseFilename=engine_quant_setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Components]
Name: "excel"; Description: "Complemento de Excel (XLL)"
Name: "python"; Description: "Paquete de Python (en los interpretes de Python 3.10+ detectados)"

; El nombre del .whl incluye la version ("engine_quant-1.2.3-cp312-...whl"): sin este borrado
; previo, una actualizacion ANADE el .whl nuevo junto al antiguo en vez de sustituirlo (Inno
; Setup no borra ficheros que ya no forman parte de [Files] solo porque cambien de nombre), y
; Install-EngineWheels.ps1 podria acabar reinstalando el mas antiguo de los dos (probado: asi
; ocurria antes de anadir este [InstallDelete]). Corre antes de copiar los ficheros nuevos.
[InstallDelete]
Type: filesandordirs; Name: "{app}\wheels"
Type: filesandordirs; Name: "{app}\xll"

[Files]
Source: "payload\xll\*"; DestDir: "{app}\xll"; Flags: ignoreversion recursesubdirs; Components: excel
Source: "payload\wheels\*"; DestDir: "{app}\wheels"; Flags: ignoreversion recursesubdirs; Components: python

; runascurrentuser: tanto el complemento de Excel (registro en HKCU) como los interpretes de
; Python "solo para mi" del usuario deben instalarse/desinstalarse como el usuario que lanzo el
; instalador, no como el token elevado de administrador (PrivilegesRequired=admin arriba).
; -Optional en Install-EngineExcelAddin.ps1: si la maquina no tiene Excel, termina con exito en
; vez de fallar el instalador entero (ver ese script).
[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\xll\Install-EngineExcelAddin.ps1"" -Optional"; \
    StatusMsg: "Registrando el complemento de Excel..."; \
    Components: excel; Flags: runhidden waituntilterminated runascurrentuser

Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\wheels\Install-EngineWheels.ps1"" -WheelsDir ""{app}\wheels"" -ManifestPath ""{app}\wheels\installed_pythons.txt"""; \
    StatusMsg: "Instalando en los interpretes de Python detectados..."; \
    Components: python; Flags: runhidden waituntilterminated runascurrentuser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\xll\Uninstall-EngineExcelAddin.ps1"" -RemoveFiles"; \
    Components: excel; Flags: runhidden waituntilterminated runascurrentuser; RunOnceId: "UninstallExcelAddin"

Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\wheels\Uninstall-EngineWheels.ps1"" -ManifestPath ""{app}\wheels\installed_pythons.txt"""; \
    Components: python; Flags: runhidden waituntilterminated runascurrentuser; RunOnceId: "UninstallPythonWheels"
