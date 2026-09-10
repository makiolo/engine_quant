# Cliente Excel (XLL)

Cliente Excel del motor XVA (PLAN.md, Fase 4, §7.8; API rediseñada en §7.15): expone el mismo
registry C++ (`Registries`/`register_builtins`/`Registry<T>::create`/`engine::calc`, PLAN.md
§5.4/§7.15) que ya consumen `cpp/engine/tests` (Fase 2) y `clients/python` (Fase 3), con el
mismo modelo mental (PLAN.md §4: "la API debe sentirse equivalente en Python y en Excel").

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
| `ENGINE.CREATE_MARKET(params)` | `MarketSnapshot(...)` |
| `ENGINE.CREATE_CONTEXT(params)` | `PricingContext({...})` |
| `ENGINE.CREATE_EXECUTION(params)` | `ExecutionContext({...})` |
| `ENGINE.CALC(trade, medidas, modelo, mercado, contexto, ejecucion)` | `Engine().calc(trade, medidas, modelo, market, pricing, execution)` |
| `ENGINE.LIST_CALIBRATORS()` | `Engine().list_calibrators()` |
| `ENGINE.CREATE_CALIBRATOR(nombre)` | `Engine().create_calibrator(nombre)` |
| `ENGINE.CALIBRATE(calibrador, mercado, estimacion_inicial)` | `Calibrator.calibrate(market, initial_guess)` |

`params` es un rango de Excel de 2 o más columnas: columna A = nombre del parámetro,
columnas siguientes = su valor. Un parámetro escalar (`a`, `notional`, `backend`, ...) solo
rellena la columna B; un parámetro vector (`payment_times`, `accruals`, `pillars`,
`zero_rates`) rellena tantas columnas como valores tenga, dejando el resto de la fila en
blanco. Una fila con exactamente un valor numérico se interpreta como escalar, no como vector
de un elemento — un parámetro vector necesita al menos 2 celdas de valor para no ser
ambiguo (p. ej. un `ENGINE.CREATE_MARKET` con un único pillar no funciona; usar 2 o más).

`CREATE_MODEL`/`CREATE_PRODUCT`/`CREATE_MARKET`/`CREATE_CONTEXT`/`CREATE_EXECUTION` no
devuelven el objeto en sí (una celda de Excel no puede contener un
`IModel`/`IProduct`/`MarketSnapshot`/... opaco, a diferencia de `engine.Model`/
`engine.MarketSnapshot`/... en Python): devuelven un **handle** (texto) memoizado por
parámetros — los mismos parámetros producen siempre el mismo handle, así que la fórmula es
determinista. Las instancias creadas viven hasta que se descarga el complemento
(`xlAutoClose`); no hace falta "liberarlas" explícitamente, pero tampoco se liberan hoja a
hoja (limitación conocida, ver "Alcance y limitaciones" más abajo).

## `ENGINE.CALC` (PLAN.md §7.15)

Sustituye por completo a las antiguas `ENGINE.CREATE_MEASURE`/`ENGINE.EVALUATE`: calcula un
**lote** de medidas nombradas de una sola vez sobre el mismo trade/modelo/mercado/contexto de
valoración/contexto de ejecución, en vez de una medida a la vez con un rango de parámetros
genérico que mezclaba sin nombre propio los parámetros Monte Carlo (`n_paths`/`n_steps`/
`seed`) con los de crédito (`hazard_rate`/`recovery_rate`).

```
Trade    =ENGINE.CREATE_PRODUCT("IRSwap", <rango IRS>)
Model    =ENGINE.CREATE_MODEL("HullWhite1F", <rango Hull-White>)
Market   =ENGINE.CREATE_MARKET(<rango mercado>)
Pricing  =ENGINE.CREATE_CONTEXT(<rango contexto de valoracion>)
Compute  =ENGINE.CREATE_EXECUTION(<rango contexto de ejecucion>)
         =ENGINE.CALC(Trade, {"PV";"DV01";"ExpectedExposure";"PFE95";"UnilateralCVA"}, Model, Market, Pricing, Compute)
```

Medidas disponibles hoy (`ENGINE.LIST_MEASURES()`): `PV`, `DV01` (ambas deterministas, no usan
Monte Carlo), `ExpectedExposure`, `PFE95` (comparten una sola simulación Monte Carlo por
detrás, `ENGINE.CALC` la calcula una vez aunque se pidan las dos) y `UnilateralCVA`. Las
fechas de monitorización de `ExpectedExposure`/`PFE95` se derivan automáticamente de las
propias fechas de reseteo del trade — ya no es un parámetro que haya que pasar a mano.

`ENGINE.CALC` devuelve una tabla en **formato largo**: columnas `[MeasureName, Time, Value]`
— las medidas escalares (`PV`, `DV01`, `UnilateralCVA`) dan 1 fila (`Time` en blanco), las de
perfil (`ExpectedExposure`, `PFE95`) dan una fila por fecha de monitorización. Un único
formato homogéneo para todo el lote, fácil de filtrar/dinamizar en Excel (Tabla dinámica sobre
`MeasureName`). En Excel moderno (arrays dinámicos) basta con escribir la fórmula en la celda
superior izquierda y dejar que "derrame" (spill); en versiones sin arrays dinámicos hay que
introducirla como fórmula matricial (Ctrl+Shift+Intro) sobre un rango del tamaño esperado.

`Market` (`ENGINE.CREATE_MARKET`): rango clave/valor con `pillars`/`zero_rates` (vectores
paralelos, mismo largo) y, opcionalmente, `hazard_rate`/`recovery_rate` (por defecto `0.0`,
solo los usa `UnilateralCVA`).

`PricingContext` (`ENGINE.CREATE_CONTEXT`): `pricing_date` (opcional, metadato — ver "Alcance y
limitaciones"), `n_paths`, `n_steps` y `seed` (todos requeridos).

`ExecutionContext` (`ENGINE.CREATE_EXECUTION`): `backend` (`"cpu"`/`"gpu"`/`"auto"` — `"auto"`
se resuelve una vez, en el momento de crear el contexto, a `"gpu"` si este build tiene
`GpuBackend` compilado, si no a `"cpu"`) y `precision` (opcional, por defecto `"fp64"`, único
valor soportado hoy).

## Backend de cómputo: `ExecutionContext` sustituye el estado global (PLAN.md §7.15)

Antes de esta fase (PLAN.md §7.12, ya retirado), el backend de cómputo era un **estado global
del proceso** (`ENGINE.SET_BACKEND`/`ENGINE.GET_BACKEND`) que leían todas las llamadas
siguientes a `ENGINE.EVALUATE` — con el problema de que Excel no recalculaba automáticamente
las celdas ya existentes al cambiar de backend (hacía falta Ctrl+Alt+Intro manual). Ahora el
backend es un campo más de `ExecutionContext`, un argumento explícito de `ENGINE.CALC`: al ser
un argumento normal de la fórmula, Excel sí recalcula automáticamente cuando cambia (por
ejemplo, si `Compute` es una celda con `="cpu"`/`"gpu"` referenciada desde el rango de
`ENGINE.CREATE_EXECUTION`).

`GpuBackend` (`burn-wgpu`) no está compilado en el `.xll` por defecto (PLAN.md §5.1, §7.11):
hace falta recompilar con `-DENGINE_QUANT_ENABLE_GPU=ON` (ver [`../../CMakeLists.txt`](../../CMakeLists.txt))
para que `backend="gpu"` tenga efecto — en un `.xll` sin esa opción, `ENGINE.CREATE_EXECUTION`
devuelve `#VALUE!` si se pide `"gpu"` explícitamente (`"auto"` cae a `"cpu"` en su lugar, sin
error).

**Los resultados no son bit a bit idénticos entre backends con la misma semilla**: cada
backend de Burn implementa su propio generador de números aleatorios, así que el mismo
`seed` produce una secuencia de shocks distinta en CPU (`burn-ndarray`) y GPU (`burn-wgpu`) —
el perfil de exposición Monte Carlo difiere en ruido estadístico (variación típica sub-2%
en el caso de §5.2), no en la lógica de valoración. No usar el mismo `seed` en ambos backends
como prueba de reproducibilidad exacta.

## Market y calibración (PLAN.md §7.14, §7.15)

A diferencia de antes de §7.15, `ENGINE.CALIBRATE` toma el mercado como **handle**
(`ENGINE.CREATE_MARKET`), igual que modelo/producto — ya no como rango inline:

```
=Market  = ENGINE.CREATE_MARKET(<rango mercado: pillars, zero_rates>)
=ENGINE.CREATE_CALIBRATOR("HullWhite1F")                          -> handle del calibrador
=ENGINE.CALIBRATE(<handle calibrador>, Market, <estimacion inicial>)
```

`estimacion_inicial` es un rango clave/valor normal (`a`, `b`, `sigma`, `r0`) — mismo formato
que `CREATE_MODEL`. Solo `a`/`b` se calibran (`sigma`/`r0` se devuelven tal cual se pasaron,
ver `rust/crates/engine-core/src/calibration.rs` para el porqué: `sigma` solo entra en el
precio del bono cero-cupón como un efecto de segundo orden, mal identificado contra
únicamente una curva de descuento — en la práctica se calibra con swaptions/caps, fuera de
alcance de esta fase). El resultado "derrama" una tabla clave/valor (`a`, `b`, `sigma`, `r0`,
`rmse`, `iterations`, `converged`) que **se puede pasar tal cual como el `params` de
`ENGINE.CREATE_MODEL`**: las claves de diagnóstico (`rmse`/`iterations`/`converged`) se
ignoran, cerrando el círculo Mercado → calibrar → Modelo calibrado en dos fórmulas:

```
=ENGINE.CALIBRATE(<calibrador>, <mercado>, <estimacion inicial>)   -> celda A1, "derrama" hacia abajo
=ENGINE.CREATE_MODEL("HullWhite1F", A1#)                            -> el mismo rango derramado, referenciado con #
```

## Verificación manual (PLAN.md §5.6, capa 4: equivalencia entre clientes)

CI (`windows-latest`, PLAN.md §7.4) no tiene Excel instalado, así que la capa 4 de test
("el mismo caso ejecutado desde Python y desde Excel debe producir el mismo resultado
numérico") no puede automatizarse ahí. `clients/excel/tests/test_xloper.cpp` cubre la mitad
que sí es automatizable (que el bridge invoca exactamente el mismo `engine::Registries`/
`engine::calc` que Python, con los mismos parámetros/semillas que
`test_calc.py`/`test_registry.cpp` — ver `HandleRegistry.ExpectedExposureAndPfe95MatchOtherClients`
y `HandleRegistry.UnilateralCvaIsPositiveForNonzeroHazardRate`); falta confirmar que **Excel
real**, cargando el `.xll`, reproduce esos mismos números a través de `xlAutoOpen`/las UDFs
exportadas. Pasos:

1. Compilar (`cmake --build build --target engine_excel_ext`).
2. En Excel: Archivo → Opciones → Complementos → Administrar "Complementos de Excel" → Ir... →
   Examinar... → seleccionar `build/clients/excel/engine_excel.xll` → Aceptar.
3. En una hoja, construir el rango de parámetros de Hull-White (columna A = clave, columna B
   = valor): `a=0.1`, `b=0.03`, `sigma=0.01`, `r0=0.02`; el de IRS a la par a 5 años:
   `notional=1000000`, `payment_times=1,2,3,4,5` (una celda por valor, misma fila),
   `accruals=1,1,1,1,1`; y el de mercado: `pillars=1,2` (2+ celdas, ver nota de más arriba),
   `zero_rates=0.02,0.02`, `hazard_rate=0.02`, `recovery_rate=0.4`.
4. `=ENGINE.CREATE_MODEL("HullWhite1F", <rango Hull-White>)`,
   `=ENGINE.CREATE_PRODUCT("IRSwap", <rango IRS>)`,
   `=ENGINE.CREATE_MARKET(<rango mercado>)` en tres celdas distintas.
5. Rango de contexto de valoración: `pricing_date=0`, `n_paths=5000`, `n_steps=208`, `seed=7`.
   Rango de contexto de ejecución: `backend=cpu`, `precision=fp64`.
   `=ENGINE.CREATE_CONTEXT(<rango de arriba>)`, `=ENGINE.CREATE_EXECUTION(<rango de arriba>)`.
6. `=ENGINE.CALC(<handle trade>, {"PV";"DV01";"ExpectedExposure";"PFE95";"UnilateralCVA"}, <handle modelo>, <handle mercado>, <handle contexto>, <handle ejecucion>)`
   — con esta semilla (`seed=7`) debe "derramar" (mismos valores que
   `Registry.UnilateralCvaMatchesGoldenValue`/`Registry.ExposureProfileMatchesGoldenValue` en
   `cpp/engine/tests/test_registry.cpp` y `test_calc.py`, calculados el 2026-09-10 con este
   mismo build):

   | MeasureName | Time | Value |
   | --- | --- | --- |
   | PV | | `0` (swap a la par) |
   | DV01 | | `378.467434451206` |
   | ExpectedExposure | 0 | `0` |
   | ExpectedExposure | 1 | `12862.617942080262` |
   | ExpectedExposure | 2 | `13673.529752568928` |
   | ExpectedExposure | 3 | `11957.816098484913` |
   | ExpectedExposure | 4 | `7124.106240024746` |
   | PFE95 | 0 | `0` |
   | PFE95 | 1 | `51009.920875152675` |
   | PFE95 | 2 | `53607.17081592724` |
   | PFE95 | 3 | `46152.44561612198` |
   | PFE95 | 4 | `27535.557267142714` |
   | UnilateralCVA | | `503.64194077997536` |

Si estos números coinciden, la capa 4 de §5.6 queda verificada de punta a punta (Python y
Excel, mismo motor C++/Rust por debajo). Actualizar esta sección si cambia el caso base de
§5.2, las semillas de referencia, o `n_steps`/la malla temporal de Monte Carlo.

## Alcance y limitaciones (decisiones deliberadas de esta fase)

- **Handles memoizados por parámetros, no por celda**: más simple y verificable sin Excel
  instalado que un mecanismo de "handle" ligado al ciclo de vida de la celda que lo creó
  (invalidación en recálculo, `xlfGetCaller`, etc.); el coste es que las instancias no se
  liberan hasta `xlAutoClose` (cerrar el libro/quitar el complemento), no hoja a hoja. Para
  el caso base de esta fase (IRS + Hull-White, PLAN.md §5.2) el número de combinaciones de
  parámetros distintas en una sesión de trabajo típica es pequeño; revisar si se vuelve un
  problema real de memoria en producción.
- **`pricing_date` es un metadato, sin aritmética de calendario** (PLAN.md §7.15): se guarda
  tal cual llega (p. ej. el serial de `DATE(2026,9,10)`), pero ninguna medida lo usa todavía
  para convertir fechas a fracciones de año o aplicar day-count — sigue siendo trabajo
  pendiente, igual que antes de esta fase.
- **`PV`/`DV01` no usan la curva de `Market` para descontar**: ambas se calculan únicamente a
  partir del modelo (Hull-White 1F), igual que `ExpectedExposure`/`PFE95`/`UnilateralCVA` ya
  hacían — `Market.pillars()`/`zero_rates()` solo alimentan calibración y (via
  `hazard_rate`/`recovery_rate`) `UnilateralCVA`. Descuento híbrido con la curva de mercado
  queda fuera de alcance de esta fase.
- **Errores siempre como `#VALUE!`**: cualquier excepción de C++ (parámetro faltante, tipo
  no registrado, medida desconocida en `ENGINE.CALC`, medida incompatible con el
  modelo/producto) se traduce al mismo código de error de Excel, sin distinguir el motivo en
  la celda (sí se puede diferenciar poniendo un breakpoint/depurando el XLL, pero no hay UDF
  de "última razón de error" en esta fase).
- **`xlAutoClose` no desregistra explícitamente las UDFs** (`xlfUnregister`/`xlfSetName`):
  solo libera los handles memoizados. Excel limpia el registro al descargar la DLL; no se ha
  observado que esto deje nombres huérfanos en sesiones normales de trabajo.
