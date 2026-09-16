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

; Copia embebida (no se instala en {app}) solo para poder ejecutar -DiscoverOnly durante el
; asistente, antes de que [Files] copie nada a {app}\wheels -- ver la pagina "Interpretes de
; Python" en [Code] mas abajo.
Source: "..\clients\python\install\Install-EngineWheels.ps1"; DestDir: "{tmp}"; Flags: dontcopy

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

; -TargetPythonsFile: los interpretes que el usuario eligio/anadio en la pagina "Interpretes de
; Python" del asistente (ver [Code]). Si esa pagina se salto (instalacion desatendida sin
; interaccion, p.ej. /VERYSILENT) el fichero no existe y el script cae de vuelta a autodetectar
; todos los interpretes validos, igual que antes.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\wheels\Install-EngineWheels.ps1"" -WheelsDir ""{app}\wheels"" -ManifestPath ""{app}\wheels\installed_pythons.txt"" -TargetPythonsFile ""{tmp}\selected_pythons.txt"""; \
    StatusMsg: "Instalando en los interpretes de Python elegidos..."; \
    Components: python; Flags: runhidden waituntilterminated runascurrentuser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\xll\Uninstall-EngineExcelAddin.ps1"" -RemoveFiles"; \
    Components: excel; Flags: runhidden waituntilterminated runascurrentuser; RunOnceId: "UninstallExcelAddin"

Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\wheels\Uninstall-EngineWheels.ps1"" -ManifestPath ""{app}\wheels\installed_pythons.txt"""; \
    Components: python; Flags: runhidden waituntilterminated runascurrentuser; RunOnceId: "UninstallPythonWheels"

; Pagina extra del asistente, entre "Elige componentes" y "Elige carpeta de destino": deja
; elegir en cuales interpretes de Python instalar (en vez de instalar siempre en TODOS los
; detectados sin preguntar, que era el comportamiento anterior) y anadir a mano uno que el
; autodetectado no encuentre (p.ej. un Miniconda/Anaconda que no se registro via PEP 514 ni
; el lanzador `py`). Se salta por completo si el componente "python" no esta marcado.
[Code]
var
  PythonPage: TWizardPage;
  PythonList: TNewCheckListBox;
  PythonAddButton: TNewButton;
  PythonDiscovered: Boolean;

procedure PythonAddButtonClick(Sender: TObject);
var
  FileName: String;
begin
  FileName := '';
  if GetOpenFileName('Selecciona python.exe', FileName, '', 'python.exe|python.exe|Todos los ficheros|*.*', '') then
    PythonList.AddCheckBox(FileName, '(anadido manualmente)', 0, True, True, False, True, nil);
end;

// Ejecuta Install-EngineWheels.ps1 -DiscoverOnly (misma logica de deteccion que la instalacion
// real, sin duplicarla aqui en Pascal Script) y rellena PythonList con lo que encuentre, todo
// marcado por defecto -- asi una instalacion desatendida (/VERYSILENT) que visite esta pagina
// sin interaccion se comporta igual que antes: instala en todos los detectados.
procedure DiscoverPythons;
var
  ScriptPath, OutPath, Params: String;
  ResultCode: Integer;
  Lines, Parts: TStringList;
  I: Integer;
begin
  ExtractTemporaryFile('Install-EngineWheels.ps1');
  ScriptPath := ExpandConstant('{tmp}\Install-EngineWheels.ps1');
  OutPath := ExpandConstant('{tmp}\pythons_discovered.txt');
  Params := '-NoProfile -ExecutionPolicy Bypass -File "' + ScriptPath + '" -DiscoverOnly -DiscoverOutputPath "' + OutPath + '"';
  WizardForm.Cursor := crHourGlass;
  try
    Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  finally
    WizardForm.Cursor := crDefault;
  end;

  if not FileExists(OutPath) then Exit;

  Lines := TStringList.Create;
  Parts := TStringList.Create;
  try
    Lines.LoadFromFile(OutPath);
    for I := 0 to Lines.Count - 1 do
    begin
      if Trim(Lines[I]) = '' then Continue;
      Parts.Delimiter := '|';
      Parts.StrictDelimiter := True;
      Parts.DelimitedText := Lines[I];
      if Parts.Count < 3 then Continue;
      PythonList.AddCheckBox(Parts[0], '(Python ' + Parts[1] + ', ' + Parts[2] + ' bits)', 0, True, True, False, True, nil);
    end;
  finally
    Lines.Free;
    Parts.Free;
  end;
end;

procedure InitializeWizard;
begin
  PythonPage := CreateCustomPage(wpSelectComponents, 'Interpretes de Python',
    'Elige en cuales instalar engine-quant, o anade uno que no se haya detectado automaticamente.');

  PythonList := TNewCheckListBox.Create(PythonPage);
  PythonList.Parent := PythonPage.Surface;
  PythonList.Left := 0;
  PythonList.Top := 0;
  PythonList.Width := PythonPage.SurfaceWidth;
  PythonList.Height := PythonPage.SurfaceHeight - ScaleY(31);
  PythonList.Flat := True;

  PythonAddButton := TNewButton.Create(PythonPage);
  PythonAddButton.Parent := PythonPage.Surface;
  PythonAddButton.Caption := 'Anadir manualmente...';
  PythonAddButton.Width := WizardForm.CalculateButtonWidth([PythonAddButton.Caption]);
  PythonAddButton.Height := ScaleY(23);
  PythonAddButton.Left := 0;
  PythonAddButton.Top := PythonList.Top + PythonList.Height + ScaleY(8);
  PythonAddButton.OnClick := @PythonAddButtonClick;

  PythonDiscovered := False;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (PythonPage <> nil) and (PageID = PythonPage.ID) then
    Result := not IsComponentSelected('python');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (PythonPage <> nil) and (CurPageID = PythonPage.ID) and (not PythonDiscovered) then
  begin
    DiscoverPythons;
    PythonDiscovered := True;
    if PythonList.Items.Count = 0 then
      PythonList.AddCheckBox('(Ninguno detectado automaticamente; usa "Anadir manualmente...")', '', 0, False, False, False, False, nil);
  end;
end;

// Vuelca lo marcado en PythonList a {tmp}\selected_pythons.txt justo antes de instalar, para que
// el paso [Run] de Install-EngineWheels.ps1 lo lea via -TargetPythonsFile.
procedure CurStepChanged(CurStep: TSetupStep);
var
  I: Integer;
  Selected: TStringList;
begin
  if (CurStep = ssInstall) and (PythonList <> nil) then
  begin
    Selected := TStringList.Create;
    try
      for I := 0 to PythonList.Items.Count - 1 do
        if PythonList.Checked[I] then
          Selected.Add(PythonList.Items[I]);
      Selected.SaveToFile(ExpandConstant('{tmp}\selected_pythons.txt'));
    finally
      Selected.Free;
    end;
  end;
end;
