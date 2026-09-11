# PLAN_REAPI.md — Análisis de propuestas de rediseño de API pública

> Documento de análisis, no de ejecución. Objetivo: decidir, propuesta por propuesta, si se
> implementa, se implementa parcialmente, o se descarta — antes de tocar código. Referencia
> cruzada con PLAN.md (arquitectura/fases actuales) en vez de duplicarlo. Estado del motor en el
> momento de este análisis: v0.18, Fase 7.20 cerrada (`Curve` por composición dentro de
> `MarketSnapshot`).

## 0. Origen y encuadre

El usuario ha recibido cuatro observaciones sobre el diseño de la API pública (estilo
`q.create_product(...)` / `q.IRSwap.par(...)`, con un diagrama de capas Public API / C++ Core).
No son código de este repo — son una propuesta externa de "cómo debería verse" una API de este
tipo. Este documento contrasta cada observación contra el diseño **ya existente** en
`cpp/engine/include/engine/` y expone qué encaja, qué ya está resuelto de otra forma, y qué
falta decidir.

## 1. Las cuatro propuestas (normalizadas)

1. **Dos fachadas, un solo DTO interno.** `q.create_product("IRSwap", {...})` (dict dinámico)
   debería sobrevivir, pero como *una fachada más* (Excel, JSON, REST/gRPC, scripting, plugins)
   sobre un `TradeSpec` interno único — no como la única forma de construir un trade. Una API
   tipada (`q.IRSwap(notional=..., day_count="ACT/360", ...)`) debería producir exactamente el
   mismo `TradeSpec`.
2. **`fixed_rate=None` no debería significar "swap a la par".** `None`/ausencia de clave debe
   significar "falta el dato", nunca "constrúyelo a mercado". Un swap par es una decisión de
   construcción explícita (`rate=q.PAR` o `q.IRSwap.par(...)`), no la consecuencia de omitir un
   parámetro.
3. **Measures tipadas, no strings.** En vez de `["PV", "DV01", "ExpectedExposure", ...]`, cada
   measure sería un objeto con su propia configuración: `q.PV()`, `q.DV01(curve="EUR.OIS",
   bucketed=True)`, `q.ExpectedExposure(...)`.
4. **Separación de capas Product/Market/Model/Measure/Execution**, con un núcleo C++ (grafo de
   instrumentos, grafo de mercado, registry de modelos, calibración, motores de pricing,
   orquestación XVA, grafo de dependencias, caché) capaz de escalar de `eng.evaluate(swap,
   q.PV())` a cartera completa (500k trades → netting sets → Monte Carlo → colateral →
   CVA/DVA/FVA/MVA → sensibilidades → GPU) sin rediseñar la API otra vez.

## 2. Estado actual — qué de esto ya existe

Antes de valorar coste, hace falta saber qué parte del problema ya está resuelta:

- **`MarketSnapshot` ya implementa el patrón de la propuesta 1**: tiene un constructor tipado
  (`MarketSnapshot(pillars, zero_rates, hazard_rate, recovery_rate)`,
  `cpp/engine/include/engine/market.hpp:77-80`) **y** un constructor dinámico desde `Params`
  (`explicit MarketSnapshot(const Params&)`, línea 86) — ambos construyen el mismo objeto. Es el
  precedente directo de lo que la propuesta 1 pide para `TradeSpec`.
- **`IrSwapProduct`/`HullWhite1FModel`/`HullWhite2FModel` NO tienen ese segundo constructor
  tipado** — solo `explicit Foo(const Params&)`
  (`cpp/engine/include/engine/product.hpp:25`, `cpp/engine/include/engine/model.hpp:20,42`). Hoy
  la única vía de construcción, en cualquier lenguaje, es el bag `Params` (dict en Python/Excel).
- **El `Params` bag ya es el "TradeSpec" de facto**: `std::unordered_map<std::string,
  ParamValue>` con `ParamValue = std::variant<double, std::vector<double>, bool>`
  (`params.hpp`, ver PLAN.md §7.14 líneas 514-519). El registry (`Registry<Interface>::create`,
  `registry.hpp:43`) es agnóstico al tipo concreto — construir un `TradeSpec` tipado por encima
  no requiere tocar el registry, solo añadir una capa de traducción Spec→Params.
- **`fixed_rate=None` → par ya es el comportamiento documentado y probado**: `IrSwapProduct`
  trata `"fixed_rate"` como opcional — si falta, `use_par_rate() == true`
  (`product.hpp:20-22`, `PLAN.md:527`). Está cubierto por tests y ejemplos en las cinco capas
  (`cpp/engine/tests/test_registry.cpp:61-63`, `clients/python/tests/test_calc.py:200`,
  `examples/abi/**`) — cualquier cambio de semántica aquí es un cambio de contrato público, no
  un añadido aislado.
- **Measures ya se registran vía `Params` internamente** (`IMeasure` concreto lleva
  `Concrete(const Params&)`, PLAN.md:523-526), pero la superficie pública de `calc`/`calc_batch`/
  `calc_many`/`calc_grid` solo acepta `measure_names: std::vector<std::string>`
  (`cpp/engine/include/engine/calc.hpp:37-104`) — no hay forma hoy de pasar configuración por
  measure (p.ej. `bucketed=True`). El registry ya soportaría el `Params` por measure; falta
  únicamente exponerlo en la firma pública.
- **La separación de capas de la propuesta 4 ya es, en gran medida, la arquitectura descrita en
  PLAN.md §2-§4**: `Trade`/`Model`/`Market`/`PricingContext`/`ExecutionContext` como conceptos
  separados desde la Fase 7.15, backend CPU/GPU como configuración (no como decisión de
  producto/modelo, principio ya fijado en §4). Lo que **no** existe todavía: netting sets,
  colateral, FVA/MVA/KVA, orquestación de cartera, grafo de dependencias/caché explícito — brechas
  ya anotadas al cierre de cada fase (PLAN.md líneas 1968-1972, 1985-1990, 1999-2009).

## 3. Análisis por propuesta

### 3.1 Propuesta 1 — Fachada dinámica + fachada tipada sobre un `TradeSpec` único

- **Encaje**: alto. Es exactamente el patrón que `MarketSnapshot` ya demuestra que funciona en
  este código. Aditivo: no hay que tocar el registry ni las fachadas dinámicas existentes
  (Excel/Python dict/C ABI ya usan `Params`), solo añadir una fábrica tipada por encima que
  traduzca a `Params` antes de llegar al registry.
- **Coste**: medio. Por cada producto/modelo con builder tipado hay que decidir la firma tipada
  en cada lenguaje (Python: kwargs; Excel: no tiene un análogo natural a kwargs tipados más allá
  de UDFs con argumentos nombrados fijos, así que en Excel seguiría siendo el dict/rango de
  celdas; C ABI: seguiría siendo `Params`/`EngineParam*` porque C no tiene builders). Es decir,
  la fachada tipada tiene sentido sobre todo en Python (y quizá Rust si se expone directamente);
  en Excel/C ABI/JSON crudo el "dict tipado" ya es lo más natural que existe.
- **Riesgo**: bajo. No rompe nada existente si se implementa como capa añadida.
- **Recomendación**: **implementar, ámbito acotado a Python** (y opcionalmente Rust). No tiene
  sentido forzarlo en Excel/C ABI, donde el propio medio ya es "dinámico" por naturaleza — ahí la
  propuesta 1 ya está satisfecha por diseño.

### 3.2 Propuesta 2 — Eliminar `fixed_rate=None` como sentinel de "par"

- **Encaje**: el diagnóstico es correcto y coincide con una limitación ya conocida del diseño
  actual (ausencia de clave sobrecargada con significado de negocio). No es solo un problema de
  estilo: es la única vía hoy para marcar un swap "a la par" en las cinco capas.
- **Coste**: medio-alto, porque **es un cambio de contrato**, no una extensión. Tests y ejemplos
  actuales (`test_registry.cpp`, `test_calc.py::test_par_irs*`, `abi_c_smoke.c`) codifican
  "ausencia de `fixed_rate`" como la forma válida de pedir un swap par. Quitarlo sin sustituto
  rompe esos casos.
- **Riesgo**: si se hace mal (p.ej. cambiar semántica de golpe sin período de coexistencia),
  rompe consumidores existentes de la API dinámica (Excel/JSON/C ABI) que hoy construyen swaps
  par omitiendo la clave.
- **Alternativas concretas a decidir** (no hay una única forma "correcta"):
  - (a) Sentinel string explícito en la superficie dinámica: `"fixed_rate": "PAR"` en vez de
    omitir la clave — `ParamValue` tendría que aceptar ese caso especial o `Params` necesitaría
    un tipo sentinel nuevo.
  - (b) Clave explícita separada: mantener `fixed_rate` como estrictamente requerido si está
    presente, y usar `"rate_mode": "par"` para pedir construcción a mercado — más verboso pero
    sin ambigüedad de tipos.
  - (c) Solo en la fachada tipada de la propuesta 3.1: `IRSwap.par(...)` como constructor
    nombrado en Python; en la fachada dinámica (dict/Excel/C ABI), mantener el comportamiento
    actual documentado tal cual (aceptar que ahí "ausencia = par" seguirá siendo así porque no
    hay forma más limpia sin romper compatibilidad) — es decir, resolver el problema donde es
    barato resolverlo (Python tipado) y documentar la limitación donde es caro (dict genérico).
- **Recomendación**: **implementar la opción (c) primero** (bajo riesgo, resuelve el caso que
  más preocupa — el uso accidental) y valorar (a)/(b) para la fachada dinámica en una fase
  posterior, con una ventana de deprecación explícita si se decide cambiar el comportamiento por
  omisión ahí también.

### 3.3 Propuesta 3 — Measures tipadas con configuración

- **Encaje**: alto, y de hecho es la propuesta con **menos fricción arquitectónica** de las
  cuatro: el registry (`Registry<IMeasure>`) ya construye measures desde `Params`
  (PLAN.md:523-526); solo falta que la firma pública de `calc`/`calc_batch`/`calc_many`/
  `calc_grid` deje de asumir "measure = nombre puro" y acepte "measure = nombre + Params
  opcional".
- **Coste**: medio. Toca la firma en cuatro puntos de `calc.hpp` y sus cinco capas (Rust no,
  C++/C ABI/Python/Excel sí), pero es un cambio mecánico: `std::vector<std::string>` →
  `std::vector<MeasureSpec>` donde `MeasureSpec = {name, Params}` con `Params{}` por defecto
  (retrocompatible: pasar solo el nombre sigue funcionando).
- **Riesgo**: bajo si se mantiene la sobrecarga con solo nombres para no romper `test_calc.py`/
  `abi_c_smoke.c`/ejemplos existentes.
- **Nota de alcance**: hoy ninguna measure concreta (`PresentValueMeasure`, `Dv01Measure`, etc.)
  usa configuración propia (`curve=`, `bucketed=`) — esa funcionalidad (DV01 bucketed por pilar
  de curva, por ejemplo) no existe en el core Rust/C++ actual. Adoptar measures tipadas ahora es
  correcto para el **contrato de la API**, pero no aporta valor real hasta que existan measures
  con parámetros de verdad que consumir — de lo contrario es tipar por tipar.
- **Recomendación**: **implementar el contrato** (`MeasureSpec` con `Params` opcional) en la
  próxima fase que ya toque `calc.hpp` por otro motivo (p.ej. al añadir una measure nueva), en
  vez de como cambio aislado sin consumidor real todavía.

### 3.4 Propuesta 4 — Arquitectura de capas + escalado a cartera

- **Encaje**: esto no es una propuesta de API puntual, es una descripción de arquitectura de
  destino a largo plazo que **ya coincide, en sus líneas generales**, con PLAN.md §2-§4
  (capas Rust/C++/clientes, backend como configuración, registry extensible). La separación
  Product/Market/Model/Measure/Execution que pide ya es real en el código
  (`IProduct`/`IModel`/`MarketSnapshot`/`IMeasure`/`ExecutionContext`).
- **Lo que falta y no está en el roadmap actual**: netting sets, colateral, FVA/MVA/KVA, un
  "grafo de dependencias"/caché explícito entre instrumentos y mercados, orquestación de cartera
  a escala de cientos de miles de trades. Nada de esto es gratis añadir después — son
  decisiones de diseño de fondo (p.ej. cómo se modela un netting set frente a un `IProduct`
  individual) que si se ignoran ahora sí podrían forzar un rediseño más adelante, que es
  precisamente el riesgo que la propuesta señala.
- **Riesgo de no analizarlo**: medio. Las decisiones 1-3 (TradeSpec, sentinel de par, measures
  tipadas) son locales y no comprometen la capacidad de escalar a cartera. El riesgo real de
  "cerrar puertas" está en decisiones que este documento no cubre (representación de netting
  set, modelo de colateral) y que no están sobre la mesa todavía.
- **Recomendación**: **no tratar como propuesta a implementar**, sino como **checklist de
  validación** al decidir 3.1-3.3: verificar que el `TradeSpec`/`MeasureSpec` que se diseñe no
  asuma "un trade = una llamada aislada" de una forma que estorbe agrupar trades en netting sets
  más adelante (p.ej. que `TradeSpec` no cargue nada que solo tenga sentido a nivel de trade
  individual y no de cartera). Revisar explícitamente en la Fase de implementación de 3.1.

## 4. Resumen de recomendaciones

| # | Propuesta | Recomendación | Coste | Riesgo | Rompe compatibilidad |
|---|---|---|---|---|---|
| 1 | Fachada tipada + `TradeSpec` único | Implementar (ámbito: Python) | Medio | Bajo | No |
| 2 | Quitar `None`=par | Implementar solo vía builder tipado (`IRSwap.par`); dejar el dict dinámico como está | Medio-alto si se toca el dict | Alto si se toca el dict sin deprecación | Sí, si se toca el dict |
| 3 | Measures tipadas | Implementar el contrato (`MeasureSpec`), diferir el valor real a cuando haya measures parametrizadas | Medio | Bajo | No, con sobrecarga por nombre |
| 4 | Capas + escalado a cartera | No implementar como tal; usar como checklist al hacer 1-3 | — | — | — |

> Esta tabla es el análisis inicial (antes de decidir alcance con el usuario). Las decisiones
> finales, más amplias en 2-3, están en §5 y el plan concreto en §6.

## 5. Preguntas abiertas — todas resueltas

- [x] ¿`dataclasses` o `pydantic`? **`pydantic`, dependencia obligatoria** (no extra opcional).
  Renuncia deliberada a la promesa "sin dependencias Python en tiempo de ejecución"
  (`clients/python/README_PYPI.md:29`) — hay que actualizarla.
- [x] ¿Alcance de "tiparlo todo"? **Todo**: `TradeSpec`/`IRSwap` (propuestas 1+2), `Model`
  (`HullWhite1F`/`HullWhite2F`), `Market`, `PricingContext`, `ExecutionContext`, y `Measure`
  tipadas (propuesta 3) **incluyendo** el cambio de contrato en `calc.hpp` — no solo el producto.
- [x] Propuesta 2 en la fachada dinámica (dict/Excel/C ABI): **se deja como está**. Solo la
  fachada tipada Python resuelve `fixed_rate=None`→PAR; el dict dinámico sigue documentando esa
  limitación conocida. Cero cambios en `IrSwapProduct`/`Params`.
- [x] `MeasureSpec` en `calc.hpp`: **se implementa ahora** y **abre `calc()` a cualquier nombre
  del `Registry<IMeasure>`** (no se mantiene la tabla curada de 5 nombres de
  `calc_measure_name_mappings()`) — cambio de diseño respecto a la decisión original de la Fase
  7.15 (vocabulario de CALC deliberadamente desacoplado del registry), revertido aquí a petición
  expresa.
- [x] Measures sin semántica real (`Dv01Measure`/etc. ignoran `Params` hoy, `measure.hpp:53,69,
  84,100`): en vez de plumbing vacío o validación defensiva, **se implementa semántica real**:
  `DV01(bump=...)` configurable (sin prerequisitos, sin tocar Rust) **y** además se amplía el
  alcance para cerrar el prerequisito de `bucketed DV01` de verdad.
- [x] Prerequisito de `bucketed DV01` — hoy `PresentValueMeasure`/`Dv01Measure` no consumen la
  curva de `MarketSnapshot` en absoluto (`market` llega sin nombre = ignorado, `measure.cpp:267
  -269`; NPV se calcula 100% vía `model.zero_coupon_bond` en Rust, `irs.rs:46-70`). **Se cierra
  el prerequisito**: PV/DV01 pasan a descontar por la curva de mercado observada.
- [x] Ese cambio de descuento — ¿modo nuevo opcional o sustituye el comportamiento actual?
  **Sustituye**: `PV`/`DV01` dejan de usar `model.zero_coupon_bond` y pasan a usar
  `MarketSnapshot::discount_factor(t)`. Es un cambio de contrato numérico — ver riesgos en Fase 4.
- [x] Empaquetado: **mismo wheel `engine-quant`**, nuevo subpaquete `engine_typed` (paquete
  Python puro y aditivo — ver hallazgo de packaging en Fase 1 más abajo).
- [x] Integración en `PLAN.md`: **no ahora**. `PLAN_REAPI.md` sigue siendo el documento de
  trabajo; se migra a `PLAN.md` como fase(s) `§7.21+` solo cuando algo quede implementado y
  verificado end-to-end, mismo criterio que ya aplica el propio `PLAN.md` a cada fase cerrada.

## 6. Plan de implementación completo

Seis fases, cada una verificable de forma independiente antes de empezar la siguiente (mismo
criterio que exige `PLAN.md` para cerrar una fase: build limpio + tests + ejemplo end-to-end).
**Fases 1-3 son plumbing de API, bajo riesgo, aditivas.** **Fases 4-5 cambian semántica
numérica ya en producción (PV/DV01) — riesgo real, requieren revisión cuantitativa, no son "solo
API".** Se recomienda un punto de control explícito con el usuario entre la Fase 3 y la Fase 4
antes de tocar ninguna fórmula de valoración.

### Fase 1 — `engine_typed`: `TradeSpec`/`IRSwap` + sentinel PAR

**Hallazgo de packaging que condiciona todo el paquete nuevo**: el módulo compilado nanobind se
llama **literalmente `engine`** a nivel de C++ (`NB_MODULE(engine, m)`,
`clients/python/src/engine_py_ext.cpp:169`; `OUTPUT_NAME "engine"`,
`clients/python/CMakeLists.txt:14`) y hoy es el único artefacto instalado
(`wheel.packages = []`, `pyproject.toml`). Poner la fachada tipada *dentro* de un paquete Python
puro llamado `engine` exigiría renombrar el módulo compilado y reescribir el layout del wheel —
invasivo, sin aportar nada a las propuestas 1-3. En su lugar: **paquete Python puro separado y
aditivo, `engine_typed`**, que depende de `engine` (la extensión existente, sin tocarla) y de
`pydantic`. Uso: `import engine, engine_typed as q`. Cambios: paquete nuevo bajo
`clients/python/src/engine_typed/`, alta en `wheel.packages` de `pyproject.toml`,
`pydantic>=2` en `[project.dependencies]`, actualizar `README_PYPI.md` (ya no "sin
dependencias").

`TradeSpec`/`IRSwap` con `fixed_rate` **requerido** (`float | Literal["PAR"]`, sin default) —
omitirlo es `ValidationError`, no PAR; `IRSwap.par(...)` como constructor con nombre. Diseño
completo (código) ya esbozado más abajo en "Apéndice: `TradeSpec`". `to_params()` alimenta
`eng.create_product(...)` sin tocar el core — mismo patrón que `MarketSnapshot` (§2).

**`day_count`**: la propuesta original lo incluía (`day_count="ACT/360"`). No existe en el core
hoy (ni `IrSwapProduct` ni las funciones Rust de NPV lo reciben — pendiente ya anotado en
`PLAN.md:1973`). Queda fuera de `IRSwap` hasta que el core lo soporte, para no prometer un campo
sin efecto.

**Verificación**: tests de construcción/rechazo/`.par()`/`to_params()`; ejemplo equivalente a
`calc_flow.py` usando `engine_typed` contra el `Engine` real. Riesgo: bajo. No toca C++/Rust.

### Fase 2 — `engine_typed`: `Model`/`Market`/`PricingContext`/`ExecutionContext` tipados

Mismo patrón `BaseModel` + `.to_params()`/`.product_type` (o `.model_type`) que `IRSwap`:

- `HullWhite1F(a, b, sigma, r0)`, `HullWhite2F(a, b, sigma, eta, rho, r0)` — todos los campos
  requeridos (hoy no tienen default en C++ tampoco: `get_double` sin default en
  `HullWhite1FModel`/`HullWhite2FModel`, ver `model.hpp`).
- `Market(pillars, zero_rates, hazard_rate=0.0, recovery_rate=0.0)` — Python puro que replica
  la validación que ya hace `MarketSnapshot`/`Curve` en C++ (pillars estrictamente creciente,
  mismo tamaño que zero_rates); redundante con el constructor tipado que `MarketSnapshot` ya
  tiene en C++, pero da JSON schema/validación pydantic consistente con el resto de `engine_typed`.
- `PricingContext(pricing_date=0.0, n_paths, n_steps, seed)`, `ExecutionContext(backend="auto",
  precision="FP64")` — hoy solo existen como dict en Python (`engine.PricingContext({...})`);
  pasan a tener también la fachada tipada.

**Verificación**: mismo patrón que Fase 1, ejemplo `calc_flow.py` completo usando solo
`engine_typed` (cero dicts crudos). Riesgo: bajo. No toca C++/Rust.

### Fase 3 — `MeasureSpec` en `calc.hpp` (C++/C ABI/Python/Excel) + `DV01(bump=...)`

Cambia `measure_names: std::vector<std::string>` a `measures: std::vector<MeasureSpec>` donde
`MeasureSpec = {std::string name; Params params;}`, en las cuatro funciones de `calc.hpp`
(`calc`/`calc_batch`/`calc_many`/`calc_grid`) — **retrocompatible**: se mantiene una sobrecarga
que acepta `vector<string>` construyendo `MeasureSpec{name, {}}` internamente, así
`test_calc.py`/`abi_c_smoke.c`/ejemplos existentes con listas de nombres puros no se rompen.

**Abre `calc()` a cualquier nombre de `Registry<IMeasure>`** (decisión tomada): se retira
`calc_measure_name_mappings()` como tabla curada cerrada — `calc()` resuelve directamente vía
`registries.measures.create(name, params)` (ya soporta `Params`, `registry.hpp:33`) en vez de
traducir por una tabla fija de 5 entradas. Efecto secundario a decidir en la implementación: el
mapeo `ExpectedExposure`/`PFE95` → `ExposureProfileMeasure.primary`/`.secondary` (hoy vive en
`extract_field`, `calc.cpp:35-53`) deja de tener un nombre-alias curado — o se registran
`ExpectedExposure`/`PFE95` como nombres directos en el registry (dos entradas que delegan en
`ExposureProfileMeasure` pero exponen campos distintos), o se pierde ese alias y pasan a
llamarse por su `type_name()` real (`"ExposureProfile"`, con ambos campos en el resultado). Se
decide en la implementación, documentando cuál se elige.

**`DV01(bump=...)`** — primera measure con semántica real de `Params`: `Dv01Measure::evaluate`
lee `get_double(params, "bump", 0.0001)` en vez del `0.0001` hardcodeado
(`measure.cpp:293`). Cero cambios en Rust (`irs_hull_white_npv_delta_r0` ya devuelve la
derivada cruda; solo el multiplicador vivía en C++).

En `engine_typed`: `Measure` base + `PV()`, `DV01(bump=0.0001)`, `ExposureProfile()` (o el
nombre que se decida arriba), `UnilateralCVA()` — `.to_params()` ya con efecto real para `DV01`.

**Verificación**: tests C++ (`test_registry.cpp`/nuevo test de `calc` con nombre arbitrario del
registry), Python (`DV01(bump=0.0002)` da un resultado distinto y verificable de
`DV01(bump=0.0001)`), Excel, C ABI. Riesgo: medio — toca 4 de las 5 capas (no Rust) y cambia un
principio de diseño ya fijado en la Fase 7.15 (vocabulario curado); documentar el porqué del
cambio si esto migra a `PLAN.md`.

---

**Punto de control recomendado aquí.** Las fases 1-3 son aditivas y de bajo riesgo. Las fases 4-5
cambian la fórmula de valoración de PV/DV01 que ya usan tests/ejemplos/clientes existentes en
las cinco capas — antes de tocar código ahí, confirmar que se sigue queriendo proceder tal como
se decidió (sustituir, no añadir un modo opcional).

### Fase 4 — Descuento por curva de mercado sustituye la semántica de PV/DV01

**Alcance real, acotado a C++, sin tocar Rust**: `MarketSnapshot::discount_factor(t)` ya existe
(`market.hpp:96`, interpolación lineal en zero rate + extrapolación plana fuera de rango,
`market.cpp:26-40`). La réplica de bonos cero-cupón que hoy hace `IrSwap::npv` en Rust
(`floating_leg = notional*(P(t,start)-P(t,end))`, `fixed_leg = Σ notional*fixed_rate*accrual_i*
P(t,Ti)`, `irs.rs:46-70`) es la misma fórmula que se puede recalcular en C++ usando
`market.discount_factor(t)` en vez de `model.zero_coupon_bond(...)` — **nueva función en
`measure.cpp`**, no una modificación de la de Rust (que sigue haciendo falta tal cual para las
rutas Monte Carlo, ver más abajo).

**Cambia el contrato**: `PresentValueMeasure`/`Dv01Measure` dejan de llamar a
`irs_hull_white_npv`/`irs_hull_white_npv_delta_r0` (Rust, dependientes del modelo) y pasan a
calcular PV directamente desde `IrSwapProduct` + `MarketSnapshot`, sin `IModel` en absoluto —
**PV/DV01 dejan de depender del modelo**. Esto es coherente (PV determinista de un swap vainilla
es, en efecto, solo función de la curva de descuento) pero es un cambio de comportamiento
observable: hoy `PresentValueMeasure::evaluate` ignora `market` (parámetro sin nombre,
`measure.cpp:267-269`) y usa el modelo; después, ignora el modelo y usa `market`.

**`ExposureProfile`/`UnilateralCVA` NO cambian** — necesitan revalorar el swap en fechas
*futuras* bajo estados simulados del tipo corto; no existe curva de mercado observable en el
futuro, por diseño tienen que seguir usando `model.zero_coupon_bond` vía Monte Carlo. Esta
asimetría (PV/DV01 por curva, exposición/CVA por modelo) es intencional y hay que documentarla
explícitamente para que no parezca una inconsistencia.

**DV01 pierde su definición actual** (`d(NPV)/d(r0)`, ya no aplica: NPV deja de depender de
`r0`). Pasa a ser una sensibilidad a la curva: bump paralelo de todos los `zero_rates` en
`bump` (reusa el parámetro de Fase 3), reprecio con una `MarketSnapshot` bumpeada, diferencia —
**bump-and-reval, no AAD**: no hace falta Rust para esto tampoco.

**Coste de migración de fixtures — más grande de lo que parece**: los mercados de test actuales
son de un solo pillar (`pillars=[1.0], zero_rates=[0.02]`, `test_calc.py:51`) mientras los swaps
de test pagan hasta 5y — con extrapolación plana eso "funciona" pero da un número distinto al
que da hoy el modelo (Hull-White no es una curva plana). Hace falta **enriquecer los mercados de
test a curvas multi-pillar que cubran el vencimiento del swap**, en las cinco capas
(`test_registry.cpp`, `test_calc.py`, `abi_c_smoke.c`, ejemplos Excel/Rust/Python/C/`ctypes`/
`cffi`), no solo recalcular valores dorados. Sanity check recomendado como test nuevo: un swap
par (`fixed_rate` = par rate calculado con la misma curva) debe dar `PV ≈ 0` bajo el nuevo
descuento — regresión barata y significativa.

**Verificación**: build limpio, `ctest`, `test_calc.py`, ejemplo end-to-end con curva
multi-pillar real en las cinco capas, sanity check PV par ≈ 0. Riesgo: **alto** — cambia una
fórmula de valoración ya usada por consumidores existentes; requiere revisión cuantitativa, no
solo de ingeniería de API.

### Fase 5 — Bucketed DV01

Construida sobre la Fase 4: `DV01(bucketed=True)` bumpea cada `zero_rates[i]` individualmente
(uno a la vez, `bump` de Fase 3 como tamaño), reprecia con `compute_npv` de la Fase 4, y
devuelve un vector de deltas (uno por pillar) en vez de un escalar — reusa la forma de
`MeasureResult` que ya usa `ExposureProfileMeasure` (`times`=pillars, `primary`=deltas por
pillar) en vez de inventar un tipo de resultado nuevo. El DV01 "parcial" (Fase 4, escalar) es la
suma de los deltas bucketed — se puede verificar como test de consistencia entre ambos.

**Verificación**: test C++ que compruebe `sum(bucketed deltas) ≈ DV01 escalar` (mismo `bump`,
misma curva); ejemplo Python con `q.DV01(bucketed=True)` mostrando la serie por pillar. Riesgo:
medio — depende enteramente de que la Fase 4 esté cerrada y verificada.

### Fase 6 — Consolidación

Una vez verificadas 1-5 end-to-end: migrar las fases relevantes a `PLAN.md` como `§7.21+`
(mismo formato que el resto del documento), actualizar `README_PYPI.md`/`clients/excel/README.md`
con los nuevos ejemplos tipados, y revisar contra la propuesta 4 (§3.4) que nada de lo anterior
cierra puertas a trabajo futuro de cartera/netting.

## Apéndice — `TradeSpec` (Fase 1, código de referencia)

Con `fixed_rate` como campo **requerido** (sin default, sin `Optional`), pydantic ya impide
construir un `IRSwap` sin él — "campo ausente" deja de significar nada especial, vuelve a ser
simplemente un error de validación. Un swap par se pide con un *sentinel* explícito
(`q.PAR`, `Literal["PAR"]`) o el constructor con nombre `IRSwap.par(...)`, tal como pedía la
propuesta 2 — sin tocar `IrSwapProduct`/`Params` en C++ (`to_params()` simplemente omite la
clave `"fixed_rate"` al pedir PAR, que es el comportamiento que el core **ya** entiende):

```python
from typing import ClassVar, Literal
from pydantic import BaseModel, ConfigDict

PAR: Literal["PAR"] = "PAR"

class TradeSpec(BaseModel):
    model_config = ConfigDict(frozen=True)  # inmutable: coincide con "construir = decisión", no mutación
    product_type: ClassVar[str]

    def to_params(self) -> dict: ...  # cada subclase lo implementa

class IRSwap(TradeSpec):
    product_type: ClassVar[str] = "IRSwap"

    notional: float
    fixed_rate: float | Literal["PAR"]     # requerido: omitirlo es ValidationError, no PAR
    start: float = 0.0
    payment_times: list[float]
    accruals: list[float]

    @classmethod
    def par(cls, *, notional: float, payment_times: list[float], accruals: list[float], start: float = 0.0) -> "IRSwap":
        return cls(notional=notional, fixed_rate=PAR, start=start, payment_times=payment_times, accruals=accruals)

    def to_params(self) -> dict:
        params = {"notional": self.notional, "start": self.start,
                  "payment_times": self.payment_times, "accruals": self.accruals}
        if self.fixed_rate != PAR:
            params["fixed_rate"] = self.fixed_rate
        return params
```

`eng.create_product(spec.product_type, spec.to_params())` alimenta el `Engine` existente sin
tocarlo — misma idea que ya demuestra `MarketSnapshot` (§2), solo que aquí la traducción vive en
Python puro en vez de en un segundo constructor C++.

**Nota — `day_count`**: la propuesta original incluía `day_count="ACT/360"` como campo de
`IRSwap`. **No existe hoy en el core**: ni `IrSwapProduct` (`product.hpp`) ni
`irs_hull_white_npv`/`irs_hull_white_exposure_profile` en Rust reciben día-cómputo — ya está
anotado como pendiente en PLAN.md (línea 1973, "day count/calendarios"). Incluir `day_count` en
`IRSwap` ahora sería un campo que no hace nada — se deja fuera del diseño hasta que el core lo
soporte, para no prometer algo que `to_params()` no puede honrar.

## 7. Fuera de alcance de este documento

- No se propone tocar `Curve`/`MarketSnapshot` (Fase 7.20, ya cerrada) — el patrón que usan ya
  es el que se recomienda extender a `TradeSpec`.
- No se decide aquí el diseño concreto de netting sets/colateral/FVA/MVA/KVA — la propuesta 4 se
  trata como validación de las otras tres, no como trabajo a planificar todavía.
- No se toca el core Rust (`engine-core`) en ninguna de las recomendaciones de la §3 — todos los
  cambios discutidos son de la capa C++ de orquestación y de las fachadas de cliente, igual que
  ya distingue PLAN.md §3.1 vs §3.2.
