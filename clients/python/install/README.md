# Instalador del paquete de Python (multi-intérprete)

Instala `engine-quant` en los intérpretes de Python de 64 bits detectados en la máquina, sin
necesitar saber de antemano qué versiones hay instaladas. Usado tanto desde el instalador
`.exe` (`installer/EngineQuantSetup.iss`, que deja elegir en cuáles vía una página del
asistente — ver `installer/README.md`) como de forma independiente, por ejemplo en un
despliegue automatizado.

```powershell
# Instalar (en cada .whl de esta carpeta, en todo interprete >= 3.10 detectado que le corresponda)
.\Install-EngineWheels.ps1

# Instalar solo en los intérpretes indicados (uno por línea en este fichero), en vez de
# autodetectar todos los de la máquina
.\Install-EngineWheels.ps1 -TargetPythonsFile mis_pythons.txt

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
El instalador `.exe` también usa este mismo fichero para saber, en una actualización, en qué
intérpretes se instaló la vez anterior y premarcar exactamente esos en su página de selección
(ver "Persistencia de la selección entre instalaciones" en `installer/README.md`).

## Modo de solo detección (`-DiscoverOnly`)

`.\Install-EngineWheels.ps1 -DiscoverOnly -DiscoverOutputPath candidatos.txt` no instala nada:
escribe una línea por intérprete candidato con el formato
`ruta|major.minor|bits|version_de_engine_quant_ya_instalada_o_vacio` — el cuarto campo permite
saber, sin instalar nada, qué intérpretes ya tienen el paquete y con qué versión (para decidir
cuáles hace falta actualizar). Con `-ExtraCandidatesFile` se pueden sumar rutas que ya no se
autodetectan solas (p.ej. un manifiesto de una instalación anterior). Pensado para que
`installer/EngineQuantSetup.iss` rellene su página de selección sin duplicar la lógica de
detección en Pascal Script.
