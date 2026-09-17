# PLAN_IMPROVE_NOTEBOOK2.md — segunda ronda de mejoras al motor, motivada por `08`/`09`

> Documento de arquitectura y plan de implementación. Complementa a [PLAN_GREEKS.md](PLAN_GREEKS.md),
> [PLAN_PRODUCTS.md](PLAN_PRODUCTS.md) y [PLAN_IMPROVE_NOTEBOOK.md](PLAN_IMPROVE_NOTEBOOK.md) (cuyas
> seis fases ya quedaron construidas): no repite ninguna friccion de ese documento, recoge las que
> aparecieron DESPUES, al construir `08_multi_asset_options.ipynb` y sobre todo
> `09_option_strategies_and_greeks.ipynb` (14 estrategias custom x 6 escenarios de volatilidad,
> payoff + 7 griegas cada una) contra el motor compilado real. Mismo criterio editorial que los
> planes anteriores: nada de aqui esta implementado, cada fase se migra a `PLAN.md` solo despues de
> quedar construida y verificada de extremo a extremo.

## 0. Motivación y alcance

Construir `09` (la bateria mas exigente de la familia hasta ahora: por cada una de 14 estrategias,
6 escenarios de volatilidad, un grid de spot, y por cada punto varias llamadas de motor para
delta/gamma/theta/vega/vanna/volga/charm) forzo a leer con detalle `greeks.cpp`/`measure.cpp` en
vez de solo consumir la API desde Python, y eso saco a la luz limitaciones que ningun notebook
anterior habia ejercitado: un bug de correccion silenciosa (no solo una pieza que falta), una
limitacion de cobertura de la Hessiana para contratos multi-fecha, y varios sitios donde el
notebook tuvo que apoyarse en NumPy porque el motor no expone el calculo equivalente -- exactamente
el patron que dio origen a `PLAN_IMPROVE_NOTEBOOK.md` la primera vez, ahora una capa mas adentro.

No es un documento de rendimiento en el sentido de backend GPU (aunque la Fase 3 de aqui abajo SI
es una mejora de rendimiento real: reducir el numero de pasadas Monte Carlo por informe de riesgo)
ni de paridad Excel/C ABI -- eso sigue fuera de alcance salvo mencion explicita.

## 1. Resumen de las fricciones

| # | Friccion | Donde se vio | Coste del workaround / riesgo |
|---|----------|---------------|-------------------------------|
| 0 | `Greek(method="auto")` resuelve a la ruta *pathwise* para GBM+`PayoffPriceQ`+factor de modelo, y esa ruta (`greeks.cpp::try_pathwise` -> `payoff::payoff_sensitivity_gbm[_p]`) NUNCA recibe `pricing.pricing_date()` | `09`: charm salio exactamente `0.0` en las 14 estrategias hasta forzar `method="bump_and_reval"` a mano | **Bug de correccion silenciosa, no de cobertura**: cualquier Greek de spot/vol/rate pedida con `pricing_date != 0` da un numero sin avisar de que ignoro el desplazamiento temporal |
| 1 | `Engine.hessian` no cubre contratos que dependen de mas de una fecha terminal (LRM exige una unica fecha, mismo criterio que ya excluye `ContractOp::Exercise`) | `09`: gamma/vanna/volga salen `NaN` para `long_calendar_spread`/`short_calendar_spread`, sin ningun mensaje que lo explique | El notebook tuvo que documentar el `NaN` a mano; no hay fallback bump-and-reval como si existe para Greeks de primer orden via `method="auto"` |
| 2 | No hay binding Python de `ScenarioEvaluator`/`market_snapshot_bridge` (evaluacion DETERMINISTA de un contrato dado un escenario, ya usada internamente por `PresentValueMeasure`) | `09`: `intrinsic_value(legs, spot_grid)` reimplementa `max(S-K,0)`/`max(K-S,0)` a mano en NumPy por cada leg de cada una de las 14 estrategias | Misma familia de riesgo que la Fase 0 original de `PLAN_IMPROVE_NOTEBOOK.md` (motor no expone algo que ya calcula internamente) pero en la rama determinista, no la Monte Carlo |
| 3 | `Engine.simulate_paths` (Fase 0 del plan anterior) solo dispacha a `GBM`/`GBM_P` -- `GbmBasket` (Fase 3, posterior) no tiene diagnostico de trayectorias | Ningun notebook lo necesito todavia, pero `08` no podria dibujar un fan chart de basket sin reimplementar Cholesky+GBM a mano si se pidiera | Gap de cobertura previsible en cuanto un notebook futuro quiera visualizar trayectorias correlacionadas |
| 4 | `GbmBasket` solo tiene cableada `PayoffPriceQ` (decision explicita de la Fase 3 original) -- sin `Greek`/`all_greeks`/`hessian` para modelos multi-activo | `08` no puede mostrar delta/vega por activo de un basket, ni la vanna real entre dos activos (cross-gamma spot1/spot2) | Limitacion de cobertura de producto, ya documentada como fuera de alcance en su momento, pero ahora hay un caso de uso concreto que la necesitaria |
| 5 | `Engine.price`/`price_batch`/`price_many`/`price_grid` devuelven resultado indexado por NOMBRE de medida; toda `Greek` se registra bajo el nombre fijo `"Greek"`, asi que pedir delta+vega+theta en una sola llamada hace que se pisen entre si en el dict de salida | `09`: 14 estrategias x 6 vols x 9 spots x ~5 llamadas de motor cada punto (una por Greek), cada una su propia simulacion Monte Carlo | Multiplica el numero de simulaciones Monte Carlo necesarias para un informe de riesgo que en principio podria compartir una unica pasada |
| 6 | Incluso `Engine.all_greeks` (el barrido "automatico") dispara una llamada a `compute_greek` -- con su propia simulacion -- POR CADA factor de riesgo candidato (confirmado leyendo `compute_all_greeks` en `greeks.cpp`); solo la Hessiana LRM comparte una pasada, y solo para gamma+vanna+volga de UN par de factores | Mismo caso que la friccion 5: sin este cableado, ningun notebook puede pedir un "informe de riesgo" barato en pasadas Monte Carlo | Coste de motor (tiempo de ejecucion) que crece linealmente con el numero de Greeks pedidas, no compartido |
| 7 | No hay forma de alinear el generador aleatorio del motor (backend Burn/ndarray) con uno externo (NumPy `default_rng`) para comparar trayectoria a trayectoria, solo momentos agregados | `07` (ya con `Engine.simulate_paths` real, Fase 0 del plan anterior) solo puede hacer el cross-check motor-vs-NumPy sobre medias/varianzas, nunca ruta a ruta | Fricción menor, documentada en el propio notebook; limita cuanto se puede endurecer un test de regresion cruzada |
| 8 | `engine_typed.payoff` no tiene builders de "pata vainilla" (`call_leg`/`put_leg`) ni un combinador de estrategia -- cada notebook que construye combinaciones (`02` straddle/strangle, `09` 14 estrategias) reinventa el mismo patron `when(T, cashflow(qty*max(fixing-K,0)))` por su cuenta | `02` y `09` tienen cada uno su propia copia casi identica de estos dos helpers | Riesgo de duplicacion/inconsistencia (de signo, de redondeo) si un tercer notebook los reinventa de nuevo con un criterio distinto |
| 9 | Asimetria ya senalada en la Fase 4 de `PLAN_IMPROVE_NOTEBOOK.md` (`engine.MeasureResult` no expone `bump_used`, a diferencia de `engine.GreekResult`) -- alli quedo "fuera de alcance", pero ahora hay un caso de uso real que la rodea | `09` tuvo que leer `theta` desde `report.greeks` de `all_greeks` (que SI expone `bump_used`) en vez de desde `price()` suelto con `greeks.theta(...)`, para poder anualizar sin duplicar el numero magico `1/365` | Sigue viva la duplicacion de `DEFAULT_THETA_BUMP` (Python) vs `default_time_shift_bump()` (C++) que la Fase 4 original dejo pendiente de cerrar del todo |

(Numeradas 0-9 por orden de impacto/severidad -- 0 es un bug de correccion silenciosa, el resto son
gaps de cobertura o de rendimiento -- no por orden de aparicion en los notebooks.)

## 2. Plan por fases

### Fase 0 — Cerrar el bug silencioso de `pricing_date` ignorado en pathwise

**Problema.** `greeks.cpp::try_pathwise` (linea ~527) llama a `payoff::payoff_sensitivity_gbm`/`_gbm_p`
pasando unicamente `n_paths`/`seed` del `PricingContext` recibido -- nunca `pricing.pricing_date()`.
Para GBM/GBM_P + `PayoffPriceQ` + `risk_factor` de modelo (spot/volatility/rate/dividend_yield),
`method="auto"` (el default) siempre resuelve a esta ruta si la combinacion esta en
`pathwise_capabilities()` y el contrato no tiene `Exercise` -- asi que CUALQUIER llamada a
`greeks.delta/vega/rho(...)` sobre un `PricingContext` con `pricing_date != 0` devuelve el valor
como si `pricing_date` fuera `0`, sin lanzar, sin loguear, sin marcarlo en `method_used`. Se
descubrio porque el charm de `09` (diferencia finita de delta entre dos `pricing_date`) salia
identicamente `0.0` en las 14 estrategias.

**Tareas de motor.**
- Decision de diseno explicita antes de tocar nada (documentar como ADR corto, mismo criterio que
  `ADR-IN-01` de `PLAN_IMPROVE_NOTEBOOK.md` Fase 4): dos caminos posibles --
  1. **Propagar `valuation_time`** a `payoff::payoff_sensitivity_gbm`/`_gbm_p` (Rust,
     `rust/crates/engine-core/src/payoff/sensitivity.rs`) igual que ya hace
     `simulate_gbm_columns`/la ruta de Theta (`payoff::api::risk_neutral_price_gbm` con
     `valuation_time`, ver `PLAN_GREEKS.md §7.2/Fase 5`): pathwise pasaria a honrar `pricing_date`
     de verdad, con el mismo `Err` explicito si algun `required_time` ya paso. Mas trabajo (toca
     Rust + el bridge cxx + la firma de `payoff_sensitivity_gbm_q` en `engine-ffi`), pero cierra la
     limitacion de raiz en vez de solo detectarla.
  2. **Rechazar pathwise si `pricing_date != 0`** en `try_pathwise`/`compute_greek` (C++, un
     `if (pricing.pricing_date() != 0.0) return std::nullopt;` al principio de `try_pathwise`):
     bajo `method="auto"` cae en silencio a `bump_and_reval` (que SI reconstruye `PricingContext`
     con el `pricing_date` pedido, via el mismo mecanismo que ya usa Theta) -- mas simple, cero
     cambios en Rust, pero pathwise deja de aplicarse (mas lento/mas ruido MC) para cualquier
     Greek pedida desde una fecha de valoracion futura, no solo para charm.
  Este documento no decide -- lo decide quien implemente, con las dos rutas comparadas y una
  justificacion corta.
- Si se elige la opcion 2 (o incluso si se elige la 1, como salvaguarda): que `GreekResult`/
  `MeasureResult` reflejen SIEMPRE el `method_used` real que se aplico, y que un `method="pathwise"`
  EXPLICITO (no `"auto"`) sobre un `pricing_date != 0` lance un error claro en vez de devolver un
  numero incorrecto en silencio -- mismo criterio que ya usa el motor para "no aplica" en vez de
  aproximar (`PLAN_GREEKS.md §7.4`).

**Tareas de notebook.** `09_option_strategies_and_greeks.ipynb`: una vez cerrada la limitacion,
simplificar el calculo de charm (hoy fuerza `method="bump_and_reval"` a mano en dos llamadas) para
usar `method="auto"` si la opcion 1 se implementa (pathwise ya seria correcto con `pricing_date`
desplazado) -- si se elige la opcion 2, el notebook ya esta bien tal cual (fuerza bump_and_reval),
solo hay que quitar el comentario que dice que es un workaround y confirmar que sigue siendo
necesario por diseno, no por bug.

**Criterio de aceptacion.** Un test nuevo (`cpp/engine/tests/test_greeks.cpp` o
`clients/python/tests/`) que pida `greeks.delta("PayoffPriceQ","spot")` sobre un `PricingContext`
con `pricing_date > 0` y verifique que el resultado NO coincide con el mismo calculo a
`pricing_date=0` (para un contrato/modelo donde de verdad deberian diferir) -- hoy ese test fallaria
(ambos saldrian iguales). Documentado en `method_used`/error explicito segun la opcion elegida.

### Fase 1 — Evaluacion determinista de un contrato expuesta a Python (`Engine.evaluate_scenario`)

**Problema.** `cpp/engine/include/engine/payoff/scenario_evaluator.hpp::ScenarioEvaluator` (y el
`market_snapshot_bridge` que ya usa `PresentValueMeasure` para valorar un `PayoffProduct`
deterministamente, sin modelo) no tienen NINGUN binding en `clients/python/src/engine_py_ext.cpp`
(confirmado por grep: cero coincidencias). Para dibujar el payoff intrinseco a vencimiento de
cualquier estrategia, `09_option_strategies_and_greeks.ipynb::intrinsic_value(legs, spot_grid)`
reimplementa `max(S-K,0)`/`max(K-S,0)` a mano en NumPy, leg a leg, en vez de pedirle al motor que
evalue el contrato compilado sobre un escenario de spot fijo -- exactamente el mismo patron de
riesgo que motivo la Fase 0 de `PLAN_IMPROVE_NOTEBOOK.md` (simulate_paths), aqui para la rama
determinista/sin-modelo.

**Tareas de motor.**
- Verificar primero si el camino correcto es `ScenarioEvaluator` (AST C++ nativo, via
  `market_snapshot_bridge`) o si conviene exponer en su lugar una evaluacion determinista sobre el
  `CompiledPayoff` de Rust (un solo "path" con `n_paths=1` y valores de observable fijados a mano
  en vez de simulados) -- comparar las dos rutas antes de decidir, mismo criterio de "decision de
  diseno explicita" que otras fases de este documento y del anterior.
- C++: `Engine::evaluate_scenario(product, scenario) -> CashflowLedger` (o el nombre que se decida),
  NO una `IMeasure` (es diagnostico/utilidad de autoria, mismo criterio que `simulate_paths` en la
  Fase 0 del plan anterior: "no ensuciar `Registry<IMeasure>` con algo que no admite
  `price_batch`/`price_many`/`price_grid`"). `scenario` como minimo necesita spot(s) por
  observable en la(s) fecha(s) que el contrato requiera (`required_times`/`generated_observables`).
- Bindings Python: devolver algo utilizable directo en NumPy (p.ej. lista de cashflows
  `(time, currency, amount)`, o ya sumado si el contrato es de una sola moneda -- igual que hace
  `PresentValueMeasure` hoy internamente).

**Tareas de notebook.** `09_option_strategies_and_greeks.ipynb`: sustituir `intrinsic_value(...)`
por una llamada vectorizada a `Engine.evaluate_scenario` sobre el grid de spot (o, si el binding
solo acepta un escenario a la vez, un bucle Python que igual delega el calculo real al motor);
mantener la formula NumPy como celda de verificacion cruzada, no como unico camino (mismo patron
que ya exige `PLAN_IMPROVE_NOTEBOOK.md` para sus propias fases 0/2/5). Aplica tambien a
`02_exotic_and_path_dependent_options.ipynb` si alguna celda todavia calcula un payoff intrinseco a
mano.

**Criterio de aceptacion.** `Engine.evaluate_scenario(...)` da, para las 14 estrategias de `09`
sobre el mismo grid de spot, un resultado identico (no solo "dentro de tolerancia": es un calculo
determinista) al que hoy produce `intrinsic_value(...)` en NumPy.

### Fase 2 — Hessiana con fallback bump-and-reval para contratos multi-fecha

**Problema.** `Engine.hessian` (likelihood-ratio, una unica pasada Monte Carlo) exige que el
contrato dependa de una unica fecha terminal (`payoff_supports_second_order_lrm[_p]`, mismo criterio
que excluye `ContractOp::Exercise`). Para cualquier contrato que no cumpla eso -- hoy, en la
practica, los calendar spreads de `09` -- las entradas correspondientes simplemente NO aparecen en
`HessianReport::entries`, sin ningun mensaje ni entrada en algo equivalente a `skipped` (que si
existe para `GreeksReport`, Fase 1 original de `PLAN_GREEKS.md`). El notebook `09` tuvo que
documentar a mano, en una celda markdown, por que gamma/vanna/volga salen `NaN` para esos dos
productos.

**Tareas de motor.**
- `cpp/engine/src/greeks.cpp::compute_hessian`: cuando la ruta LRM no aplica (contrato multi-fecha,
  o cualquier otro motivo de los ya excluidos), en vez de omitir la entrada en silencio, o (a)
  caer a un estencil bump-and-reval generico de segundo orden (mas caro: 4 revaluaciones por
  entrada cruzada, pero generico -- ya existe el mecanismo de bump-and-reval de primer orden, hay
  que extenderlo a segundo orden/cruzado para el caso generico) o (b) devolver la entrada en un
  `HessianReport::skipped` (mismo patron que `GreeksReport::skipped`) con el motivo exacto en vez
  de solo omitirla. Documentar la decision (si (a), el rendimiento cambia; si (b), sigue faltando
  gamma/vanna/volga para multi-fecha pero al menos queda explicito, no hay que adivinarlo desde
  Python). Dado el criterio de "nunca aproximar en silencio" que ya seguia el motor para Theta, (b)
  como minimo es obligatorio aunque no se implemente (a) en esta fase.

**Tareas de notebook.** `09_option_strategies_and_greeks.ipynb`: si se implementa (a), quitar el
`.get(..., np.nan)` defensivo y la nota markdown sobre el `NaN` (ya no aplicaria); si solo se
implementa (b), simplificar la nota markdown para citar el motivo real que ahora reporta el motor
(`HessianReport.skipped`) en vez de una explicacion inferida a mano por quien escribio el notebook.

**Criterio de aceptacion.** `Engine.hessian` nunca deja una entrada pedida completamente ausente sin
explicacion: o la calcula (via el fallback generico) o aparece en `skipped` con el motivo. Test
nuevo en `cpp/engine/tests/` sobre un contrato multi-fecha (p.ej. un calendar spread minimo)
verificando uno de los dos comportamientos segun lo que se decida.

### Fase 3 — Informe de riesgo de una sola pasada Monte Carlo (precio + Greeks)

**Problema.** Ni `Engine.price` con varias `Greek` en la misma llamada (colisionan bajo la misma
clave `"Greek"` en el dict de salida) ni `Engine.all_greeks` (que enumera automaticamente, pero
dispara una simulacion Monte Carlo independiente POR CADA factor de riesgo candidato, confirmado
leyendo `compute_all_greeks`) comparten una unica pasada de simulacion entre distintas Greeks de un
mismo contrato/modelo/mercado -- salvo la Hessiana LRM, que si comparte una pasada pero solo para
gamma+vanna+volga de un par de factores concreto. `09_option_strategies_and_greeks.ipynb` paga esto
directamente: cada punto `(estrategia, vol, spot)` dispara ~5 llamadas de motor independientes
(`all_greeks`, `hessian`, ademas de las dos llamadas extra de la Fase 0 de aqui arriba para charm),
cada una su propia simulacion, para calcular cantidades que en principio podrian compartir las
mismas trayectorias.

**Tareas de motor.**
- Decision de diseno: (a) un metodo nuevo `Engine.risk_report(product, metric_name, model, market,
  pricing, execution, ...)` que, en una sola pasada Monte Carlo (mismo seed/trayectorias), calcule
  precio + delta + vega + gamma + vanna + volga + theta (y charm, si la Fase 0 de aqui arriba lo deja
  disponible por el camino generico) reutilizando entre si los pathwise/LRM que ya existen
  internamente en vez de volver a simular; o (b), mas modesto, permitir que `Engine.price`/
  `price_grid` acepten un ALIAS por entrada de medida (p.ej. tupla de 3
  `(nombre, params, alias)`) para que varias `Greek` puedan convivir en el mismo dict de salida sin
  colisionar -- no comparte simulacion, pero al menos permite pedirlas en una sola llamada de
  Python (menos round-trips del lado cliente, aunque el motor siga simulando N veces por dentro).
  (a) es la mejora real de rendimiento; (b) es un quick win de ergonomia de API que no la resuelve
  de raiz. Documentar cual(es) se implementan y por que.

**Tareas de notebook.** `09_option_strategies_and_greeks.ipynb::compute_greeks_grid`: si se
implementa (a), sustituir las ~5 llamadas actuales por una sola a `Engine.risk_report(...)` por
punto del grid -- deberia reducir el tiempo total de ejecucion del notebook de forma medible (el
propio notebook puede llevar un `%%time`/medicion antes/despues como evidencia). Si solo se
implementa (b), simplificar el codigo de `compute_greeks_grid` para pedir varias Greeks en una
llamada aunque el tiempo de ejecucion no mejore tanto.

**Criterio de aceptacion.** Con (a): el tiempo total de ejecucion de `09` baja de forma medible
(reportar el numero) sin cambiar ningun valor de las Greeks (test de paridad exacta contra el
camino actual, misma seed). Con (b) solamente: el codigo de Python se simplifica, documentado como
mejora parcial (queda una Fase 3b pendiente para la version que si comparte simulacion, si en su
momento se decide que vale la pena el esfuerzo).

### Fase 4 — Extender `simulate_paths`/Greeks a `GbmBasket`

**Problema.** `Engine.simulate_paths` (Fase 0 de `PLAN_IMPROVE_NOTEBOOK.md`) solo hace dispatch a
`GbmModel`/`GbmPModel` -- se construyo antes de que existiera `GbmBasket` (Fase 3 del mismo plan,
posterior). Igualmente, `Greek`/`all_greeks`/`hessian` (`greeks.cpp`) solo reconocen `GbmModel` (via
`dynamic_cast`) para las rutas pathwise/LRM -- `GbmBasket` esta completamente fuera de la maquinaria
de Greeks, decision explicita de la Fase 3 original ("Sin `PayoffExerciseQ`/... sensibilidades...
para `GbmBasket`") pero que ahora, con `08_multi_asset_options.ipynb` ya construido, se nota como
un hueco de cobertura real: no hay forma de mostrar delta/vega POR ACTIVO de un basket, ni una
vanna cruzada real entre dos activos (`d^2V/dS_A dS_B`, distinta de la vanna spot-vol de un unico
activo que ya calcula la Hessiana existente).

**Tareas de motor.**
- `Engine.simulate_paths`: anadir la rama `GbmBasketModel` (dispatch identico al que ya existe para
  GBM/GBM_P, reutilizando `GbmBasket::simulate_at_times` de Rust que la Fase 3 original ya escribio
  vectorizado sobre N activos) -- forma de salida a decidir: `(times, paths)` con `paths` de forma
  `(n_paths, n_steps+1, n_assets)`, documentado igual de explicito que la convencion de aplanado
  que fijo la Fase 0 original.
  - Greeks de primer orden por activo (delta_A, delta_B, ...): extender `try_pathwise`/
  `pathwise_capabilities` (o el bump-and-reval generico, si pathwise no aplica limpiamente a un
  basket) para reconocer `GbmBasketModel` y enumerar un `risk_factor` por activo (p.ej.
  `"model.spot_0"`/`"model.spot_1"`, mismo criterio de nombrado que ya usa
  `resolve_model_parameter_key`).
  - Cross-gamma entre activos (`d^2V/dS_i dS_j`, la "vanna de basket" real, distinta de la vanna
  spot-vol de un unico activo): extender `Engine.hessian`/LRM o, si no es viable en esta fase,
  documentarlo expresamente como excluido con una razon tecnica (mismo criterio de "decision
  explicita, no silencio" del resto de este documento).

**Tareas de notebook.** `08_multi_asset_options.ipynb`: nueva seccion con delta por activo de un
basket call (y, si se implementa, la cross-gamma entre los dos activos, comparando su signo con la
intuicion: sube con la correlacion para un basket, exactamente igual que ya se razono el precio en
la Fase 3 original).

**Criterio de aceptacion.** Un basket de 2 activos tiene delta por activo alcanzable via
`greeks.delta(...)`, verificado contra bump-and-reval manual (mismo criterio de paridad que el resto
de Greeks del motor); `simulate_paths` funciona sobre `GbmBasket` con un test de paridad de momentos
igual que ya exige el criterio de aceptacion de la Fase 0 original para GBM/GBM_P.

### Fase 5 — Cerrar la asimetria `bump_used` entre `GreekResult` y `MeasureResult`

**Problema.** `engine.GreekResult` (lo que devuelve `Engine.all_greeks`/`Engine.hessian`) expone
`bump_used`; `engine.MeasureResult` (lo que devuelve `Engine.price(...)["Greek"]`, el camino que usa
directamente `greeks.theta(...).to_spec()`) no. La Fase 4 de `PLAN_IMPROVE_NOTEBOOK.md` ya senalo
esto y dejo `engine_typed.greeks.DEFAULT_THETA_BUMP` como una constante Python que DEBE coincidir a
mano con `default_time_shift_bump()` (C++) -- "fuera de alcance" en su momento porque tocaba el
bridge. `09_option_strategies_and_greeks.ipynb` termino evitando el problema del todo leyendo theta
desde `report.greeks` de `all_greeks` (que si expone `bump_used`) en vez de desde `price()` suelto
-- funciona, pero es un rodeo: cualquiera que solo quiera pedir "theta" sin las demas Greeks sigue
atado a la constante duplicada.

**Tareas de motor.**
- `cpp/engine/include/engine/price.hpp`/`measure.hpp` (`MeasureResult`): anadir un campo opcional
  `bump_used` (mismo patron que `GreekResult`), poblado cuando la medida evaluada es `"Greek"`
  (el resto de medidas lo dejan vacio/`std::nullopt`, no aplica).
- `clients/python/src/engine_py_ext.cpp`: exponer el campo nuevo en el binding de `MeasureResult`.
- `engine_typed/greeks.py`: `ThetaGreek.annualize(...)` puede entonces leer `bump_used` del propio
  `MeasureResult` en vez de depender de `DEFAULT_THETA_BUMP` -- eliminar la constante duplicada y el
  comentario que documenta el acoplamiento cruzado Python/C++ (ya no aplicaria).

**Tareas de notebook.** `01_vanilla_options_black_scholes.ipynb` §6 y
`09_option_strategies_and_greeks.ipynb`: simplificar cualquier sitio que hoy calcule/asuma
`DEFAULT_THETA_BUMP`/`bump_used` a mano.

**Criterio de aceptacion.** `MeasureResult.bump_used` existe y coincide exactamente con el `bump`
efectivo que uso el motor para una `Greek` con `bump=None` (resuelto internamente); la constante
`DEFAULT_THETA_BUMP` en Python deja de ser necesaria para `annualized=True` con `bump=None` (sigue
pudiendo existir como conveniencia, pero ya no es la UNICA fuente de verdad).

### Fase 6 — Builders de estrategia reutilizables en `engine_typed.payoff`

**Problema.** No hay `call_leg`/`put_leg`/combinador de estrategia en `engine_typed.payoff` --
`02_exotic_and_path_dependent_options.ipynb` (straddle/strangle) y
`09_option_strategies_and_greeks.ipynb` (14 estrategias) reinventan, cada uno por su lado, el mismo
patron `when(T, cashflow(qty*max(fixing-K,0)))`/`when(T, cashflow(qty*max(K-fixing,0)))`. Ningun bug
concreto hoy, pero es la misma clase de riesgo de "isla" que `PLAN_GREEKS.md §0` ya senalo para otra
funcionalidad: dos implementaciones casi identicas que pueden divergir silenciosamente (p.ej. en la
convencion de signo, o en como se combina con `Give` vs cantidad negativa).

**Tareas de motor (cliente Python, no motor C++/Rust).**
- `clients/python/src/engine_typed/payoff.py`: anadir `call_leg(observable, strike, qty, maturity)`/
  `put_leg(...)` (mismo patron que `european_call`/`european_put` ya existentes, pero devolviendo un
  `Contract` crudo en vez de un `PayoffProduct` completo, para poder combinarse via `both(...)`) y
  un `custom_strategy(id, legs)` que envuelva `both(legs)` en un `PayoffProduct`. Convencion de signo
  explicita en el docstring (qty positivo = largo/comprado, negativo = corto/vendido, ADR-P0-02, sin
  `give()`).

**Tareas de notebook.** `02_exotic_and_path_dependent_options.ipynb` y
`09_option_strategies_and_greeks.ipynb`: sustituir los helpers locales `vanilla_call_contract`/
`vanilla_put_contract`/`call_leg`/`put_leg`/`strategy`/`build_contract` propios de cada notebook por
los builders nuevos de `engine_typed.payoff`, verificando que el resultado (precio/Greeks) no
cambia frente al comportamiento actual.

**Criterio de aceptacion.** Un unico builder de pata vainilla y un unico combinador de estrategia
viven en `engine_typed.payoff`, con test Python de paridad, y los notebooks `02`/`09` los consumen
en vez de reimplementarlos.

### Fase 7 — Auditoria y actualizacion de TODA la bateria de notebooks (`01`-`09`)

**Problema.** Las fases 0-6 de arriba, en su seccion "Tareas de notebook", solo tocan el notebook
que motivo cada friccion (sobre todo `09`, a veces `08`/`02`/`01`). Pero varias de estas mejoras son
transversales -- una vez el motor las tiene, pueden simplificar o corregir celdas de notebooks que
NI SIQUIERA se mencionan arriba porque no fueron los que expusieron la friccion por primera vez
(p.ej. `03_bermudan_exercise.ipynb`/`04_greeks_and_risk_surfaces.ipynb`/
`05_physical_measure_forecasting.ipynb`/`06_exposure_cva_portfolio.ipynb`/
`07_montecarlo_paths_q_vs_p.ipynb` pueden tener sus propias celdas que se beneficiarian de
`Engine.evaluate_scenario` (Fase 1), de un informe de riesgo de una sola pasada (Fase 3), o de los
builders de estrategia (Fase 6), sin que este documento las haya revisado una a una todavia). Migrar
las fases 0-6 sin volver a pasar por el resto de la bateria dejaria el motor mejorado pero los
notebooks inconsistentes entre si -- unos usando la forma nueva, otros todavia con el workaround
viejo que la propia mejora dejo obsoleto -- exactamente el patron de "isla" que
`PLAN_GREEKS.md §0`/este mismo documento (friccion 8) ya senalan como riesgo cuando dos caminos
casi identicos conviven sin que nadie los reconcilie.

**Tareas de motor.** Ninguna -- esta fase es puramente de notebook/cliente, se ejecuta DESPUES de
que las fases 0-6 esten construidas y verificadas (no tiene sentido auditar contra una API que
todavia va a cambiar).

**Tareas de notebook.** Recorrer los NUEVE notebooks (`01` a `09`, no solo los que aparecen en la
tabla de trazabilidad de este documento) y, para cada uno:
- Grep de patrones que las fases 0-6 dejan obsoletos: calculo manual de payoff/intrinsic value en
  NumPy (Fase 1), llamadas a `Greek` con `method="auto"` sobre un `pricing_date` desplazado sin el
  workaround de bump_and_reval (Fase 0, si ya no hace falta), varias llamadas de motor separadas
  para Greeks relacionadas donde ahora existe un informe de una sola pasada o un alias (Fase 3),
  helpers locales `call_leg`/`put_leg`/`vanilla_call_contract`/combinador de estrategia (Fase 6),
  lectura de `DEFAULT_THETA_BUMP`/calculo manual de `bump_used` (Fase 5), notas markdown que
  documentan un `NaN`/limitacion que la Fase 2 o 4 ya cerraron.
- Actualizar la celda para usar la funcionalidad nueva, conservando (donde el propio notebook ya
  lo hacia) la version anterior como celda de verificacion cruzada explicita en vez de borrarla sin
  mas -- mismo patron editorial que exige `PLAN_IMPROVE_NOTEBOOK.md` para sus propias fases.
  Regenerar cada notebook tocado con
  `jupyter nbconvert --to notebook --execute --inplace <notebook>.ipynb` y confirmar 0 errores antes
  de continuar con el siguiente.
- Actualizar `clients/python/notebooks/README.md` si la descripcion de algun notebook mencionaba un
  workaround que ya no existe.

**Criterio de aceptacion.** Ningun notebook de `01` a `09` contiene, tras esta fase, un patron que
duplique a mano (en NumPy o en Python puro) algo que el motor ya sabe hacer nativamente segun las
fases 0-6 de este documento; los que documentaban una limitacion ya cerrada (`NaN` de calendar
spread, `DEFAULT_THETA_BUMP`, helpers de estrategia locales) referencian la solucion nueva en vez
del workaround. Los nueve notebooks ejecutan limpio de extremo a extremo en la misma pasada de
verificacion final.

## 3. Priorizacion sugerida

| Fase | Esfuerzo relativo | Notebooks que mejora | Bloquea |
|------|--------------------|------------------------|---------|
| 0 — bug silencioso de `pricing_date` en pathwise | bajo-medio (opcion 2) / medio-alto (opcion 1, toca Rust+bridge) | `09` (y cualquier Greek futura sobre `pricing_date != 0`) | Maxima prioridad: es un resultado incorrecto sin aviso, no solo cobertura |
| 5 — `bump_used` en `MeasureResult` | trivial (bridge + campo nuevo) | `01`, `09` | nada, quick win igual que la Fase 4 del plan anterior |
| 6 — builders de estrategia reutilizables | trivial (solo Python) | `02`, `09` | nada, quick win |
| 1 — `Engine.evaluate_scenario` | bajo-medio | `09` (y futuro) | nada |
| 2 — Hessiana con fallback multi-fecha | medio | `09` | nada |
| 4 — `simulate_paths`/Greeks para `GbmBasket` | medio | `08` | nada |
| 3 — informe de riesgo de una sola pasada | alto (opcion a) / bajo (opcion b) | `09` (mayor impacto en tiempo de ejecucion de toda la bateria) | mayor impacto en rendimiento de cualquier notebook futuro con muchas Greeks |
| 7 — auditoria de `01`-`09` | medio (nueve notebooks, pero cada cambio individual ya es de bajo esfuerzo) | los nueve | **Depende de que las fases 0-6 esten TODAS construidas** -- es el cierre del ciclo, no se hace en paralelo con ellas |

## 4. Fuera de alcance de este documento

- Ninguna fase de este documento se implementa aqui -- es un plan, mismo criterio editorial que
  `PLAN_IMPROVE_NOTEBOOK.md §0`/`PLAN_GREEKS.md §11`: cada fase se migra a `PLAN.md` solo tras
  quedar construida y verificada de extremo a extremo.
- Friccion 7 (alineacion de RNG motor vs NumPy trayectoria a trayectoria): documentada en la tabla
  de fricciones pero SIN fase propia -- expondria detalles internos del backend Burn (semilla del
  generador subyacente) fuera del contrato publico actual del motor, y el beneficio (un test de
  regresion cruzada mas estricto) no justifica ese acoplamiento hoy. Si en el futuro se necesita,
  merece su propio documento, no una fase suelta aqui.
- Rendimiento GPU, paridad Excel/C ABI: fuera de alcance, igual que en el documento anterior.
- Cobertura de producto para `GbmBasket` mas alla de lo que pide la Fase 4 de aqui (N>2 activos
  ejercitados en tests/notebook, medidas P bajo básket): se deja para un tercer documento si llega
  a hacer falta, no se fuerza aqui.

## 5. Trazabilidad: fase → notebook → celda

| Fase | Notebook | Seccion afectada hoy |
|------|----------|------------------------|
| 0 | `09_option_strategies_and_greeks.ipynb` | celda de `compute_greeks_grid`, calculo de `charm` |
| 1 | `09_option_strategies_and_greeks.ipynb` | `intrinsic_value(legs, spot_grid)` |
| 2 | `09_option_strategies_and_greeks.ipynb` | nota markdown sobre `NaN` en `long_calendar_spread`/`short_calendar_spread` |
| 3 | `09_option_strategies_and_greeks.ipynb` | `compute_greeks_grid` (bucle completo estrategia x vol x spot) |
| 4 | `08_multi_asset_options.ipynb` | seccion nueva (no existe todavia) |
| 5 | `01_vanilla_options_black_scholes.ipynb`, `09_option_strategies_and_greeks.ipynb` | uso de `DEFAULT_THETA_BUMP`/`annualized=True` |
| 6 | `02_exotic_and_path_dependent_options.ipynb`, `09_option_strategies_and_greeks.ipynb` | helpers locales `call_leg`/`put_leg`/`vanilla_call_contract` |
| 7 | `01` a `09`, TODOS | cierre: auditoria completa tras 0-6, no una celda concreta |

Cada notebook de la lista de arriba deberia llevar, tras implementar la fase correspondiente, una
nota corta apuntando a este documento en el punto exacto donde vivia el workaround -- mismo criterio
que ya siguen los notebooks `01`-`08` con `PLAN_IMPROVE_NOTEBOOK.md`. La Fase 7 es la unica que se
ejecuta al final, sobre la bateria entera, en vez de sobre el notebook puntual que motivo cada
friccion -- ver su propia seccion (§2) para el porque.
