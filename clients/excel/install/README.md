# Instalador del complemento de Excel

Estos dos scripts (des)registran `engine_excel.xll` en Excel para el usuario actual, sin
pasos manuales en Archivo > Opciones > Complementos. Pensados para ejecutarse desde la
carpeta descomprimida del asset `engine_excel-<version>-win_amd64.zip` de una release
(`engine_excel.xll`, `msvcp140.dll` y estos dos `.ps1` en la misma carpeta).

```powershell
# Instalar (copia el .xll + DLL a %LOCALAPPDATA%\engine_quant\excel y lo registra)
.\Install-EngineExcelAddin.ps1

# Quitar
.\Uninstall-EngineExcelAddin.ps1 -RemoveFiles
```

Tras instalar, abre una instancia **nueva** de Excel (una ya abierta no recoge el cambio) y
prueba, por ejemplo, `=ENGINE.LIST_MODELS()` en una celda.

## Cómo funciona

`Install-EngineExcelAddin.ps1` escribe directamente la clave del registro que Excel usa para
complementos persistentes por usuario: `HKCU\Software\Microsoft\Office\<versión>\Excel\Options`,
valores `OPEN`/`OPEN1`/`OPEN2`/... con formato `/R "ruta\al\complemento"` — el mismo mecanismo
que usa el propio diálogo de Complementos al marcar un XLL (verificado contra un complemento
ya instalado por otra vía en la máquina donde se desarrolló este script). Se prefiere a
automatizar Excel por COM (`Application.AddIns`) porque no depende de lanzar una instancia
real de Excel: más rápido, sin ventanas ni cuadros de diálogo, y sin la fragilidad de la
automatización COM con Excel oculto (en algunos entornos —el usado para desarrollar este
script incluido— las llamadas COM a una instancia de Excel oculta se rechazan con
`RPC_E_CALL_REJECTED` por motivos ajenos al complemento; el enfoque de registro lo evita
por completo).

## Dependencias

`engine_excel.xll` enlaza Rust estáticamente (PLAN.md §5.4): no hace falta el toolchain de
Rust para usarlo. La única dependencia dinámica que no forma parte de Windows es el runtime
de C++ (`MSVCP140.dll`); `vcruntime140.dll`/`vcruntime140_1.dll` se consideran ya presentes
en cualquier Windows 10/11 actualizado (forman parte del CRT universal servido vía Windows
Update, mismo criterio que aplica `delvewheel` a la rueda de Python — ver
`.github/workflows/release.yml`). `msvcp140.dll` viaja ya en el zip de la release junto al
`.xll`, en la misma carpeta: Windows busca primero ahí las DLL de las que depende un módulo
cargado por ruta completa (que es como Excel carga un XLL), así que no hace falta instalar
nada más.
