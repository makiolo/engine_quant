# Cliente Excel (XLL)

Cliente Excel del motor XVA (PLAN.md, Fase 4, §7.8): expone el mismo registry C++
(`Registries`/`register_builtins`/`Registry<T>::create`/`IMeasure::evaluate`, PLAN.md §5.4)
que ya consumen `cpp/engine/tests` (Fase 2) y `clients/python` (Fase 3), con el mismo modelo
mental (PLAN.md §4: "la API debe sentirse equivalente en Python y en Excel").

## Compilar

Igual que el resto del árbol (`build-and-smoke-test` en CI, PLAN.md §7.4): CMake + Ninja +
MSVC en una "Developer Command Prompt" (con `cl.exe` en el `PATH`).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target engine_excel_ext
```

El resultado es `build/clients/excel/engine_excel.xll`. Solo se construye en Windows (guard
`WIN32` en el `CMakeLists.txt` raíz): un XLL es un artefacto específico de esa plataforma.

## API (equivalente a `clients/python`)

| Excel | Python (`clients/python`, Fase 3) |
| --- | --- |
| `ENGINE.LIST_MODELS()` | `Engine().list_models()` |
| `ENGINE.LIST_PRODUCTS()` | `Engine().list_products()` |
| `ENGINE.LIST_MEASURES()` | `Engine().list_measures()` |
| `ENGINE.CREATE_MODEL(nombre, params)` | `Engine().create_model(nombre, params)` |
| `ENGINE.CREATE_PRODUCT(nombre, params)` | `Engine().create_product(nombre, params)` |
| `ENGINE.CREATE_MEASURE(nombre)` | `Engine().create_measure(nombre)` |
| `ENGINE.EVALUATE(medida, modelo, producto, params)` | `Measure.evaluate(modelo, producto, params)` |

`params` es un rango de Excel de 2 o más columnas: columna A = nombre del parámetro,
columnas siguientes = su valor. Un parámetro escalar (`a`, `notional`, ...) solo rellena la
columna B; un parámetro vector (`payment_times`, `accruals`, `monitoring_times`) rellena
tantas columnas como valores tenga, dejando el resto de la fila en blanco.

`CREATE_MODEL`/`CREATE_PRODUCT`/`CREATE_MEASURE` no devuelven el objeto en sí (una celda de
Excel no puede contener un `IModel`/`IProduct`/`IMeasure` opaco, a diferencia de
`engine.Model`/`engine.Product`/`engine.Measure` en Python): devuelven un **handle** (texto)
memoizado por nombre + parámetros — los mismos parámetros producen siempre el mismo handle,
así que la fórmula es determinista. Las instancias creadas viven hasta que se descarga el
complemento (`xlAutoClose`); no hace falta "liberarlas" explícitamente, pero tampoco se
liberan hoja a hoja (limitación conocida, ver "Alcance y limitaciones" más abajo).

`EVALUATE` devuelve un único número si la medida tiene un escalar (`UnilateralCVA`), o una
matriz de 3 columnas (tiempo, EE, PFE 95%) si no lo tiene (`ExposureProfile`) — en Excel
moderno (arrays dinámicos) basta con escribir la fórmula en la celda superior izquierda y
dejar que "derrame" (spill); en versiones sin arrays dinámicos hay que introducirla como
fórmula matricial (Ctrl+Shift+Intro) sobre un rango del tamaño esperado.

## Verificación manual (PLAN.md §5.6, capa 4: equivalencia entre clientes)

CI (`windows-latest`, PLAN.md §7.4) no tiene Excel instalado, así que la capa 4 de test
("el mismo caso ejecutado desde Python y desde Excel debe producir el mismo resultado
numérico") no puede automatizarse ahí. `clients/excel/tests/test_xloper.cpp` cubre la mitad
que sí es automatizable (que el bridge invoca exactamente el mismo `engine::Registries`/
`IMeasure::evaluate` que Python, con los mismos parámetros/semillas que
`test_registry.py`/`test_registry.cpp` — ver `HandleRegistry.ExposureProfileMatchesOtherClients`
y `HandleRegistry.UnilateralCvaIsPositiveForNonzeroHazardRate`); falta confirmar que **Excel
real**, cargando el `.xll`, reproduce esos mismos números a través de `xlAutoOpen`/las UDFs
exportadas. Pasos:

1. Compilar (`cmake --build build --target engine_excel_ext`).
2. En Excel: Archivo → Opciones → Complementos → Administrar "Complementos de Excel" → Ir... →
   Examinar... → seleccionar `build/clients/excel/engine_excel.xll` → Aceptar.
3. En una hoja, construir el rango de parámetros de Hull-White (columna A = clave, columna B
   = valor): `a=0.1`, `b=0.03`, `sigma=0.01`, `r0=0.02`; y el de IRS a la par a 5 años:
   `notional=1000000`, `payment_times=1,2,3,4,5` (una celda por valor, misma fila),
   `accruals=1,1,1,1,1`.
4. `=ENGINE.CREATE_MODEL("HullWhite1F", <rango Hull-White>)`,
   `=ENGINE.CREATE_PRODUCT("IRSwap", <rango IRS>)`,
   `=ENGINE.CREATE_MEASURE("UnilateralCVA")` en tres celdas distintas.
5. Rango de parámetros de la medida: `monitoring_times=0,1,2,3`, `n_paths=5000`, `seed=13`,
   `hazard_rate=0.02`, `recovery_rate=0.4`.
6. `=ENGINE.EVALUATE(<handle medida>, <handle modelo>, <handle producto>, <rango de arriba>)`
   — con esta semilla (`seed=13`) debe dar **`426.7618244093184`** (mismo valor que
   `test_unilateral_cva_is_positive_for_nonzero_hazard_rate` en `test_registry.py`, calculado
   el 2026-09-10 con este mismo build vía Python: `CVA = 426.76182440931836`).
7. Repetir con `ExposureProfile` (medida) y `monitoring_times=0,1,2`, `n_paths=5000`,
   `seed=7`: debe "derramar" una matriz 3x3 con `times=[0, 1, 2]`,
   `EE≈[0, 12862.62, 13673.53]`, `PFE95≈[0, 51009.92, 53607.17]` (mismos valores que
   `test_exposure_profile_is_non_negative_and_pfe_dominates_ee`).

Si estos números coinciden, la capa 4 de §5.6 queda verificada de punta a punta (Python y
Excel, mismo motor C++/Rust por debajo). Actualizar esta sección si cambia el caso base de
§5.2 o las semillas de referencia.

## Alcance y limitaciones (decisiones deliberadas de esta fase)

- **Handles memoizados por parámetros, no por celda**: más simple y verificable sin Excel
  instalado que un mecanismo de "handle" ligado al ciclo de vida de la celda que lo creó
  (invalidación en recálculo, `xlfGetCaller`, etc.); el coste es que las instancias no se
  liberan hasta `xlAutoClose` (cerrar el libro/quitar el complemento), no hoja a hoja. Para
  el caso base de esta fase (IRS + Hull-White, PLAN.md §5.2) el número de combinaciones de
  parámetros distintas en una sesión de trabajo típica es pequeño; revisar si se vuelve un
  problema real de memoria en producción.
- **Errores siempre como `#VALUE!`**: cualquier excepción de C++ (parámetro faltante, tipo
  no registrado, medida incompatible con el modelo/producto) se traduce al mismo código de
  error de Excel, sin distinguir el motivo en la celda (sí se puede diferenciar poniendo un
  breakpoint/depurando el XLL, pero no hay UDF de "última razón de error" en esta fase).
- **`xlAutoClose` no desregistra explícitamente las UDFs** (`xlfUnregister`/`xlfSetName`):
  solo libera los handles memoizados. Excel limpia el registro al descargar la DLL; no se ha
  observado que esto deje nombres huérfanos en sesiones normales de trabajo.
