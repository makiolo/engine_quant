# Instalador Windows (`engine_quant_setup.exe`)

Wizard de "siguiente, siguiente, instalar" (Inno Setup) que empaqueta en un único `.exe` los
dos clientes del motor (PLAN.md §7.9): el complemento de Excel y el paquete de Python. Es el
asset recomendado para quien solo quiere usar el motor, sin descomprimir zips ni tocar
PowerShell a mano — esa vía manual (`clients/excel/install/`, `clients/python/install/`)
sigue disponible para quien la prefiera o quiera automatizarla de otra forma.

## Qué hace

- **Complemento de Excel** (casilla marcable en el asistente): copia `engine_excel.xll` y su
  DLL de runtime a `%LOCALAPPDATA%\engine_quant\excel` y lo registra para el usuario actual —
  internamente ejecuta `Install-EngineExcelAddin.ps1` (`clients/excel/install/`), con
  `-Optional` para no fallar si la máquina no tiene Excel instalado.
- **Paquete de Python** (casilla marcable en el asistente): detecta todos los intérpretes de
  Python de 64 bits instalados (vía el registro, PEP 514, y el lanzador `py` si está
  presente) y muestra una página propia del asistente ("Intérpretes de Python") con uno
  marcable por cada uno, indicando junto a cada uno si ya tiene `engine-quant` instalado y qué
  versión ("no instalado" / "ya al día, vX.Y.Z" / "vX.Y.Z, se actualizará a vA.B.C") — se puede
  desmarcar el que no interese, o pulsar "Añadir manualmente..." para señalar con un selector
  de fichero un `python.exe` que no se haya detectado solo (p.ej. un Miniconda/Anaconda
  instalado "solo para mí" que no se registró vía PEP 514 ni el lanzador `py`). Solo instala en
  los que queden marcados al llegar a "Instalar" — internamente ejecuta
  `Install-EngineWheels.ps1` (`clients/python/install/`) con `-TargetPythonsFile`, sin
  necesitar una lista fija de versiones: cualquier intérprete ≥ 3.10 para el que la release
  incluya una rueda (`cp310`–`cp314` hoy, futuras versiones de Python en cuanto tengan su
  rueda) se puede elegir.
  - **Preselección**: en la primera instalación vienen todos marcados. En una actualización,
    solo vienen premarcados los intérpretes en los que se instaló la última vez (leído del
    manifiesto que dejó esa instalación anterior) — si más adelante se instala en uno distinto
    (p.ej. se cambia de Python), ese pasa a ser "el último instalado" y será el premarcado la
    próxima vez, porque el manifiesto se reescribe en cada instalación con lo que quedó
    efectivamente instalado.
  - Una instalación desatendida (`/VERYSILENT`) no muestra esta página pero se comporta igual
    que antes: instala en todos los intérpretes detectados válidos.
- **Desinstalar**: Panel de control → Programas y características → "Motor XVA
  (engine-quant)" → Desinstalar. Deshace exactamente lo anterior (desregistra el complemento,
  ejecuta `pip uninstall` en los mismos intérpretes donde se instaló — un manifiesto interno
  recuerda cuáles fueron).
- **Actualizar**: instalar una versión más reciente sobre una ya instalada la sustituye en el
  mismo sitio (mismo `AppId`, fijo entre releases) — no aparece una segunda entrada en
  Programas y características, y la rueda/el `.xll` antiguos no se quedan a medio camino
  junto a los nuevos (`[InstallDelete]` limpia `{app}\wheels`/`{app}\xll` antes de copiar los
  ficheros de la versión nueva; verificado en desarrollo con un ciclo 9.9.9 → 9.9.10 antes de
  confiar en este comportamiento).

## Compilar (Inno Setup 6)

```
choco install innosetup   # o descargarlo de https://jrsoftware.org/isinfo.php
```

El script (`EngineQuantSetup.iss`) espera este layout en `installer\payload\` (lo monta el
job `build-installer` de `.github/workflows/release.yml` a partir de los artefactos de
`build-wheels`/`build-xll`; para probar en local hay que montarlo a mano una vez):

```
installer\payload\xll\      engine_excel.xll, msvcp140.dll,
                             Install-/Uninstall-EngineExcelAddin.ps1, README.md
installer\payload\wheels\   *.whl (una por version de CPython),
                             Install-/Uninstall-EngineWheels.ps1
```

```
ISCC.exe /DMyAppVersion=1.2.3 installer\EngineQuantSetup.iss
```

Produce `dist_installer\engine_quant_setup.exe`.

## Instalación desatendida

`engine_quant_setup.exe /VERYSILENT /SUPPRESSMSGBOXES /COMPONENTS="excel,python"` — el
asistente interactivo marca ambos componentes por defecto, pero una instalación desatendida
sin `/COMPONENTS` **no instala ninguno** (comprobado en desarrollo: solo queda el
desinstalador, sin `.xll` ni ruedas) — hay que indicarlos explícitamente. Para desinstalar en
silencio: `"<carpeta de instalación>\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES`.

## Notas de diseño

- **`AppId` fijo** (`{C3C5CE8D-CE38-461E-A633-81B65EE77AE3}`, generado una vez): no cambiar
  nunca — es lo que le permite a Windows reconocer una versión nueva como "la misma app" al
  actualizar en vez de instalar una entrada duplicada.
- **`PrivilegesRequired=admin`**: hace falta para poder instalar en cualquier intérprete de
  Python detectado (algunos, como una instalación "para todos los usuarios", solo son
  escribibles con permisos de administrador) y para escribir en `Program Files`. Los pasos
  que sí son por-usuario (registro de Excel en `HKCU`, intérpretes de Python "solo para mí")
  se ejecutan con `Flags: runascurrentuser` para no acabar operando sobre el perfil de
  administrador en vez del del usuario que lanzó el instalador.
- **Sin lista fija de versiones de Python**: `Install-EngineWheels.ps1` pregunta a cada
  intérprete detectado su propia versión (`sys.version_info`) y busca una rueda que encaje
  por nombre de fichero (`*-cp<major><minor>-cp<major><minor>-*.whl`) — añadir una versión de
  Python nueva a la matriz de `build-wheels` (`.github/workflows/release.yml`) es lo único
  que hace falta para que el instalador la soporte, sin tocar este `.iss` ni el script.
- **Página "Intérpretes de Python" (`[Code]` en `EngineQuantSetup.iss`)**: para poder listar
  los intérpretes antes de que `[Files]` copie nada a `{app}\wheels`, el `.iss` embebe una
  copia de `Install-EngineWheels.ps1` con `Flags: dontcopy` y la ejecuta con `-DiscoverOnly
  -DiscoverOutputPath` durante el asistente (misma función `Get-CandidateInterpreters` que la
  instalación real, no duplicada en Pascal Script; el cuarto campo que devuelve cada línea es
  la versión de `engine-quant` ya instalada en ese intérprete, o vacío). Lo que queda marcado
  se vuelca a `{tmp}\selected_pythons.txt` justo antes de instalar (`CurStepChanged(ssInstall)`),
  y el paso `[Run]` se lo pasa a `Install-EngineWheels.ps1` vía `-TargetPythonsFile`. Si ese
  fichero no existe (página saltada, p.ej. `/VERYSILENT`), el script cae de vuelta a
  autodetectar todos los intérpretes válidos — comportamiento idéntico al de antes de esta
  página.
- **Persistencia de la selección entre instalaciones**: `{app}\wheels\installed_pythons.txt`
  (el manifiesto que ya usaba `Uninstall-EngineWheels.ps1` para saber de dónde quitar el
  paquete) es también la fuente de verdad de "qué se instaló la última vez". `DiscoverPythons`
  lo lee (`LoadPreviousManifest`) *antes* de que `[InstallDelete]` borre `{app}\wheels` para
  copiar los ficheros nuevos — se lee durante la navegación del asistente, no durante la
  instalación en sí, así que todavía existe — y lo pasa también como
  `-ExtraCandidatesFile` a la detección, para que un intérprete "añadido a mano" en una
  instalación anterior (que sigue sin autodetectarse solo) reaparezca igualmente en la lista.
