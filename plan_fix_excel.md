# Plan de fix: Excel solo muestra el primer elemento de los rangos dinámicos

## Resultado del análisis

El problema no está en la lista que genera el motor. La ruta actual construye un
`XLOPER12` de tipo `xltypeMulti` con `rows = values.size()` y `columns = 1` en
`clients/excel/src/xloper.cpp::new_string_column`. Además, cada celda y el array
completo se marcan para ser liberados por `xlAutoFree12`.

La ruta es:

```text
HandleRegistry::list_measures()
  -> engine::price_measure_names(registries_)
  -> xlEngineListMeasures()
  -> new_string_column(vector<string>)
  -> XLOPER12 xltypeMulti (N x 1)
  -> Excel mediante xlfRegister
```

Por tanto, que Excel enseñe un único valor apunta a la frontera XLL/Excel o al
modo de cálculo del Excel instalado, no a una conversión de `vector<string>` a
un escalar.

## Evidencia del código actual

| Punto | Observación |
|---|---|
| `clients/excel/src/handles.cpp:14` | `list_measures()` devuelve la lista completa producida por `price_measure_names`. |
| `cpp/engine/src/price.cpp:293-299` | La lista se crea a partir del registry y añade aliases; no devuelve un único nombre por diseño. |
| `clients/excel/src/engine_excel.cpp:79-82` | `xlEngineListMeasures()` delega directamente en `new_string_column`. |
| `clients/excel/src/xloper.cpp:260-278` | Se reserva un `xltypeMulti`, con `rows = values.size()` y una columna. |
| `clients/excel/src/engine_excel.cpp:343-347` | La función se registra con type text `U`; el arnés no comprueba el contrato de tipos registrado. |
| `clients/excel/tests/xll_harness_test.cpp:190-201` | Solo verifica que exista `xltypeMulti` y que aparezca `UnilateralCVA`; no verifica `rows`, `columns`, número de elementos ni `type_text`. |

La documentación de Excel define el primer carácter de `pxTypeText` como el
tipo de retorno y distingue los códigos `Q`/`U` para valores `XLOPER12`. Para
este plan se toma como requisito la semántica de spill de Excel moderno.
Referencias: [xlfRegister (Form 1)](https://learn.microsoft.com/en-us/office/client-developer/excel/xlfregister-form-1)
y [Spilled range operator](https://support.microsoft.com/en-US/Excel/spilled-range-operator).

## Hipótesis ordenadas

El objetivo de este fix será Excel moderno; no se conservará compatibilidad con
Excel antiguo ni con fórmulas matriciales CSE.

1. **El XLL se está ejecutando fuera del contrato de arrays dinámicos de Excel
   moderno.** En Microsoft 365/Excel 2021, una UDF que devuelve una matriz debe
   derramarla desde la celda superior izquierda. Un rango bloqueado debe producir
   `#SPILL!`, no reducir silenciosamente el resultado a la primera celda.
2. **La función está registrada con un contrato de salida que Excel no está
   interpretando como matriz dinámica.** El código usa `U` para todos los
   retornos `LPXLOPER12`; hay que probar de forma controlada `U` frente a `Q`
   para las UDF que devuelven `xltypeMulti`, sin cambiar globalmente las UDF que
   devuelven un escalar o un string.
3. **Excel conserva una registración antigua del XLL.** `xlAutoOpen()` no
   inspecciona el resultado de `xlfRegister()` y el arnés simulado tampoco
   captura el `type_text`; al actualizar el binario se puede estar probando una
   firma registrada previamente.
4. **Problema de dimensiones o memoria en el bridge.** Es la hipótesis menos
   probable: las dimensiones se escriben correctamente y `free_xloper()` recorre
   y libera todas las celdas, pero falta una aserción de integración que lo
   demuestre para las listas reales.

## Plan de implementación

### Fase 1 — Reproducir en Excel moderno y separar Excel del bridge

- Confirmar el nombre registrado exacto: `ENGINE.LIST_MEASURES()`; comprobar
  también `ENGINE.LIST_MODELS()`, `ENGINE.LIST_PRODUCTS()` y
  `ENGINE.LIST_CALIBRATORS()`.
- Declarar como requisito Excel para Microsoft 365 o Excel 2021 con arrays
  dinámicos habilitados. No invertir tiempo en validar Excel antiguo.
- Probar la fórmula fuera de una Tabla de Excel, con celdas libres debajo y el
  complemento recargado completamente. La fórmula canónica será
  `=ENGINE.LIST_MEASURES()`.
- Registrar en el arnés y, temporalmente, en una compilación de diagnóstico:
  `values.size()`, `rows`, `columns`, tipo base, `xlbitDLLFree` y el `type_text`
  enviado a `xlfRegister`.
- Criterio de aislamiento: si el arnés obtiene `N > 1`, `columns == 1` y todos
  los nombres, el motor y `xloper.cpp` quedan descartados como causa primaria;
  el trabajo se concentra en el registro XLL y la evaluación de Excel moderno.

### Fase 2 — Completar las pruebas automatizadas

- Añadir una prueba de `new_string_column()` que fuerce al menos tres strings y
  compruebe `xltypeMulti`, `rows == 3`, `columns == 1`, el orden y el contenido
  de las tres celdas.
- Fortalecer `xll_harness_test.cpp` para comprobar que las listas reales tienen
  más de una fila y que cada fila contiene un nombre esperado.
- Hacer que el stub de `MdCallBack12` capture por función:
  `procedure`, `type_text`, `function_name`, número de argumentos y resultado de
  `xlfRegister`. El test debe fallar si una registración devuelve error.
- Verificar explícitamente que todas las UDF de resultados tabulares
  (`LIST_*`, `PRICE*`, Greeks, Hessian, HVP y calibración) tienen una forma de
  salida consistente y rectangular.

### Fase 3 — Corregir el contrato de registro

- Separar en `FnSpec` el tipo de retorno de las UDF escalares/string y el de las
  UDF que pueden devolver `xltypeMulti`.
- Ejecutar una prueba A/B en Excel real con el mismo `XLOPER12` y únicamente
  cambiando el primer carácter del `type_text` (`U` frente a `Q`) para las
  funciones tabulares. Adoptar el código que Excel documente y que produzca
  spill en Excel moderno, dejando intactos los argumentos `Q`.
- Aplicar el resultado solo a las entradas de `kFunctions` que devuelven
  matrices; no hacer un reemplazo global de `U`, porque `ENGINE.VERSION` y los
  handles son retornos escalares/string.
- Comprobar el valor devuelto por `xlfRegister()` en `xlAutoOpen()`. Si falla,
  devolver error de carga o emitir un diagnóstico inequívoco en vez de dejar una
  registración parcial.
- Revisar el ciclo de recarga: cerrar Excel o descargar explícitamente el XLL,
  cargar el nuevo binario y evitar que una definición anterior de la función
  siga activa.

### Fase 4 — Contrato explícito de arrays dinámicos modernos

- Documentar Excel para Microsoft 365/Excel 2021 como requisito mínimo del
  complemento para resultados tabulares.
- Mantener `xltypeMulti` como única representación de listas y tablas; no añadir
  una ruta escalar, concatenada o basada en tamaño fijo.
- Confirmar que `=ENGINE.LIST_MEASURES()` derrama verticalmente y que
  `=ENGINE.LIST_MEASURES()#` puede alimentar otras fórmulas dinámicas.
- Confirmar que las funciones de varias columnas (`PRICE`, Greeks, Hessian y HVP)
  derraman la tabla completa sin reservar previamente un rango.
- Si el spill está bloqueado, aceptar `#SPILL!` como señal visible y accionable;
  no ocultarlo devolviendo solo la primera celda.

### Fase 5 — Validación final

- Excel moderno: `=ENGINE.LIST_MEASURES()` derrama exactamente `N x 1` y
  `=ENGINE.LIST_MEASURES()#` referencia todo el spill.
- Excel moderno: `LIST_MODELS`, `LIST_PRODUCTS`, `LIST_CALIBRATORS`, `PRICE` y
  los informes tabulares derraman todas sus filas/columnas.
- Celdas ocupadas debajo: aparece `#SPILL!` de forma explícita, sin truncar a la
  primera celda.
- Recálculo/reload: la nueva firma se mantiene después de cerrar y abrir Excel.
- Memoria: después de cada llamada, `xlAutoFree12()` libera el `XLOPER12`, el
  array y las cadenas sin fugas ni doble liberación.

## Criterio de aceptación

El fix se considera terminado cuando el arnés demuestra que el bridge entrega
una matriz rectangular con todos los elementos, el arnés valida el `type_text`
registrado y una prueba manual en Excel moderno muestra el spill completo para
las listas y para al menos una función tabular (`ENGINE.PRICE`). Excel antiguo,
las fórmulas CSE y las APIs escalares de compatibilidad quedan fuera del alcance.
