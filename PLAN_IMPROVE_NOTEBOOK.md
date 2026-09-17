# PLAN_IMPROVE_NOTEBOOK.md — mejoras al motor motivadas por la batería de notebooks

> Documento de arquitectura y plan de implementación. Complementa a [PLAN_GREEKS.md](PLAN_GREEKS.md)
> y [PLAN_PRODUCTS.md](PLAN_PRODUCTS.md): no propone conceptos nuevos, propone **terminar/extender**
> piezas que ya existen (o casi) porque, al construir `clients/python/notebooks/01`-`07` contra el
> motor compilado de verdad, cada una de las cinco fricciones de abajo obligó a un workaround
> concreto y documentado en el propio notebook. Este documento no declara implementadas las fases:
> cada una se migra a `PLAN.md` únicamente después de quedar construida y verificada de extremo a
> extremo, mismo criterio editorial que `PLAN_GREEKS.md`/`PLAN_PRODUCTS.md`.

## 0. Motivación y alcance

`clients/python/notebooks/` (README.md de ese directorio) es hoy la mejor prueba de humo de "qué
puede hacer un cliente Python real con el motor tal como está" -- más honesta que los tests, porque
cada celda se ejecutó de verdad contra `engine.cp3XX-....pyd` (no hay números inventados, ver
mensajes de commit de esos notebooks). Al construir esa batería aparecieron cinco fricciones
recurrentes, cada una con un workaround concreto ya visible en el propio notebook afectado. Este
documento las convierte en tareas de motor con criterio de aceptación explícito, y para cada una
señala exactamente qué cambia en qué notebook una vez resuelta -- para que "mejor notebook" sea
verificable, no una frase vacía.

No es un documento de rendimiento (backend GPU, batching adicional) ni de paridad Excel/C ABI salvo
mención explícita en la Fase 0 -- eso queda fuera de alcance aquí (§4).

## 1. Resumen de las cinco fricciones

| # | Fricción | Dónde se ve el workaround | Coste del workaround |
|---|----------|---------------------------|-----------------------|
| 0 | El motor no expone las trayectorias Monte Carlo simuladas, solo medidas agregadas | `07_montecarlo_paths_q_vs_p.ipynb` reimplementa la SDE del GBM a mano en NumPy | riesgo de divergencia silenciosa entre la réplica Python y el simulador real si este cambia |
| 1 | `PayoffSensitivityQ` está registrada pero no terminada (comentario propio en `test_registry.py`) | ningún notebook la usa — no hay ejemplo verificado de qué devuelve | medida "fantasma" en `eng.list_measures()`, sin wrapper en `engine_typed` |
| 2 | `Average`/`RunningMin`/`RunningMax` no están cableados al compilador Monte Carlo del payoff | `02_exotic_and_path_dependent_options.ipynb` hornea un asiático a mano (suma de `fixing`s); no hay forma de escribir un lookback real | un asiático horneado a mano solo cubre fechas fijas conocidas de antemano; un lookback es directamente imposible hoy |
| 3 | Cada `GBM`/`GBM_P` lleva un único `observable` — no hay modelo multi-activo correlacionado | ningún notebook cubre baskets/spreads/worst-of/quanto | familia entera de producto sin cobertura, ni con workaround |
| 4 | `greeks.theta(...)` devuelve el ΔV crudo sobre el bump, no la derivada anualizada, y no está documentado | `01_vanilla_options_black_scholes.ipynb` §6 lo explica a mano tras inferirlo comparando contra Black-Scholes | cualquier otro consumidor de la API tiene que redescubrirlo empíricamente |
| 5 | `UnilateralCVA` solo cubre el ledger determinista (IRS-like) — ninguna medida nativa de CVA para un payoff con opcionalidad | `06_exposure_cva_portfolio.ipynb` integra el CVA a mano desde `PayoffExposureProfileQ` | fórmula de CVA duplicada en Python en vez de vivir una sola vez en el motor (mismo riesgo de "islas" que ya señala PLAN_GREEKS.md §0) |

(Numeradas 0-5 porque la Fase 0 es la de mayor relación impacto/esfuerzo encontrada, no por orden de
aparición en los notebooks.)

## 2. Plan por fases

### Fase 0 — Exponer diagnóstico de trayectorias Monte Carlo (`Engine.simulate_paths`)

**Problema.** `Engine` solo expone medidas agregadas (`PayoffPriceQ`, `PayoffForecastP`, ...); no
hay forma de recuperar la matriz de trayectorias que el simulador ya calcula por dentro. El notebook
`07` tuvo que reimplementar la discretización lognormal exacta del GBM en NumPy para poder dibujar
un fan chart, y solo puede validar que esa réplica es correcta comparando estadísticos agregados
contra el motor real (§5b de ese notebook) — nunca trayectoria a trayectoria.

**Tareas de motor.**
- Rust (`rust/crates/engine-core/src/models/gbm.rs`/`gbm_p.rs` o donde viva hoy la simulación de
  caminos para las medidas `Payoff*Q`/`Payoff*P`): factorizar la función de simulación para que
  devuelva la matriz completa `(n_paths, n_steps+1)` en vez de consumirla internamente y solo emitir
  el agregado — que las medidas actuales pasen a ser un caso particular ("agrega esta matriz") de
  esta función más general.
- Rust → C++: exponer vía el bridge `cxx` un método de simulación cruda (`simulate_paths`) separado
  de las medidas de precio, con un `n_paths` explícito y un tope duro documentado (p.ej. 50 000 ×
  500 pasos) para no reventar memoria si alguien lo llama desde Excel/C ABI sin darse cuenta.
- C++: `Engine::simulate_paths(model, market, pricing) -> PathMatrix` (nuevo método, no una
  `IMeasure` — es diagnóstico, no una medida de producción, para no ensuciar `Registry<IMeasure>`
  con algo que no admite `price_batch`/`price_many`/`price_grid`).
- Bindings Python (`clients/python/src/engine_py_ext.cpp`, nanobind): `Engine.simulate_paths(...)`
  devolviendo `(times: np.ndarray, paths: np.ndarray)`.
- Excel/C ABI: **fuera de alcance** en esta fase — es una herramienta de notebook/diagnóstico, no
  una función de producción para una hoja de cálculo. Decisión explícita, mismo patrón que
  `PLAN_GREEKS.md §15` deja fuera el AAD reverse-mode de Excel.

**Tareas de notebook.** `07_montecarlo_paths_q_vs_p.ipynb`: sustituir `simulate_gbm_paths(...)` por
`eng.simulate_paths(model_q, market, pricing)`/`eng.simulate_paths(model_p, market, pricing)` como
fuente del fan chart y de las estadísticas; la reimplementación NumPy pasa de "única fuente posible"
a "test de regresión cruzada" explícito (una celda que compara ambas y falla si divergen más de lo
que explica el ruido Monte Carlo).

**Criterio de aceptación.** El fan chart de `07` se dibuja directamente desde datos del motor
compilado, no de una reimplementación Python; existe un test (`clients/python/tests/`) que compara
`Engine.simulate_paths` contra los momentos analíticos del GBM/GBM_P (igual disciplina que
`test_market_products_realistic.py`).

### Fase 1 — Terminar `PayoffSensitivityQ`

**Problema.** La medida está en `Registry<IMeasure>.list()` (confirmado: aparece en
`eng.list_measures()` de todos los notebooks) pero el propio comentario de
`clients/python/tests/test_registry.py` documenta que es un ítem pendiente: "cablear
`payoff::api::payoff_sensitivity_gbm_q` ... al bridge cxx". No hay wrapper en `engine_typed`, ni test
Python, ni notebook que la use — nadie sabe hoy, sin leer Rust, si hace lo mismo que
`greeks.delta("PayoffPriceQ", "spot")` (pathwise, PLAN_GREEKS.md §5) o algo distinto.

**Tareas de motor.**
- Verificar/terminar `rust/crates/engine-core/src/payoff/sensitivity.rs::payoff_sensitivity_gbm_q` y
  su exposición completa en el bridge `cxx`.
- C++ (`measure.cpp::PayoffSensitivityQMeasure`): que invoque la función Rust real end-to-end, no un
  stub o un camino parcial.
- **Decisión de diseño explícita antes de escribir ningún wrapper**: ¿es `PayoffSensitivityQ`
  redundante con `Greek(metric="PayoffPriceQ", risk_factor="model.*", method="pathwise")` (en cuyo
  caso se documenta como alias/ruta legado y no se expone en `engine_typed`), o cubre algo que
  `Greek` no cubre hoy (p.ej. las 4 sensibilidades de un tirón sin 4 llamadas a `Engine.price`)? Este
  documento no decide — lo decide quien implemente la fase, con los dos caminos comparados.
- Si no es redundante: wrapper en `engine_typed` (mismo patrón que `engine_typed/greeks.py`).

**Tareas de notebook.** Si termina teniendo valor propio: celda nueva en
`04_greeks_and_risk_surfaces.ipynb` comparando `PayoffSensitivityQ` contra
`greeks.delta("PayoffPriceQ", "spot")` sobre el mismo contrato — mismo patrón "una ruta del motor
contra otra" que notebook `01` ya usa para motor-vs-Black-Scholes.

**Criterio de aceptación.** `PayoffSensitivityQ` deja de ser una entrada sin ejercitar en
`eng.list_measures()`: tiene al menos un test Python (`clients/python/tests/`) y, si no resulta ser
puro alias de `Greek`, una celda de notebook con un resultado verificado.

**Estado verificado / decisión (sesión de implementación de esta fase).** El enunciado de arriba
("Verificar/terminar ... su exposición completa en el bridge `cxx`" y "que invoque la función Rust
real end-to-end, no un stub") ya estaba resuelto **antes** de esta sesión, no fue trabajo de esta
fase: `cpp/engine/src/measure.cpp::PayoffSensitivityQMeasure::evaluate` (línea ~614) ya llamaba a
`payoff::payoff_sensitivity_gbm(...)` de punta a punta, y ya existían tests C++ que lo ejercitan vía
`Registry<IMeasure>`:
- `cpp/engine/tests/payoff/test_registry_wiring_payoff_measures.cpp`
  (`PayoffSensitivityQViaRegistryMatchesDirectCallToPayoffSensitivityGbm`,
  `PayoffSensitivityQRejectsAProductThatIsNotPayoffProduct`).
- `cpp/engine/tests/payoff/test_gbm_sensitivity_and_hedge.cpp` (test directo de
  `payoff::payoff_sensitivity_gbm`, sin pasar por el registry).
- `cpp/engine/tests/test_greeks.cpp` (`GreeksFase1Test`, líneas 152-236): compara
  `PayoffSensitivityQ` (vía `Registry<IMeasure>::create`) contra `Greek(metric="PayoffPriceQ",
  risk_factor=...)` para las 4 sensibilidades que soporta (`greek="spot"/"volatility"/"rate"/
  "dividend_yield"`, único conjunto que acepta `GbmGreek::parse` en
  `rust/crates/engine-core/src/payoff/sensitivity.rs`) y contra Black-Scholes cerrado — coinciden
  dentro de tolerancia Monte Carlo. El comentario de `clients/python/tests/test_registry.py` línea
  ~61 ("PayoffSensitivityQ se sumo en Fase 11, item pendiente cablear ... al bridge cxx") es
  histórico (documenta cuándo se añadió la medida), no un ítem pendiente hoy.

Lo que sí faltaba, y es lo que se implementó en esta sesión (capa cliente, ningún cambio en
Rust/C++): ningún test Python ejercitaba `PayoffSensitivityQ` directamente, y la nota del notebook
`04` la señalaba como pendiente.

**Decisión de diseño tomada:** revisando `cpp/engine/src/greeks.cpp::try_pathwise` (líneas
519-554) se confirma algo más fuerte que "coinciden dentro de tolerancia": para modelo `GBM` +
métrica `PayoffPriceQ`, la ruta `Greek(..., method="pathwise"` o `"auto"`)` invoca **literalmente
la misma función Rust** `payoff::payoff_sensitivity_gbm(...)` que
`PayoffSensitivityQMeasure::evaluate` — mismo `model`/`product`/`n_paths`/`seed` producen el mismo
resultado hasta precisión numérica, no solo dentro de ruido Monte Carlo. `PayoffSensitivityQ`
**sí es redundante con `Greek`** para las 4 sensibilidades que cubre (`spot`/`rate`/
`dividend_yield`/`volatility`, solo GBM) — no cubre nada que `Greek` no cubra hoy (no hay ningún
`greek` adicional aceptado por `GbmGreek::parse` fuera de esos 4). Por tanto: **no se añade
wrapper en `engine_typed`** (sería un alias que solo añade superficie de API); la ruta recomendada
para clientes Python sigue siendo `greeks.delta/vega/rho(...)` (`engine_typed/greeks.py`), y
`PayoffSensitivityQ` queda como medida de motor alcanzable por nombre
(`eng.price(product, [("PayoffSensitivityQ", {"greek": "spot"})], model, market, pricing,
execution)`) para quien la necesite directamente sin pasar por `Greek`.

Trabajo añadido en esta sesión: test Python
`clients/python/tests/test_engine_typed_greeks.py::test_payoff_sensitivity_q_is_a_pathwise_alias_of_greek`
(las 4 sensibilidades, tolerancia ajustada porque es la misma simulación evaluada dos veces); nota
del notebook `04` (`690df850`) actualizada con esta decisión, y sección nueva "1b" con una celda de
código real que compara ambas rutas sobre la misma call ATM del notebook.

Detalle útil para las Fases 0/5 (también añaden medidas/llamadas nuevas desde Python): `Engine.price`
no acepta un `params` separado por medida — cada elemento de la lista `measures` es o bien un
string "pelado" (`"PayoffPriceQ"`) o una tupla `(nombre, dict_de_params)`
(`("PayoffSensitivityQ", {"greek": "spot"})`), traducida a `engine::MeasureSpec` en
`to_measure_spec` (`clients/python/src/engine_py_ext.cpp` línea ~69). Este mecanismo ya soporta
pasar cualquier `Params` a cualquier medida registrada sin cambios de binding.

### Fase 2 — Compilar `Average`/`RunningMin`/`RunningMax` en el compilador Monte Carlo del payoff

**Problema.** `rust/crates/engine-core/src/payoff/compile.rs::compile_scalar` soporta 14 de los 23
nodos de `ScalarExpr` documentados en `engine_typed.payoff`; `Average`/`RunningMin`/`RunningMax`
están explícitamente fuera de alcance (`compile.rs` línea ~360, test
`average_no_deberia_soportarse_todavia`). `02_exotic_and_path_dependent_options.ipynb` §6 hornea un
asiático aritmético a mano (`(fixing(t1)+...+fixing(tn))/n` vía `add`/`div`) — funciona porque las
fechas de promediado son fijas y conocidas de antemano, pero un lookback (payoff que depende del
máximo o mínimo corrido de la trayectoria, no de un conjunto fijo de fechas) no tiene ningún
workaround posible con los nodos actuales.

**Tareas de motor.**
- Rust (`compile.rs`): añadir los brazos `"average"`, `"running_min"`, `"running_max"` en
  `compile_scalar`. A diferencia de los nodos actuales (que evalúan un escalar en un instante dado
  sin memoria de la trayectoria), estos requieren **estado acumulado por trayectoria a lo largo de
  la simulación** — el intérprete del `CompiledPayoff` necesita llevar un acumulador (suma corriente
  para `Average`, running min/max) por cada trayectoria simulada, no solo evaluar nodos puros.
- Rust: extender el motor de ejecución del payoff compilado para materializar y actualizar ese
  estado en cada paso de tiempo simulado (probablemente el cambio de mayor alcance de las seis
  fases — toca el intérprete, no solo el compilador).
- Verificación: test de paridad entre el `Average` nativo nuevo y la réplica manual (suma de
  fixings) que ya usa el notebook `02` hoy — deben coincidir EXACTAMENTE en las mismas
  trayectorias/semilla (no solo "dentro de ruido Monte Carlo", porque ambos caminos deberían operar
  sobre las mismas trayectorias si la semilla y el número de pasos coinciden).

**Tareas de notebook.** `02_exotic_and_path_dependent_options.ipynb`: sustituir la sección de
asiático horneado a mano por `q.average(...)` nativo (manteniendo la réplica manual como celda de
verificación, no como la única vía); añadir una sección nueva de lookback real
(`q.running_max`/`running_min`) que hoy directamente no se puede escribir.

**Criterio de aceptación.** Los 23 nodos de `ScalarExpr` llegan al compilador Monte Carlo (hoy 14);
notebook `02` tiene un lookback real, no solo un asiático horneado a mano.

**Estado verificado / correcciones (sesión de implementación de esta fase).** Dos premisas del
enunciado de arriba estaban equivocadas o incompletas; se implementó y verificó lo que sigue, sin
restarle mérito al plan:

1. **"Estado acumulado por trayectoria a lo largo de la simulación" (líneas 173-179) es
   incorrecto.** `CompiledPayoff::required_times()` (`ir.rs`) ya recopila POR ADELANTADO (antes de
   simular) la unión de todos los instantes que el programa necesita; el modelo simula
   exactamente ese conjunto de una vez para todas las rutas, y `eval::eval_scalar`/
   `sensitivity::eval_scalar` son consultas de ACCESO ALEATORIO (`ObservablePath::value_at`) sobre
   esa ruta ya materializada — nunca un recorrido con estado mutable paso a paso. Los tres nodos
   nuevos son agregados PUROS, igual de "puros" que el resto de `ScalarOp`; no hizo falta tocar el
   intérprete en el sentido de streaming, solo añadir tres brazos de `match` en `eval_scalar` (y su
   espejo en `sensitivity::eval_scalar`, genérico sobre `T: DualNumber`, que el enunciado no
   mencionaba pero también tiene un `match` exhaustivo sobre `ScalarOp`).

2. **La forma JSON de `Average`/`RunningMin`/`RunningMax` NO era una decisión de diseño libre —
   ya estaba establecida, de forma consistente, en tres sitios independientes ANTES de esta
   sesión**, algo que ni el plan ni la sesión de arranque habían verificado:
   `cpp/engine/include/engine/payoff/expression.hpp` (clases `Average`/`RunningMin`/`RunningMax`
   del AST de autoría C++, ya con `ScenarioEvaluator`/`ValidationVisitor`/`DependencyVisitor`/
   `CanonicalVisitor` completos), `docs/schema/engine.payoff/v1.schema.json` (`ScalarAverage`/
   `ScalarRunningMin`/`ScalarRunningMax`), y `engine_typed/payoff.py` en Python (builders
   `average(observable, schedule, weights)`/`running_min(observable)`/`running_max(observable)`,
   ya expuestos, sin usar). La forma real, que el compilador Rust ahora respeta exactamente:
   - `Average { observable, schedule: Vec<f64>, weights: Vec<f64> }` — una suma PONDERADA
     (`sum(weights[i] * fixing(observable, schedule[i]))`), NO una media aritmética con un
     divisor `1/n` implícito. Un asiático equiponderado se expresa pasando `weights = [1/n; n]`.
   - `RunningMin`/`RunningMax { observable }` — SIN ningún campo de schedule propio. La semántica
     ya establecida en C++ (`ScenarioEvaluator::visit(RunningMin)`,
     `context_.path.fixings_up_to(observable, *cursor_)`) es "mínimo/máximo de TODOS los fixings
     conocidos hasta el instante activo (cursor)", sobre una `MarketPath` poblada libremente por
     quien evalúa — un concepto que no existe en el motor Monte Carlo Rust (que solo conoce, en
     preflight, `CompiledPayoff::required_times()`).

   **Decisión de diseño tomada** (documentada también en el doc-comment de
   `ScalarOp::RunningMin`, `ir.rs`): `eval::eval_scalar`/`sensitivity::eval_scalar` reducen sobre
   TODOS los instantes de `payoff.required_times()` que sean `<=` el cursor activo (panica si no
   hay cursor, igual que `ValidationVisitor::visit(RunningMin)` en C++ exige uno en preflight). Es
   la única forma fiel a la semántica C++ ("hasta el instante activo, sobre lo que ya se conoce")
   compatible con la arquitectura "preflight conoce todo por adelantado" de Rust, sin inventar un
   campo nuevo que divergiera del AST/schema/Python ya establecidos. Implicación práctica para
   quien autora un contrato: si ningún otro nodo del programa referencia instantes intermedios del
   observable, `required_times()` no tiene más puntos que los ya usados en otra parte del árbol —
   un lookback con monitorización fina necesita un "ancla" de monitorización (un `Average` con
   `weights` a cero, que no contribuye ningún importe pero inyecta su `schedule` en
   `required_times()`); el notebook `02` documenta y usa este patrón explícitamente, no lo oculta.

   `RunningMin`/`RunningMax` **sí leen el cursor** (a diferencia de lo que un diseño "desde cero"
   habría podido elegir, ver la nota original de esta fase en `PLAN_IMPROVE_NOTEBOOK.md` antes de
   esta corrección) — no por elección estética sino porque es la única lectura consistente con la
   forma ya fijada del AST (`observable` sin `schedule`) y con el nombre "running"/corrido.

3. **El criterio de aceptación ("los 23 nodos... hoy 14") sobreestima el alcance real de esta
   fase.** 14 (antes) + 3 (`average`/`running_min`/`running_max`) = **17**, no 23. Los 6 nodos
   restantes sin soportar (`DiscountFactor`/`FxConversion`/`Parameter`/`EventTime`/`Before`/
   `After`, ver `compile.rs::unsupported_node`) requieren un observable de mercado que ningún
   modelo Q de esta fase evalúa todavía — fricción DISTINTA, no mencionada en las tareas de motor
   concretas de esta fase (línea 172), y deliberadamente NO implementada aquí para no expandir el
   alcance sin que se pidiera explícitamente.

**Trabajo realizado.** `rust/crates/engine-core/src/payoff/{ir,compile,eval,sensitivity,mod}.rs`:
tres brazos nuevos de `ScalarOp` con la forma exacta ya establecida (ver arriba), `required_times()`
extendido para `Average::schedule`, `compile_scalar` con la validación de `schedule`/`weights` de
`average` (no vacío, longitudes iguales, `schedule` estrictamente ascendente — mismo criterio que
`ValidationVisitor::check_schedule_ascending_and_finite` en C++, que valida el mismo AST), y los
tres brazos de reducción en `eval_scalar`/`sensitivity::eval_scalar`. 226/226 tests Rust
(`cargo test -p engine-core --release`, antes 223), incluida paridad EXACTA (no solo "compila")
entre el nodo nativo y la réplica horneada a mano sobre la misma ruta determinista, y un test de
`running_max`/`running_min` que confirma el filtrado `<= cursor`. C++ (`cmake --build build`,
`ctest --test-dir build -C Release`): 470/470 sin tocar una línea de C++ — `ScalarOp`/
`CompiledPayoff` nunca cruzan el bridge `cxx` (confirmado por grep en `rust/crates/engine-ffi/` y
`cpp/`), solo el JSON canónico, que C++ ya sabía producir para estos tres nodos. Python: no hizo
falta añadir builders a `engine_typed.payoff` (`average`/`running_min`/`running_max` ya existían,
sin ejercitar) — se añadieron 3 tests de paridad exacta en
`clients/python/tests/test_engine_typed_payoff.py`. Notebook `02`: sección 6 reescrita para usar
`q.average` nativo (réplica manual como celda de verificación cruzada), nueva sección 7 de
lookback real con `q.running_max`/`q.running_min` (patrón de "ancla" de monitorización semanal),
ejecutado de punta a punta con `jupyter nbconvert --execute --inplace` — paridad exacta confirmada
en ejecución real (`15.767402 == 15.767402` para el asiático n=4, `41.946712 == 41.946712` para el
lookback de 52 fechas) y el lookback put domina a la vanilla put ATM (29.110 vs 15.964), como
exige la cota teórica.

### Fase 3 — Modelo multi-activo correlacionado

**Problema.** Cada `GBM`/`GBM_P` lleva un único `observable`; no existe forma de precisar baskets,
spreads, quantos o worst-of/best-of con dos o más activos correlacionados bajo el mismo browniano.
Es la única familia de producto que ningún notebook de la batería cubre, ni siquiera con un
workaround — a diferencia de las fricciones 1/2, aquí no hay ningún truco de composición de AST que
lo resuelva sin cambiar el modelo subyacente.

**Tareas de motor** (la de mayor alcance de las seis — diseño nuevo, no solo terminar algo a medias):
- Rust (`rust/crates/engine-core/src/models/`): nuevo modelo `GbmBasket` con un vector de
  `observable`s, vectores de `s0`/`sigma`/`r`/`q` (o `mu` bajo P) por activo, y una matriz de
  correlación — simulación vía descomposición de Cholesky de la matriz de correlación aplicada a
  normales independientes, un vector de trayectorias correlacionadas por paso.
- Rust: extender `compile.rs`/el intérprete del payoff para resolver `fixing`/`current` sobre
  CUALQUIERA de los observables declarados por el modelo activo (hoy el compilador asume un único
  observable de contexto — ver cómo `Fixing`/`Current` resuelven `observable_slot` en `compile.rs`).
- C++ (`bootstrap.cpp`): registrar `GbmBasket` en `Registries`, mismo patrón que
  `HullWhite2F`/`GBM_P`.
- Python (`engine_typed/model.py`): clase tipada `GbmBasket` (lista de observables/parámetros +
  matriz de correlación con validación de simetría/PSD, mismo criterio que `Market` valida pillars
  crecientes hoy).

**Tareas de notebook.** Nuevo notebook `08_multi_asset_options.ipynb` (basket call, spread option,
worst-of/best-of, quanto) — no tiene sentido intentarlo hoy porque el motor no lo soporta, ni con
workaround.

**Criterio de aceptación.** Un basket call de 2 activos correlacionados se precia, y su precio se
mueve en la dirección correcta al variar la correlación (test de sensibilidad de signo, mismo
espíritu que las verificaciones de `PLAN_GREEKS.md`); notebook `08` existe y compara al menos dos
niveles de correlación.

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).** A diferencia
de la Fase 2, el grep explícito pedido por el enunciado (`expression.hpp`/
`docs/schema/engine.payoff/v1.schema.json`/`engine_typed.payoff` por señales de
`basket`/`spread_option`/`worst_of`/`quanto`/`correlation`/`observable`s en plural) **no encontró
nada preexistente** — se partió de cero en las tres capas de autoría de AST, confirmando la
premisa del plan (línea 280: "aquí no hay ningún truco de composición de AST que lo resuelva sin
cambiar el modelo subyacente"). Lo que sí estaba ya preparado, exactamente como adelantaba el
enunciado (línea 283-290 de la versión previa de esta sección): `CompiledPayoff::observable_slots`
(`ir.rs`) y `Compiler::observable_slot` (`compile.rs`) resuelven cualquier número de nombres de
observable distintos a índices sin ningún cambio — un contrato con `fixing`/`current` sobre dos
observables distintos ya compilaba antes de esta sesión.

1. **Mecanismo elegido para `Params` (punto 4 del enunciado): Opción A** — `"observables"` es un
   único `string` delimitado por comas (`"EQ.SPOT.A,EQ.SPOT.B"`), parseado en el constructor de
   `GbmBasketModel` (`cpp/engine/src/model.cpp::split_comma_separated`), en vez de claves
   numeradas (Opción B). Motivo: `dict_to_params` (`engine_py_ext.cpp`) ya distinguía `list`/
   `tuple` → `vector<double>`; añadir una rama que primero mira el tipo del primer elemento
   (`list[str]` → `string` unido por comas) es un cambio de ~20 líneas contenido en un único
   punto del binding, y deja `GbmBasket(observables=["EQ.SPOT.A", "EQ.SPOT.B"], ...)` como la
   forma natural de construirlo desde Python — más "de Python" que `observable_0`/`observable_1`.
   **No se amplió `ParamValue`** con un variante `vector<string>` (la opción que el enunciado
   pedía evitar salvo necesidad real): no hizo falta, Opción A cubre el caso sin tocar un tipo que
   usan todos los modelos/productos/medidas del motor. `"correlation"` (matriz `n x n`) sí cabe
   tal cual en `ParamValue` como `vector<double>` aplanada **fila a fila** — convención
   documentada de forma idéntica en `GbmBasketModel::correlation()` (C++),
   `basket_api::price_payoff_basket_gbm_q` (Rust) y `GbmBasket.to_params()` (Python); ninguna capa
   reordena.
2. **Misma medida `"PayoffPriceQ"`, generalizada, no una medida nueva** (punto 3 del enunciado):
   `PayoffPriceQMeasure::evaluate` (`measure.cpp`) intenta `dynamic_cast<GbmModel>` primero y
   `dynamic_cast<GbmBasketModel>` después, delegando en una sobrecarga nueva de
   `payoff::risk_neutral_price_gbm(..., const GbmBasketModel&, ...)` (mismo patrón de sobrecarga
   por tipo de modelo que ya usaban `hit_probability_gbm(GbmModel)`/`hit_probability_gbm(GbmPModel)`
   para distinguir Q/P). Se descartó `"PayoffBasketPriceQ"` como medida separada porque el AST/
   compilador Rust ya es agnóstico al número de observables — un caller no debería tener que
   elegir el nombre de la medida según si el contrato se va a precisar contra un único activo o
   un basket; el nombre de la medida describe QUÉ se pide (un precio Q), no QUIÉN lo calcula.
3. **Diseño Rust: `Vec<Tensor<B,1>>`, no `Tensor<B,2>`** (una de las dos representaciones que
   dejaba abiertas el enunciado, punto 1): se generalizó directamente el patrón ya probado de
   `HullWhite2F::simulate_path` (combinación lineal escalar de shocks independientes por Cholesky
   2x2, extendida aquí a Cholesky N x N vía `payoff::hedge::cholesky_decompose` reutilizado —
   ahora `pub(crate)` en vez de privado al módulo) en lugar de introducir `matmul`/`reshape` con
   broadcasting `[1,n]` vs `[n_paths,n]`, una API de tensor 2D que este crate no usa hoy en ningún
   otro sitio. La vectorización real sigue ocurriendo en la dimensión de PATHS (cada
   `Tensor<B,1>` mueve `n_paths` escalares a la vez), no se pierde nada frente a una
   representación `Tensor<B,2>`.
4. **Limitación real descubierta durante la implementación, no anticipada por el enunciado**: el
   numerario de descuento. `GbmModel` (N=1) descuenta con su única `r`; un basket con `r`
   heterogénea por activo no tiene un numerario único obvio sin modelar curvas de descuento por
   activo (lo que necesitaría un quanto de tipo fijo real, con su ajuste de drift
   `-rho*sigma_S*sigma_FX`). Se decidió, con criterio, **exigir la MISMA `r` para todos los
   activos** (`basket_api::validate_common_discount_rate`, `Err` explícito si difieren en vez de
   elegir `r[0]` en silencio) — alcance mínimo suficiente para el criterio de aceptación de esta
   fase (basket call de 2 activos con la misma `r`), documentado como límite explícito, no oculto.
5. **Validación en Python** (`engine_typed/model.py::GbmBasket`): simetría, diagonal `== 1.0`, y
   PSD vía una implementación de Cholesky en Python puro (`_cholesky_lower`, mismo algoritmo que
   `payoff::hedge::cholesky_decompose` en Rust) — sin depender de `numpy` solo para esto
   (`engine_typed` no lo importaba en ningún otro sitio).

**Trabajo realizado, por capa** (compilado y testeado en orden, cada capa antes de pasar a la
siguiente):

- **Rust** (`rust/crates/engine-core/src/models/gbm_basket.rs` nuevo,
  `payoff/basket_api.rs` nuevo, `payoff/hedge.rs::cholesky_decompose` → `pub(crate)`,
  `payoff/api.rs::bridge_seed_for_path` → `pub(crate)`, registrados en `models/mod.rs`/
  `payoff/mod.rs`): `GbmBasket<B>::new`/`simulate_at_times`, `price_payoff_basket_gbm_q` (preflight
  de tamaños + `resolve_slot_to_asset` por nombre + `validate_common_discount_rate`). Tests:
  momentos marginales vs. GBM univariante, covarianza de log-retornos vs. fórmula analítica
  `rho*sigma1*sigma2*T`, rechazos de matriz no cuadrada/no PSD/longitudes distintas, rechazo de
  observable no declarado, rechazo de `r` heterogénea, y el test de signo explícito del criterio
  de aceptación (`basket_call_price_increases_with_correlation`). `cargo test -p engine-core
  --release`: **235/235** (226 previos + 9 nuevos: 6 en `gbm_basket`, 3 en `basket_api`).
- **cxx bridge** (`rust/crates/engine-ffi/src/lib.rs`): `price_payoff_basket_gbm_q` nueva,
  reutiliza el struct `PayoffQPriceResult` ya existente (misma forma que la ruta de un único
  activo, sin struct nuevo).
- **C++** (`cpp/engine/include/engine/model.hpp`/`model.cpp`: `GbmBasketModel`;
  `cpp/engine/include/engine/payoff/measures.hpp`/`measures.cpp`: sobrecarga de
  `risk_neutral_price_gbm`; `measure.cpp`: `PayoffPriceQMeasure::evaluate` generalizado;
  `bootstrap.cpp`: `register_type<GbmBasketModel>("GbmBasket")`). Test nuevo
  `cpp/engine/tests/payoff/test_gbm_basket_measures.cpp` (6 tests: construcción/round-trip de
  `to_params`, rechazos de forma, precio vía llamada directa y vía `Registry<IMeasure>`, signo de
  correlación). `ctest --test-dir build -C Release`: **476/476** (470 previos + 6 nuevos).
- **Python binding** (`clients/python/src/engine_py_ext.cpp::dict_to_params`): rama nueva
  `list[str]`/`tuple[str]` → `string` delimitado por comas.
- **Python tipado** (`clients/python/src/engine_typed/model.py::GbmBasket`, exportada en
  `engine_typed/__init__.py`): validación de simetría/diagonal/PSD documentada arriba.
- **Test Python** nuevo `clients/python/tests/test_engine_typed_model.py` (10 tests: 5 de
  validación temprana con `pytest.raises`, paridad `to_params`, precio positivo/finito, rechazo de
  observable no declarado en el basket, y los dos tests de sensibilidad de signo explícitos del
  criterio de aceptación — basket call sube con la correlación, spread option BAJA, razonando el
  signo de cada uno en vez de asumir "correlación sube todo"). `pytest clients/python/tests`:
  **155 passed** (145 previos + 10 nuevos), mismos 2 errores preexistentes de fixture
  (`abi_dll_path` no encontrado) ya conocidos y no relacionados.
- **Notebook** `clients/python/notebooks/08_multi_asset_options.ipynb` (nuevo, ejecutado con
  `jupyter nbconvert --to notebook --execute --inplace`, sin errores, 4 gráficas generadas):
  basket call (`max(S_A+S_B-K,0)`, sube con rho: 8.90 → 11.34 → 13.51 para rho=0.0/0.4/0.8), spread
  option (`max(S_A-S_B-K,0)`, baja: 14.10 → 10.96 → 6.35), worst-of call (sube: 2.08 → 3.56 →
  5.85) y best-of call (baja: 16.17 → 14.69 → 12.40) sobre los mismos tres niveles de correlación,
  y un compo option etiquetado explícitamente como "quanto-style" (`(FX(T)/FX0)*max(S-K,0)`, sube:
  9.41 → 10.24 → 11.11) con una nota explicando por qué NO es un quanto de tipo fijo real (ese
  necesitaría el ajuste de drift `-rho*sigma_S*sigma_FX` que `GbmBasket` no implementa — límite
  documentado, no oculto). `clients/python/notebooks/README.md` actualizado con la entrada `08`.

**Limitaciones dejadas fuera de alcance a propósito** (documentadas en el notebook `08` §6 y
repetidas aquí para que queden en el plan, no solo en la celda):

- Solo `PayoffPriceQ` — sin `PayoffExerciseQ`/`PayoffHitProbabilityQ`/`PayoffExposureProfileQ`/
  sensibilidades/CVA/hedge para `GbmBasket` (`GbmBasketModel::capabilities()` no declara
  `supports_continuous_barrier_bridge` ni `supports_early_exercise_regression`).
- Sin `valuation_time`/Theta para el basket.
- Todos los activos de un mismo `GbmBasket` deben compartir la MISMA `r` (sin curvas de descuento
  por activo) — un quanto de tipo fijo real no está soportado, solo el compo option del notebook.
- El diseño soporta `N` activos arbitrarios (Cholesky general `N x N`, sin límite estructural),
  pero solo se ejercitó `N=2` en tests/notebook — no hay ninguna limitación conocida para `N>2`,
  simplemente no hacía falta más para el criterio de aceptación de esta fase.

### Fase 4 — Documentar/normalizar la convención de `greeks.theta`

**Problema.** `greeks.theta(...)` (`engine_typed/greeks.py`) devuelve el ΔV crudo sobre el bump
`dt` (1 día por defecto: `1/365` años) — NO la derivada anualizada `dV/dt`. Es la única Greek con
esta convención (delta/gamma/vega/rho sí son derivadas propiamente normalizadas por el tamaño del
bump). El docstring actual no lo dice explícitamente; se detectó comparando numéricamente contra la
fórmula cerrada de Black-Scholes en `01_vanilla_options_black_scholes.ipynb` §6
(`theta_mc ≈ bs_theta_anual * (1/365)`, no `theta_mc ≈ bs_theta_anual`).

**ADR.**

**ADR-IN-01 — Convención de `greeks.theta` y anualización opcional.** `theta()` **mantiene** por
defecto (`annualized=False`) el comportamiento actual: el ΔV crudo de un único bump `dt` (años,
`DEFAULT_THETA_BUMP` = 1/365 si no se pasa `bump` explícito) — es decir, `V(t+dt) - V(t)`, sin
dividir por `dt` — porque para un desk es más intuitivo como "P&L de revalorizar un día" que como
una tasa anualizada, y porque `dt` por defecto es un día calendario real (no un bump infinitesimal
elegido solo por estabilidad numérica, como en `delta`/`vega`/`rho`/`dv01`). Se **añade** un
parámetro `annualized: bool = False`: si es `True`, el valor devuelto es `ΔV / dt` (derivada
anualizada `dV/dt`), para que el caller no tenga que redescubrir ni escalar el bump a mano (el
workaround que hoy vive en `01_vanilla_options_black_scholes.ipynb` §6). El escalado ocurre en
Python **después** de que el motor devuelva el escalar — `Engine.price(...)["Greek"]` es
`engine.MeasureResult`, que no expone `bump_used` (a diferencia de `engine.GreekResult`, lo que
devuelve `Engine.all_greeks`) — así que `theta()` devuelve una subclase `ThetaGreek` con un método
`annualize(raw_value)` que el caller invoca explícitamente sobre el escalar ya leído; no hace falta
tocar el bridge C++ (fuera de alcance de esta fase). Cuando `annualized=True` y no se pasa `bump`
explícito, `ThetaGreek` resuelve `bump` a `DEFAULT_THETA_BUMP` y lo manda **explícito** en el spec
(en vez de dejarlo en `None`, que dejaría al motor aplicar su propio default interno) — así el bump
que usa el motor para calcular y el bump por el que se divide son garantizadamente el mismo, sin
depender de que el default de Python y `cpp/engine/src/greeks.cpp::default_time_shift_bump()` sigan
coincidiendo por convención entre dos lenguajes.

**Tareas de motor** (cambio de documentación/API, no de cálculo — la de menor esfuerzo de las
seis):
- `engine_typed/greeks.py::theta()`: docstring ampliado con la fórmula exacta devuelta en cada
  caso (`annualized=False`/`True`), y parámetro `annualized: bool = False` que divide el resultado
  por `bump` (vía `ThetaGreek.annualize(...)`, ver ADR-IN-01) antes de devolverlo.

**Tareas de notebook.** `01_vanilla_options_black_scholes.ipynb` §6 ya explica la convención a mano;
una vez el docstring/API la exponga explícitamente (y exista `annualized=True` si el ADR lo decide
así), la celda puede simplificarse a usar esa opción en vez de escalar `bump_days` manualmente.

**Criterio de aceptación.** El docstring de `greeks.theta` explica la convención sin que haga falta
inferirla empíricamente comparando contra una fórmula cerrada externa.

### Fase 5 — Medida de CVA genérica sobre cualquier perfil de exposición

**Problema.** `UnilateralCVA` solo funciona sobre el ledger determinista (`IRSwap`-like, vía
`market_snapshot_bridge`) — no hay medida de CVA nativa para un `PayoffProduct` con opcionalidad real
(fuera de ese ledger determinista, ver notebook `02`/`03`). `06_exposure_cva_portfolio.ipynb` §2
integra el CVA a mano en Python desde `PayoffExposureProfileQ`
(`CVA = (1-R) * Σ EE_i · ΔPD_i · DF_i`) — la misma fórmula que `UnilateralCvaMeasure` ya implementa
en C++ para IRS, duplicada en un notebook. Es exactamente el patrón de "islas" que
`PLAN_GREEKS.md §0` ya señaló como riesgo para otra funcionalidad, aquí aplicado a CVA.

**Tareas de motor.**
- C++ (`measure.cpp`): nueva medida `PayoffUnilateralCvaQMeasure` (o generalizar
  `UnilateralCvaMeasure` para aceptar cualquier fuente de perfil de exposición, no solo el ledger
  determinista) que reutilice `PayoffExposureProfileQ` internamente + la misma integración de
  supervivencia (hazard rate constante, recovery rate) que ya usa `UnilateralCvaMeasure` para IRS —
  una sola implementación de la fórmula de integración, no dos.
- Registrar en `Registries`/`bootstrap.cpp`, exponerla en `eng.list_measures()`.

**Tareas de notebook.** `06_exposure_cva_portfolio.ipynb` §2: sustituir `manual_cva(...)` por
`eng.price(call_product, ["PayoffUnilateralCvaQ"], ...)`; la función manual queda como celda de
verificación cruzada (mismo patrón que Fase 0/2), no como único camino.

**Criterio de aceptación.** `eng.price(option_product, ["PayoffUnilateralCvaQ"], ...)` da el mismo
resultado (dentro de ruido Monte Carlo, misma tolerancia que usan los tests de
`test_market_products_realistic.py`) que la integración manual que hoy vive en el notebook `06`.

## 3. Priorización sugerida

| Fase | Esfuerzo relativo | Notebooks que mejora | Bloquea |
|------|--------------------|------------------------|---------|
| 4 — convención de theta | trivial (docs + flag opcional) | `01` | nada, quick win |
| 1 — terminar `PayoffSensitivityQ` | bajo–medio | `04` | nada |
| 5 — CVA genérico | medio | `06` | nada |
| 0 — `simulate_paths` | medio | `07` (y cualquier notebook futuro con visualización de trayectorias) | nada |
| 2 — `Average`/running min/max | medio–alto (toca el intérprete, no solo el compilador) | `02` | Fase 3 no lo necesita pero comparte intérprete |
| 3 — modelo multi-activo | alto (diseño nuevo) | notebook `08` futuro | mayor impacto en cobertura de producto |

## 4. Fuera de alcance de este documento

- Ninguna fase de este documento se implementa aquí — es un plan, mismo criterio editorial que
  `PLAN_GREEKS.md §11`/`PLAN_PRODUCTS.md`: cada fase se migra a `PLAN.md` solo tras quedar construida
  y verificada de extremo a extremo.
- Rendimiento (backend GPU, batching adicional más allá de `price_batch`/`price_many`/`price_grid`
  ya existentes): fuera de alcance.
- Paridad Excel/C ABI para las fases 1, 2, 3 y 5: cada una se decide en su propia fase si aplica —
  por defecto se asume que sí (son medidas/modelos que entran por `Registry<IMeasure>`/
  `Registry<IModel>`, el mismo mecanismo que ya llega a Excel/C ABI sin cambios adicionales, ver
  `PLAN.md §5.4`), salvo la Fase 0 que se declara explícitamente fuera de alcance para Excel/C ABI.

## 5. Trazabilidad: fase → notebook → celda

| Fase | Notebook | Sección afectada hoy |
|------|----------|------------------------|
| 0 | `07_montecarlo_paths_q_vs_p.ipynb` | intro + `simulate_gbm_paths` (§1) |
| 1 | `04_greeks_and_risk_surfaces.ipynb` | nueva sección (no existe todavía) |
| 2 | `02_exotic_and_path_dependent_options.ipynb` | §6 "Asiático aritmético horneado a mano" |
| 3 | `08_multi_asset_options.ipynb` | secciones 2-5 (basket/spread/worst-of-best-of/compo) |
| 4 | `01_vanilla_options_black_scholes.ipynb` | §6 "Greeks de la call ATM" |
| 5 | `06_exposure_cva_portfolio.ipynb` | §2 "Opción frente a una contraparte" |

Cada notebook de la lista de arriba lleva ya, tras esta actualización, una nota corta apuntando a la
fase correspondiente de este documento en el punto exacto donde vive el workaround.
