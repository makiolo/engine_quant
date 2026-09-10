# Instalador del paquete de Python (multi-intérprete)

Instala `engine-quant` en todos los intérpretes de Python de 64 bits detectados en la
máquina, sin necesitar saber de antemano qué versiones hay instaladas. Usado tanto desde el
instalador `.exe` (`installer/EngineQuantSetup.iss`) como de forma independiente, por ejemplo
en un despliegue automatizado.

```powershell
# Instalar (en cada .whl de esta carpeta, en el interprete >= 3.10 que le corresponda)
.\Install-EngineWheels.ps1

# Quitar (de los mismos interpretes donde se instaló, via el manifiesto que dejó el anterior)
.\Uninstall-EngineWheels.ps1
```

Pensado para ejecutarse desde la carpeta descomprimida del asset de la release
(`Install-EngineWheels.ps1` y los `*.whl`, uno por versión de CPython, en el mismo
directorio).

## Cómo detecta los intérpretes

Vía el registro (PEP 514, `HKLM`/`HKCU\SOFTWARE\Python\PythonCore`, incluida la vista de 32
bits en un Windows de 64 — comprobado que Miniconda también se registra ahí, no solo los
instaladores de python.org) y, si está presente, vía el lanzador `py` (`py -0p`). Para cada
intérprete encontrado se le pregunta directamente su versión y arquitectura
(`sys.version_info`, `struct.calcsize('P') * 8`) en vez de fiarse del nombre de la clave del
registro — así una entrada obsoleta (paquete desinstalado, entorno borrado) se descarta sola
en vez de fallar. Un intérprete de 32 bits, o de Python < 3.10, o para el que esta release no
incluya rueda, se omite con un mensaje explicativo, sin detener el resto.

## Manifiesto

`Install-EngineWheels.ps1` anota en `installed_pythons.txt` (junto a los `.whl`, salvo que se
indique `-ManifestPath`) en qué intérpretes instaló el paquete, para que
`Uninstall-EngineWheels.ps1` sepa exactamente de dónde quitarlo después sin tener que repetir
todo el proceso de detección ni arriesgarse a desinstalar de un intérprete que nunca lo tuvo.
