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
  presente) e instala en cada uno la rueda que le corresponda — internamente ejecuta
  `Install-EngineWheels.ps1` (`clients/python/install/`), sin necesitar una lista fija de
  versiones: cualquier intérprete ≥ 3.10 para el que la release incluya una rueda
  (`cp310`–`cp314` hoy, futuras versiones de Python en cuanto tengan su rueda) se detecta
  automáticamente.
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
