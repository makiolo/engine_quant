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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).** El enunciado se
verificó leyendo el código real antes de tocar nada (instrucción 1 de ejecución) — con dos matices
que cambian el alcance real del bug frente a como lo describe el enunciado:

1. **El bug es más amplio que solo `try_pathwise` (línea ~527)**: exactamente el mismo patrón —
   reenviar únicamente `pricing.n_paths()`/`pricing.seed()` a Rust, nunca `pricing.pricing_date()`
   — aparece también en `try_pathwise2` (Gamma vía likelihood ratio, `payoff_sensitivity2_gbm[_p]`),
   `try_pathwise_cross` (Vanna vía likelihood ratio, `payoff_sensitivity_cross_gbm[_p]`) y
   `try_hessian_likelihood_ratio` (el Hessiano local de `Engine.hessian`,
   `payoff_local_hessian_gbm[_p]`) — las cuatro funciones de `greeks.cpp` que delegan en una ruta
   Rust de likelihood-ratio/pathwise para GBM. Las cuatro se corrigieron con el mismo criterio, no
   solo `try_pathwise`.
2. **El bug NO afecta por igual a GBM (Q) y GBM_P (P)**, al contrario de lo que sugiere el
   enunciado ("Para GBM/GBM_P + `PayoffPriceQ`..."): se verificó que el *fallback* bump-and-reval
   (`compute_greek`, estencil genérico que llama a `metric->evaluate(..., pricing, ...)`) SÍ
   reconstruye correctamente el resultado bajo `pricing_date` desplazado para GBM+`"PayoffPriceQ"`,
   porque `PayoffPriceQMeasure::evaluate` (`measure.cpp`) ya reenvía explícitamente
   `pricing.pricing_date()` a `risk_neutral_price_gbm` (cableado desde PLAN_GREEKS.md §7.2/Fase 5,
   el mismo mecanismo que ya usa Theta). Para GBM_P+`"PayoffForecastP"`, en cambio,
   `PayoffForecastPMeasure::evaluate` **nunca** usa `pricing.pricing_date()` — ni el bump-and-reval
   genérico corrige nada ahí, porque el propio `forecast_gbm_p` (Rust, `api_p.rs`) no tiene ningún
   parámetro de `valuation_time` ni concepto de "hoy" desplazable (el módulo documenta
   explícitamente que P nunca descuenta y reporta cashflows en su fecha de pago tal cual). Esto ya
   estaba señalado, aunque de forma indirecta, por `metric_supports_time_shift()` en `greeks.cpp`
   (excluye `"PayoffForecastP"` de Theta con el comentario "cualquier otra metrica lo ignora
   todavia"). **Se deja fuera de alcance de esta fase**: arreglar `pricing_date` para GBM_P
   requeriría rediseñar qué significa "hoy" bajo la medida física P (el propio doc-comment de
   `api_p.rs` dice que P se reserva para forecast/stress, no para valorar en una fecha desplazada) —
   un cambio de otro tamaño y de otro documento, no un bug de correción silenciosa aislado como el
   de GBM/Q.

**Decisión de diseño (ADR-IN2-01): Opción 2 — rechazar la especialización pathwise/likelihood-ratio
cuando `pricing.pricing_date() != 0`, en vez de propagar `valuation_time` a Rust (Opción 1).**
Justificación, con el código real ya verificado:

- El fallback correcto **ya existe y ya es correcto** para la combinación donde el bug importa
  (GBM+`"PayoffPriceQ"`): `PayoffPriceQMeasure::evaluate` ya reenvía `pricing.pricing_date()`, así
  que rechazar la especialización pathwise bajo `method="auto"` cae, sin ningún código nuevo en
  Rust/bridge cxx, en un cálculo YA verificado como correcto (el mismo estencil genérico de
  bump-and-reval que ya pasa `GreeksFase1Test`/`GreeksFase6Test`). La Opción 1 habría duplicado esa
  corrección en Rust (`sensitivity.rs`/`api.rs`, cuatro funciones: `payoff_sensitivity_pathwise_on`,
  la de Gamma, la de Vanna, la del Hessiano local) para llegar exactamente al mismo resultado
  numérico que el fallback ya da hoy.
- La Opción 1 no habría cerrado el caso GBM_P de todas formas (punto 2 de arriba: el problema ahí
  no es que pathwise ignore `pricing_date`, es que **ninguna** ruta lo soporta bajo la medida física
  P) — el ahorro de "cerrar la limitación de raíz" que prometía la Opción 1 en el enunciado no
  aplica a GBM_P sin un rediseño más grande, así que el argumento a favor de la Opción 1 pierde peso
  frente a lo que realmente se ganaría.
- Coste de la Opción 2: pathwise deja de aplicarse (más lento, más ruido Monte Carlo a igual
  `n_paths`) para cualquier Greek pedida desde una fecha de valoración futura sobre GBM/Q — aceptado
  explícitamente como el trade-off correcto dado que el criterio de máxima prioridad de esta fase es
  "nunca un resultado incorrecto sin aviso", no rendimiento.

**Trabajo realizado, por capa:**

- **C++ únicamente — sin cambios en Rust ni en el bridge cxx** (consistente con la Opción 2):
  `cpp/engine/src/greeks.cpp`:
  - `try_pathwise`, `try_pathwise2`, `try_pathwise_cross`, `try_hessian_likelihood_ratio`: cada una
    gana un `if (pricing.pricing_date() != 0.0) return std::nullopt;` (documentado inline con la
    razón exacta y referencia a esta fase), colocado tras los chequeos estructurales existentes
    (capacidad listada, `Exercise`/soporte LRM) para no cambiar el orden de los mensajes de error ya
    verificados por tests previos.
  - `pathwise_unsupported_reason`, `pathwise2_unsupported_reason`, `pathwise_cross_unsupported_reason`:
    ganan un parámetro `const PricingContext& pricing` y una rama nueva que nombra explícitamente
    `pricing_date` (valor incluido) cuando esa es la causa — así `method="pathwise"`/`"aad"`
    explícito sobre un `pricing_date != 0` lanza `std::invalid_argument` con el motivo exacto en vez
    de aproximar en silencio (satisface la segunda tarea de motor del enunciado).
  - `compute_hessian`: cuando la especialización LRM no aplica y la causa es específicamente
    `pricing_date != 0` sobre una combinación por lo demás cubierta (`hessian_capabilities()`), el
    mensaje en `HessianReport::skipped` lo nombra explícitamente en vez de caer en el mensaje
    genérico de "combinación no cubierta" — ya existía el campo `skipped` desde Fase 1 de
    PLAN_BACKWARD.md, no hizo falta añadirlo.
  - `cpp/engine/include/engine/greeks.hpp`: doc-comment de `GreekMethod` ampliado con la nueva
    exclusión de `pricing_date != 0` para las cuatro especializaciones, mismo criterio editorial que
    el resto del archivo.
  - `GreekResult::method_used`/`MeasureResult` ya reflejaban correctamente el método real aplicado
    sin cambios adicionales: al caer al fallback genérico, el código existente ya fija
    `method_used = GreekMethod::BumpAndReval` (y `bump_used`) — la tarea "que `GreekResult` refleje
    siempre el método real" ya estaba satisfecha por el propio mecanismo de fallback, no hizo falta
    tocar `GreekResult`/`MeasureResult` en sí.
- **Rust**: sin cambios (Opción 2 no los requiere). `cargo test -p engine-core --release` no se
  ejecutó porque no se tocó ningún archivo de `rust/` — mismo criterio que las instrucciones de
  ejecución piden ("solo si tocaste Rust").
- **Tests nuevos** en `cpp/engine/tests/test_greeks.cpp` (suite `GreeksImproveNotebook2Fase0Test`,
  4 tests): (1) `AutoDeltaOfPayoffPriceQFallsBackToBumpAndRevalAndMatchesBlackScholesUnderNonZeroPricingDate`
  — verifica que a `pricing_date=0` la ruta sigue siendo `Pathwise` (baseline sin regresión), que a
  `pricing_date=0.4` cae a `BumpAndReval`, que el valor coincide con Black-Scholes usando la
  madurez remanente correcta (`T - pricing_date`), y que difiere de forma económicamente
  significativa del valor a `pricing_date=0` (el criterio de aceptación exacto del enunciado); (2)
  `ExplicitPathwiseMethodRejectsANonZeroPricingDateWithAnExplicitError` — `method="pathwise"`
  explícito lanza `std::invalid_argument` mencionando `pricing_date`; (3)
  `AutoGammaFallsBackToTheGenericBumpAndRevalStencilUnderNonZeroPricingDate` — mismo criterio que
  (1) pero para Gamma (`try_pathwise2`); (4)
  `ComputeHessianSkipsWithAnExplicitPricingDateReasonInsteadOfAWrongLikelihoodRatioValue` — verifica
  que `Engine.hessian` no devuelve un Hessiano LRM incorrecto bajo `pricing_date != 0`, sino que cae
  a `skipped` nombrando `pricing_date` explícitamente.
- **Verificación en capas** (todas en verde antes de continuar a la siguiente, ninguna se saltó):
  - C++: `cmd.exe /C "vcvars64.bat && cmake --build build --config Release"` compiló limpio
    (incremental: solo recompilaron `greeks.cpp`, `test_greeks.cpp` y sus dependientes). `ctest
    --test-dir build -C Release`: **480/480** (476 previos + 4 nuevos de esta fase), incluyendo los
    4 tests nuevos ejecutados de forma aislada con `-R ImproveNotebook2Fase0` para confirmar que
    corren y pasan (no solo que no rompen nada).
  - Python: `pytest clients/python/tests`: **155 passed** + los mismos 2 errores preexistentes de
    fixture `abi_dll_path` (no relacionados, no tocados) — sin cambios frente a la línea base, como
    se esperaba (esta fase no tocó ningún binding Python; se copió el `.pyd`/`.py` recién
    compilados al `venv` solo para que la verificación reflejara el binario nuevo, no como cambio
    de producto).
  - Notebook: `jupyter nbconvert --to notebook --execute --inplace
    --ExecutePreprocessor.record_timing=False clients/python/notebooks/09_option_strategies_and_greeks.ipynb`
    ejecutó de punta a punta sin errores (0 celdas con `output_type == "error"`); los valores
    observados de charm/delta/etc. en las celdas de resumen no cambiaron frente a la versión previa
    del notebook (el camino que el notebook ya usaba para charm, `method="bump_and_reval"`
    explícito, no cambió de comportamiento con esta fase — solo cambió qué pasa bajo `method="auto"`,
    que el notebook no usaba para charm).

**Cambios de notebook (Opción 2 — "el notebook ya está bien tal cual").** Como predecía el propio
enunciado para la Opción 2, no hizo falta cambiar ninguna llamada de
`09_option_strategies_and_greeks.ipynb::compute_greeks_grid` — seguía siendo necesario pedir charm
con `method="bump_and_reval"` explícito, no por el bug (ya cerrado) sino porque
`RiskFactorKind::TimeShift` nunca estuvo cubierto por ninguna especialización pathwise/
likelihood-ratio (razón estructural distinta, no relacionada con esta fase). Se reescribió el
párrafo markdown ("**Detalle importante, no anticipado...**" → "**Nota (cerrado en
PLAN_IMPROVE_NOTEBOOK2.md Fase 0)**") y el comentario de código inmediatamente anterior a las dos
llamadas de charm, ambos para dejar explícito que `method="bump_and_reval"` es una elección de
diseño confirmada, no un workaround pendiente de que el motor se arregle.
`clients/python/notebooks/README.md` (entrada de `09`) actualizado con el mismo criterio.

**Limitaciones/decisiones que las fases siguientes deben conocer:**

- **Para la Fase 3** (informe de riesgo de una sola pasada, que también toca `try_pathwise`/
  `compute_greek`): el orquestador especuló que la Opción 1 "cerraría la limitación de raíz" y
  beneficiaría a la Fase 3 dejando que pathwise siguiera aplicando incluso con `pricing_date`
  desplazado. Con la Opción 2 elegida, **pathwise NO aplica bajo `pricing_date != 0`** — cualquier
  diseño de Fase 3 que quiera compartir una única pasada Monte Carlo para precio+Greeks debe asumir
  que, si el informe de riesgo se pide con `pricing_date != 0` (p.ej. para un charm/theta futuro
  integrado ahí), la ruta compartida tendrá que ser bump-and-reval (o LRM solo cuando
  `pricing_date == 0`), no pathwise incondicionalmente. Esto es un dato de diseño, no un bloqueo.
- **Para la Fase 2** (Hessiano con fallback bump-and-reval para contratos multi-fecha): esta fase
  reutilizó el campo `HessianReport::skipped` ya existente (no lo creó) para nombrar la causa
  `pricing_date != 0` de forma explícita cuando aplica. Fase 2 seguirá necesitando su propio
  fallback bump-and-reval de segundo orden para el caso multi-fecha (sin relación con
  `pricing_date`) — **no** existe todavía un fallback bump-and-reval de segundo orden genérico que
  Fase 0 pudiera reutilizar para el caso `pricing_date != 0` del Hessiano LRM (por eso ese caso cae
  a `skipped`, no a un número calculado); si Fase 2 construye ese fallback genérico, valdría la pena
  revisar si también puede cubrir `pricing_date != 0` para el Hessiano, cerrando esa entrada de
  `skipped` con un número real en vez de con una excusa.
- **GBM_P (`PayoffForecastP`) sigue ignorando `pricing_date` bajo cualquier método**, no solo
  pathwise — limitación preexistente y ya parcialmente documentada (`metric_supports_time_shift()`),
  confirmada pero **no cerrada** por esta fase (ver el punto 2 de "Estado verificado" arriba). Si
  algún notebook/fase futura necesita desplazar `pricing_date` bajo P, hace falta una fase propia
  que decida qué significa "hoy" para GBM_P antes de tocar código.
- Los mensajes de error nuevos (`pathwise_unsupported_reason` y compañía) verifican `pricing_date`
  DESPUÉS de los chequeos estructurales existentes (capacidad, `Exercise`, soporte LRM) — si dos
  condiciones fallan a la vez (p.ej. un contrato con `Exercise` Y `pricing_date != 0`), el mensaje
  reportado es el estructural, no el de `pricing_date`. Se documenta como comportamiento esperado
  (mismo orden que ya usaban las funciones `try_*` correspondientes), no un bug.

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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).** El enunciado
se verificó contra el código real antes de tocar nada: `grep` confirmó cero coincidencias de
`ScenarioEvaluator`/`market_snapshot_bridge` en `engine_py_ext.cpp`, y la lectura de
`market_snapshot_bridge.cpp` reveló un matiz no anticipado por el enunciado: ese puente (el que
ya usa `PresentValueMeasure` para `PayoffProduct`) NO soporta ningún `Fixing`/`Current` -- solo
resuelve el schedule de cashflows fijos de un contrato tipo IRS (evalúa el ledger sobre un
`MarketPath` vacío solo para *descubrir la moneda*, nunca para fijar un spot). Las 14 estrategias
de `09` SÍ dependen de `Fixing(observable, maturity)`, así que `PresentValueMeasure` tal cual no
sirve para este caso -- había que ir un nivel más abajo, directo a `ScenarioEvaluator` +
`MarketPath::set_fixing`, no reutilizar el puente existente sin modificarlo.

**Decisión de diseño: `ScenarioEvaluator` nativo C++, NO evaluación sobre `CompiledPayoff` de
Rust con `n_paths=1`.** Comparación real hecha antes de decidir (instrucción 1 de ejecución):
`ScenarioEvaluator` (`cpp/engine/include/engine/payoff/scenario_evaluator.hpp`) ya declara
explícitamente en su propio doc-comment que "interpreta el contrato sobre una única ruta
conocida... no descuenta, no decide Q/P -- solo ejecuta el programa" (PLAN_PRODUCTS.md §5.1/§5.2)
-- es literalmente la pieza que este caso de uso necesita, ya construida, ya probada
(`cpp/engine/tests/payoff/test_scenario_evaluator_vanilla.cpp`), operando directamente sobre el
`ContractPtr` que `PayoffProduct::payoff_program()->contract` ya expone. La ruta Rust
(`CompiledPayoff` con `n_paths=1`) habría exigido: (a) compilar el AST C++ a la representación
Rust (`payoff::compile`) -- un paso que hoy solo ocurre dentro de las medidas Q/P vía el bridge
cxx, nunca expuesto sin pasar por un modelo; (b) fingir una "simulación" Monte Carlo de un único
path para un cálculo que por definición no tiene componente aleatoria; (c) tocar Rust +
`engine-ffi` + el bridge cxx para exponer algo que el propio C++ ya resuelve sin cruzar el FFI.
Cero beneficio a cambio de mucho más código -- se descartó sin ambigüedad.

**Diseño de `scenario`: `dict {observable: spot}`, sin fecha explícita.** El spot dado se
registra en TODAS las fechas de `DependencyReport::fixing_dates` para ese observable (via
`DependencyVisitor`, ya existente, reutilizado sin cambios) -- deliberadamente MÁS simple que
`{(observable, time): spot}` porque ninguna de las 14 estrategias de `09` lo necesita (todas usan
un único observable; las de calendario usan el mismo observable en dos fechas distintas, y
`intrinsic_value` en NumPy YA asumía el mismo spot en ambas patas al indexar `spot_grid[i]` sin
importar la pata). Verificado con un test manual: un calendar spread con patas en `T=0.5` y
`T=1.5` sobre `spot=110.0` da `[(0.5, 'USD', -10.0), (1.5, 'USD', 10.0)]`, exactamente lo
esperado. Un observable requerido ausente de `scenario` lanza el `EvaluationError` nativo de
`ScenarioEvaluator` (mensaje `"fixing ausente para observable '...'"`), nunca `0.0` silencioso.

**Trabajo realizado, por capa:**

- **Rust: sin cambios** (Opción elegida no los requiere). `cargo test -p engine-core --release`
  no se ejecutó, mismo criterio que las fases anteriores ("solo si tocaste Rust").
- **C++ (bridge nanobind, NO el core `cpp/engine/src`)**: toda la lógica nueva vive en
  `clients/python/src/engine_py_ext.cpp` (mismo patrón que `Engine::simulate_paths`, que
  tampoco vive en `engine.hpp`/`engine.cpp`, según su propio comentario: "el dispatch... vive en
  `Engine::simulate_paths` (Python, `clients/python/src/engine_py_ext.cpp`), NO aqui"):
  - Nuevos `#include` de `engine/payoff/{dependency_visitor,evaluation_context,market_path,
    scenario_evaluator}.hpp` y `<nanobind/stl/tuple.h>`.
  - `Engine::evaluate_scenario(const engine::IProduct&, const nb::dict& scenario) ->
    std::vector<std::tuple<double, std::string, double>>`: valida `dynamic_cast<const
    payoff::PayoffProduct*>` (mismo criterio de "producto no soportado" explícito que
    `PresentValueMeasure`/`simulate_paths`), corre `DependencyVisitor` sobre el contrato, puebla
    un `MarketPath` con `set_fixing(observable, t, spot)` para cada fecha requerida de cada
    observable presente en `scenario`, y llama a `ScenarioEvaluator().evaluate(root, context)`
    con `FixingStore`/`RuntimeState` vacíos (mismo patrón que
    `market_snapshot_bridge.cpp::evaluate_ledger_currency_probe`). Devuelve el `CashflowLedger`
    crudo como `(time, currency, amount)` -- SIN descontar, a propósito.
  - Binding en `NB_MODULE` inmediatamente después de `simulate_paths`, mismo estilo de docstring
    extenso con ejemplo `>>>`.
- **Tests nuevos** (`clients/python/tests/test_engine_evaluate_scenario.py`, 16 tests): las 14
  estrategias de `09` reproducidas letra a letra (mismos `kind/strike/qty/maturity` que
  `STRATEGY_LEGS`, duplicadas a propósito para no depender de ejecutar el notebook en CI) --
  `test_evaluate_scenario_matches_numpy_intrinsic_value_exactly` (parametrizado x14) compara
  `Engine.evaluate_scenario` contra una copia literal de `intrinsic_value` (NumPy) sobre un grid
  de 41 spots con `np.testing.assert_array_equal` (exacto, no `np.isclose`); más dos tests de
  error explícito (observable ausente del escenario; producto no-`PayoffProduct`, usando
  `"IRSwap"`).
- **Notebook** `09_option_strategies_and_greeks.ipynb`: `intrinsic_value(legs, spot_grid)` pasa a
  delegar en `Engine.evaluate_scenario` (bucle Python sobre `spot_grid`, el binding acepta un
  escenario a la vez); la fórmula NumPy original se conserva como
  `intrinsic_value_manual(legs, spot_grid)`, celda de verificación cruzada (mismo criterio
  editorial que `call_leg_manual`/`put_leg_manual`/`build_contract_manual` de Fase 6). Se añadió
  un bucle de asserts sobre las 14 estrategias (incluidas las dos de calendario, que no dibujan
  la curva de payoff intrinseco en el notebook pero sí se verifican aquí) comparando
  `intrinsic_value` vs `intrinsic_value_manual` con `np.array_equal` -- pasa para las 14. No se
  tocó `02_exotic_and_path_dependent_options.ipynb`: se revisó explícitamente (grep de
  `np.maximum`/`intrinsic` sobre el notebook) y ninguna celda calcula un payoff intrínseco a mano
  ahí, así que no aplica.
- **Verificación en capas** (todas en verde, ninguna se saltó):
  - C++: `cmd.exe /C "vcvars64.bat && cmake --build build --config Release"` compiló limpio
    (solo recompiló `engine_py_ext.cpp`/el `.pyd`, ningún archivo de `cpp/engine/src` cambió).
    `ctest --test-dir build -C Release`: **480/480** (sin cambios frente a la línea base: esta
    fase no tocó ningún test C++ ni ningún archivo de `cpp/engine/`).
  - Python: `.pyd`/`engine_typed` recién compilados copiados a mano a
    `venv/Lib/site-packages` (venv con copias STALE, según la nota de entorno). `pytest
    clients/python/tests`: **179 passed** (163 previos + 16 nuevos de este test) + los mismos 2
    errores preexistentes de fixture `abi_dll_path` (no relacionados, no tocados).
  - Notebook: `jupyter nbconvert --to notebook --execute --inplace
    --ExecutePreprocessor.record_timing=False
    clients/python/notebooks/09_option_strategies_and_greeks.ipynb` ejecutó de punta a punta sin
    errores (0 celdas `output_type == "error"`); el nuevo print de verificación cruzada confirma
    "coinciden exactamente en las 14 estrategias, grid de 41 spots"; el diff resultante contra
    HEAD anterior es de 39 líneas, contenido exclusivamente en la celda de leg builders/`PRODUCTS`
    (código nuevo + su output), ninguna otra celda cambió.

**Limitaciones/decisiones que las fases siguientes deben conocer:**

- **El diseño de `scenario` (`{observable: spot}`, sin fecha) NO distingue observables con
  fechas propias**: si un contrato futuro tiene MÁS DE UN observable, cada uno requerido solo en
  SUS PROPIAS fechas (p.ej. un basket multi-activo con fixings escalonados por activo), este
  diseño aplicaría el spot de cada observable a TODAS las fechas de `DependencyReport::
  fixing_dates` del contrato completo (unión de fechas de TODOS los observables), no solo a las
  suyas -- inofensivo hoy (fechas de más nunca usadas no generan error, `ScenarioEvaluator` solo
  falla si falta algo que SÍ necesita), pero potencialmente confuso si a alguien se le ocurriera
  fijar spots DISTINTOS por fecha para el MISMO observable (no soportado: una sola entrada por
  observable en el dict). **Relevante para Fase 2** (calendar spreads) solo en la medida en que
  Fase 2 trabaja con Hessiana/Greeks, no con `evaluate_scenario` -- no hay interacción directa,
  pero si Fase 2 (u otra) quisiera reutilizar `evaluate_scenario` para diagnosticar un contrato
  multi-fecha con Greeks a mano, esta limitación aplicaría igual. Si una fase futura necesita
  distinguir fechas por observable, extender la clave del dict a `(observable, time)`.
  Respondiendo explícitamente a la pregunta del enunciado de esta fase: **sí sirve tal cual para
  los calendar spreads de `09`/Fase 2** (un único observable en dos fechas, el caso que sí está
  cubierto) -- la limitación solo aparecería con más de un observable, que ningún calendar spread
  de este documento usa.
- `PresentValueMeasure`/`market_snapshot_bridge.hpp` siguen sin soportar `Fixing`/`Current`
  (confirmado, no una limitación nueva de esta fase) -- `evaluate_scenario` es un camino
  completamente separado (usa `ScenarioEvaluator` directamente, no pasa por el puente), así que
  esta fase no cierra ni amplía esa limitación preexistente del puente, solo la esquiva para el
  caso de uso que necesitaba.
- `Engine.evaluate_scenario` queda fuera de la C ABI/Excel a propósito (mismo criterio que
  `simulate_paths`, herramienta de notebook/diagnóstico, no una medida de producción) -- ninguna
  tarea de este documento lo pedía.

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

**Estado verificado / decisiones tomadas (sesion de implementacion de esta fase).**

- **Lectura del codigo real primero:** `HessianReport` YA tenia `skipped` desde PLAN_BACKWARD.md
  Fase 1 (`cpp/engine/include/engine/greeks.hpp`, no hubo que anadirlo), y `compute_hessian` YA
  poblaba `skipped` en el camino `!applied` -- incluido un motivo especifico para
  `pricing_date() != 0` (Fase 0 de este mismo plan, `89f0250`). Es decir, la premisa literal del
  problema ("las entradas... NO aparecen... sin ningun mensaje ni entrada en `skipped`") ya no
  describia el estado real del codigo al empezar esta fase: SI habia una entrada en `skipped` para
  un calendar spread, pero era la rama GENERICA ("(GBM, PayoffPriceQ) no esta cubierta por
  hessian_capabilities()") -- **enganosa**, porque esa combinacion SI esta cubierta (lo prueba el
  test `ComputeHessianWithEmptyFactorsReturnsGammaVolgaVannaAllViaLikelihoodRatio` con una call de
  una unica fecha); solo este CONTRATO concreto no cumple la condicion dinamica de una unica fecha
  terminal (`payoff_supports_second_order_lrm[_p]` en `false`). Ese era el gap real a cerrar.
- **Decision: (b), no (a).** Se anadio una TERCERA rama de skip en `compute_hessian` (entre la de
  `pricing_date() != 0` y la generica) que detecta especificamente "el (modelo, metrica) SI esta en
  `hessian_capabilities()`, `pricing_date()==0`, pero el contrato no soporta
  `payoff_supports_second_order_lrm[_p]`" y emite un mensaje que nombra el motivo exacto (contrato
  multi-fecha, mismo criterio estructural que excluye `ContractOp::Exercise`), sin implementar el
  fallback bump-and-reval generico de segundo orden/cruzado ((a)). Motivo: (a) es "el cambio de
  mayor alcance" segun el propio texto de la fase, y el criterio de aceptacion se satisface
  igualmente con (b) ("o la calcula... o aparece en `skipped`"). Queda "Fase 2b" explicitamente
  pendiente si en el futuro se quiere el numero real para contratos multi-fecha (costaria, como
  minimo, 3 revaluaciones extra por diagonal y 4 por cruzada, sobre la metrica completa en vez de
  reutilizar la simulacion LRM ya optimizada de una pasada).
- **Nota de Fase 0 (unificar/separar skip de `pricing_date != 0` vs multi-fecha):** se DEJAN
  SEPARADOS, como dos ramas de `skipped` distintas con mensajes propios -- son motivos
  estructuralmente distintos (uno depende de `PricingContext`, el otro de la forma del contrato) y
  unificarlos perderia precision diagnostica sin ganar nada (ninguna de las dos tiene fallback
  bump-and-reval de segundo orden todavia, asi que no hay codigo compartido que "fundir").
- **Archivos tocados:** `cpp/engine/src/greeks.cpp` (`compute_hessian`, nueva rama de skip +
  doc-comments actualizados); `cpp/engine/tests/test_greeks.cpp` (test nuevo
  `GreeksHessianTest.ComputeHessianOnACalendarSpreadGoesToSkippedWithTheMultiDateReasonNotTheGenericOne`);
  `clients/python/tests/test_engine_typed_greeks.py` (paridad Python:
  `test_hessian_on_a_calendar_spread_reports_the_multi_date_reason_in_skipped` -- no hizo falta
  tocar el binding nanobind, `HessianReport.skipped` ya estaba expuesto); `09_option_strategies_and_
  greeks.ipynb` (nota markdown de `long_calendar_spread` simplificada para citar el mecanismo real
  en vez de una explicacion inferida a mano, mas una celda de verificacion en vivo que consulta
  `HessianReport.skipped` directamente sobre el producto del calendario largo). El
  `.get(..., np.nan)` defensivo de `compute_greeks_grid` se mantiene (decision (b): sigue sin haber
  numero real que poner ahi para multi-fecha).
- **Verificacion:** C++ `ctest --test-dir build -C Release` = 481/481 (incluye el test nuevo);
  Python `pytest clients/python/tests` = 180 passed + los mismos 2 errores preexistentes de
  `abi_dll_path` (no relacionados); notebook `09` re-ejecutado de extremo a extremo sin errores.
- **Relevante para Fase 3 (siguiente en orden de prioridad):** el diseño de un "informe de riesgo de
  una sola pasada" que la Fase 3 quiera construir sobre `compute_hessian` hereda el mismo hueco que
  esta fase deja explicito: para un contrato multi-fecha, ese informe tampoco podra ofrecer
  gamma/vanna/volga sin antes resolver la Fase 2b (el fallback generico). Esta fase NO cambia la
  forma de `HessianEntry`/`HessianReport` ni el conjunto de factores que produce la especializacion
  aplicable -- solo añade texto a `skipped` -- asi que no hay ningun cambio de contrato que Fase 3/
  Fase 4/Fase 7 deban tener en cuenta al construir encima.

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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).** El enunciado se
verificó literal contra el código real (instrucción 1 de ejecución) antes de tocar nada: el único
sitio que traduce `greeks::GreekResult` (que SÍ lleva `bump_used`, poblado por `compute_greek`) a
`engine::MeasureResult` (que NO lo llevaba) es `GreekMeasure::evaluate` en
`cpp/engine/src/greeks.cpp` (línea ~1544) -- confirma exactamente el "camino que usa directamente
`greeks.theta(...).to_spec()`" que describe el enunciado. `PayoffPriceQMeasure`/`PresentValueMeasure`
y el resto de medidas de `measure.hpp` construyen su propio `MeasureResult` sin pasar nunca por
`GreekResult`, así que dejan `bump_used` en `std::nullopt` por default de agregado -- sin código
adicional, exactamente el "no aplica" documentado.

**Decisión de diseño: `ThetaGreek.annualize()` pasa a recibir el `MeasureResult` completo, no solo
`.scalar`.** El enunciado no lo especifica explícitamente, pero es la única forma de que
`annualize()` pueda leer `bump_used` sin que el llamante tenga que pasarlo por separado a mano (lo
que habría reintroducido el mismo acoplamiento que se está cerrando, solo que en el sitio de
llamada en vez de en el módulo). Cambia la firma pública (`annualize(raw_value: float)` ->
`annualize(measure_result)`) -- cambio de ruptura deliberado, documentado en el docstring de la
clase; los dos únicos consumidores dentro del repo (`01_vanilla_options_black_scholes.ipynb` §6 y
`test_engine_typed_greeks.py`) se actualizaron en el mismo commit. `DEFAULT_THETA_BUMP` se retiró
por completo (no se dejó como "conveniencia" opcional, ya que ningún sitio del repo lo necesitaba
tras el cambio) -- si una fase futura necesita un valor de bump por defecto conocido de antemano
(antes de llamar al motor), puede releerse añadiéndolo de nuevo, pero no había ningún consumidor
real que lo justificara ahora.

**Trabajo realizado, por capa:**

- **C++ (motor)**:
  - `cpp/engine/include/engine/measure.hpp`: `MeasureResult` gana `std::optional<double> bump_used`
    (mismo patrón que `GreekResult::bump_used`), con doc-comment explicando cuándo se puebla (solo
    "Greek") y cuándo no (el resto de medidas, deliberado, no un descuido).
  - `cpp/engine/src/greeks.cpp`: `GreekMeasure::evaluate` copia `out.bump_used` al
    `MeasureResult` que devuelve (una línea, mismo patrón que ya copiaba
    `has_scalar`/`scalar`/`times`/`primary`/`secondary`).
  - **C ABI (`abi.h`/`abi.cpp`) deliberadamente NO tocada**: `EngineMeasureResult` (el struct
    plano de la C ABI, usado por `engine_abi_price`/Excel) no gana un campo `bump_used` -- el plan
    solo pide `price.hpp`/`measure.hpp` y `engine_py_ext.cpp`. `EngineGreekResultEntry` (la fila de
    `engine_abi_all_greeks`) ya tenía su propio `has_bump`/`bump_used` construido aparte, sin pasar
    por `EngineMeasureResult`, así que Excel/C ya podían leer `bump_used` para `all_greeks` -- lo
    único que sigue sin poder es leerlo para un "Greek" suelto vía `engine_abi_price`, igual que
    Python antes de esta fase. **Limitación conocida, documentada aquí para la Fase 7 (auditoría de
    notebooks) y cualquier trabajo futuro sobre el cliente Excel**: si algún notebook Excel o test
    de paridad C ABI necesita `bump_used` de un "Greek" vía `price()`, hace falta una fase aparte
    que añada el campo a `EngineMeasureResult`/`export_calc_result` (abi.cpp) -- no incluida aquí
    por no estar en el alcance que pidió el enunciado.
  - Test nuevo en `GreeksFase5Test.GreekMeasureReachesTimeThetaThroughEnginePrice`
    (`test_greeks.cpp`): confirma `bump_used.has_value()` para theta vía "Greek", y añade una
    segunda aserción (`PayoffPriceQ` suelto) confirmando que una medida no-"Greek" deja `bump_used`
    vacío -- ambos lados del criterio de aceptación, mismo test, sin crear un `TEST` nuevo (se
    amplió el existente porque ya montaba el fixture correcto).
- **Python (bridge nanobind)**: `clients/python/src/engine_py_ext.cpp` -- `MeasureResult` gana
  `.def_ro("bump_used", ...)`, mismo patrón que la línea equivalente de `GreekResult` unas filas
  más abajo en el mismo archivo.
- **Python (`engine_typed`)**: `clients/python/src/engine_typed/greeks.py` --
  - `DEFAULT_THETA_BUMP` eliminada, junto con el comentario de acoplamiento cruzado Python/C++ que
    la documentaba (ya no aplica: el bug estructural que motivaba la constante -- `MeasureResult`
    sin `bump_used` -- está cerrado).
  - `theta()`: ya NO resuelve `bump=None` a `DEFAULT_THETA_BUMP` cuando `annualized=True` --
    `bump=None` se deja pasar tal cual al spec en los dos casos (`annualized` True o False), el
    motor resuelve su propio default interno y lo reporta de vuelta en `bump_used`.
  - `ThetaGreek.annualize(measure_result)`: firma cambiada (recibía `raw_value: float`, ahora
    recibe el `MeasureResult` completo) -- lee `.scalar` para el caso `annualized=False`
    (paso-through) y `.scalar / .bump_used` para `annualized=True`, con un `assert` explícito
    (mensaje nombrando la causa) si `bump_used` faltara -- no debería ocurrir nunca para
    `risk_factor="time.theta"` porque `TimeShift` no tiene ruta AAD/pathwise, pero se prefirió un
    assert con mensaje a un `KeyError`/`AttributeError` opaco si algún día se rompiera esa
    invariante.
- **Tests Python** (`clients/python/tests/test_engine_typed_greeks.py`):
  - Test nuevo `test_theta_measure_result_exposes_bump_used_matching_the_effective_bump`: confirma
    que `bump_used` para `bump=None` reproduce el mismo escalar que pedir explícitamente ESE
    `bump_used` como `bump`, y que una medida no-"Greek" (`PayoffPriceQ` suelto) deja `bump_used`
    en `None`.
  - `test_theta_annualized_divides_the_raw_bump_delta_by_the_bump_used` renombrado a
    `..._by_measure_result_bump_used` y reescrito para la nueva firma de `annualize()` (pasa el
    `MeasureResult`, no `.scalar`) y para confirmar `"bump" not in spec[1]` en vez de
    `spec[1]["bump"] == DEFAULT_THETA_BUMP` (la constante ya no existe).
- **Notebooks**:
  - `01_vanilla_options_black_scholes.ipynb` §6: las dos llamadas a `annualize(...)` ahora
    guardan el `MeasureResult` completo (`eng.price(...)["Greek"]`) y se lo pasan a `annualize()`
    en vez de solo `.scalar`; la fila de comparación "theta (1 dia, crudo)" multiplica por
    `theta_raw_result.bump_used` (leído del motor) en vez de `greeks.DEFAULT_THETA_BUMP` (retirada).
    Markdown de la sección actualizado para no mencionar la constante retirada.
  - `09_option_strategies_and_greeks.ipynb`: **sin cambios de código** -- ya evitaba el problema
    por completo leyendo `th.bump_used` de `GreekResult` (vía `Engine.all_greeks`), exactamente el
    rodeo que describe el "Problema" de esta fase como ya funcional; no calcula ni asume
    `DEFAULT_THETA_BUMP` en ningún sitio, así que no había nada que simplificar ahí. Se re-ejecutó
    igualmente para confirmar 0 errores y verificar que el diff resultante es solo timestamps de
    ejecución (`iopub.execute_input`/`execution_count`), ninguna celda de texto/imagen cambió.
- **Verificación en capas** (todas en verde, ninguna se saltó):
  - C++: `cmd.exe /C "vcvars64.bat && cmake --build build --config Release"` compiló limpio.
    `ctest --test-dir build -C Release`: **480/480** (mismo conteo que la línea base de Fase 0: el
    test nuevo se añadió DENTRO de un `TEST()` ya existente, no como `TEST()` nuevo, así que el
    número total de casos no cambia aunque el número de aserciones sí).
  - Python: `.pyd`/`.py` recién compilados copiados a mano a `venv/Lib/site-packages` (el venv
    tenía copias STALE de una instalación no editable, según la nota de entorno del enunciado).
    `pytest clients/python/tests`: **156 passed** (155 previos + 1 test nuevo) + los mismos 2
    errores preexistentes de fixture `abi_dll_path` (no relacionados, no tocados).
  - Notebooks: `jupyter nbconvert --to notebook --execute --inplace` en los dos notebooks, 0 errores
    en ambos. Valores numéricos verificados idénticos a la versión previa: la tabla de `01` §6 sigue
    dando exactamente `delta=0.59763`, `theta (1 dia, crudo)=-0.01479`,
    `theta (anualizado)=-5.39911`, `rho=50.25497` (mismos 5 dígitos que antes de esta fase, mismos
    seeds); el diff de `09` contra HEAD anterior no toca ninguna celda de texto/imagen, solo
    metadata de timing.

**Limitaciones/decisiones que las fases siguientes deben conocer:**

- **Cambio de firma de ruptura en `ThetaGreek.annualize()`** (`raw_value: float` ->
  `measure_result`): cualquier código nuevo (Fase 7, auditoría de notebooks) que use
  `greeks.theta(...)` debe pasar el `MeasureResult` completo a `annualize()`, no `.scalar`.
- **La C ABI (`EngineMeasureResult`/Excel) sigue sin `bump_used` para un "Greek" pedido vía
  `price()`** -- ver el punto de arriba bajo "Trabajo realizado, por capa · C++". Si la Fase 7 (u
  otra) audita el cliente Excel y encuentra una necesidad real de este dato ahí, es una fase nueva,
  no una extensión silenciosa de esta.
- `09_option_strategies_and_greeks.ipynb` no necesitó cambios: cualquier fase futura que quiera
  "unificar" el patrón de lectura de theta entre `01` (via `Greek`/`MeasureResult.bump_used`) y `09`
  (via `all_greeks`/`GreekResult.bump_used`) encontrará que ambos caminos ya leen el bump real del
  motor por su propio tipo de resultado -- no hay una asimetría que cerrar ahí, son dos APIs
  distintas (`price()` vs `all_greeks()`) que ya convergen en el mismo dato.

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

**Estado verificado / decisiones tomadas (sesion de implementacion de esta fase).**
- `clients/python/src/engine_typed/payoff.py`: anadidos `call_leg(observable, strike, qty,
  maturity)`, `put_leg(observable, strike, qty, maturity)` (devuelven un `Contract` crudo, no un
  `PayoffProduct`) y `custom_strategy(id, legs)` (envuelve `both(legs)` en un `PayoffProduct`),
  junto a los builders `european_call`/`european_put`/`irs`/`fx_forward` ya existentes -- misma
  moneda fija `"USD"` que esos, mismo patron `qty * maximum(...)`/`qty * maximum(..., ...)`
  siempre multiplicando (nunca omitido, ni siquiera con `qty=1.0`), replicando exactamente el
  `call_leg`/`put_leg` que ya usaba `09_option_strategies_and_greeks.ipynb` con `qty` explicito.
  Convencion de signo (ADR-P0-02) documentada en el docstring de ambas: `qty` positivo =
  comprado/largo, negativo = vendido/corto, nunca hace falta `give()` porque el signo ya vive en
  el propio cashflow (misma logica que `irs` usa para su pata fija, que si necesita `give()`
  porque ahi el signo lo pone la posicion completa, no un factor negativo por pata). Exportados
  en `clients/python/src/engine_typed/__init__.py` (`__all__` y el bloque de imports).
- Diferencia de AST documentada, NO un bug (instruccion 6 de la tarea): la
  `vanilla_call_contract`/`vanilla_put_contract` que usaba `02_exotic_and_path_dependent_options.ipynb`
  ANTES de esta fase no tenian parametro `qty` y por tanto nunca generaban el nodo `Mul` (solo
  `q.maximum(...)` pelado). `call_leg`/`put_leg` con `qty=1.0` SI generan `Mul(Constant(1.0),
  Max(...))` porque siguen el patron ya establecido por `european_call`/`european_put` (que
  siempre multiplican por `notional`, aunque valga 1.0) y por el `call_leg`/`put_leg` que YA
  usaba `09` con `qty` explicito -- se decidio ese comportamiento como el correcto (precedente ya
  existente en `engine_typed.payoff`, `09` lo necesita para `qty != 1.0`, y `1.0 * x == x` exacto
  en IEEE754, sin perdida de precision ni cambio de precio). El test de paridad y las celdas de
  verificacion cruzada de los notebooks comprueban la parte de AST no ambigua (el `Max` interior)
  y la equivalencia de PRECIO via motor real, no la igualdad byte a byte del arbol completo contra
  la version pre-Fase-6 de `02`.
- Test nuevo en `clients/python/tests/test_engine_typed_payoff.py` (7 tests): paridad
  estructural de `call_leg`/`put_leg` contra los patrones exactos de `02` (sin `qty`) y de `09`
  (con `qty` negativo), `custom_strategy` reproduciendo el `both([...])` de straddle de `02` y el
  `build_contract` de butterfly de `09`, y una parida de PRECIO contra el motor real
  (`Engine.price`, `PayoffPriceQ`) entre `call_leg` y el AST manual equivalente.
- `02_exotic_and_path_dependent_options.ipynb`: `vanilla_call_contract`/`vanilla_put_contract`
  ahora delegan en `q.call_leg`/`q.put_leg`; las versiones previas se conservan como
  `vanilla_call_contract_manual`/`vanilla_put_contract_manual` (celda de verificacion cruzada,
  mismo patron editorial que las Fases 0-5 de `PLAN_IMPROVE_NOTEBOOK.md` para `average`/
  `running_max`), con `assert` de igualdad EXACTA de precio contra el motor real. El straddle
  (`STRADDLE`) se reconstruye con `q.custom_strategy` y se verifica, tambien con `assert` de
  igualdad exacta, contra el `q.both([...])` manual previo.
- `09_option_strategies_and_greeks.ipynb`: los `call_leg`/`put_leg` locales ahora delegan en los
  de `engine_typed.payoff` (mismo `qty`/`maturity`, `OBS` fijado via closure igual que antes);
  `build_contract` (que envolvia `q.both` a mano) se sustituye por `q.custom_strategy` en el
  bucle que construye `PRODUCTS`. Las versiones previas (`call_leg_manual`/`put_leg_manual`/
  `build_contract_manual`) se conservan como celda de verificacion cruzada, con un bucle que
  compara el precio de las 14 estrategias (`STRATEGY_LEGS`) construidas con el camino nuevo
  contra el camino manual -- las 14 coinciden EXACTAMENTE (mismo modelo/mercado/pricing context).
- Verificacion en capas: `pytest clients/python/tests` -> 163 passed (156 base + 7 nuevos), 2
  errores preexistentes de `abi_dll_path` sin tocar (identicos a la linea base). Notebooks `02` y
  `09` regenerados con `jupyter nbconvert --to notebook --execute --inplace`, 0 errores en ambos.
  Comparados los outputs de texto/markdown de ambos notebooks contra `HEAD` (`git show
  HEAD:...ipynb`): en `02`, `vanilla=23.501`, `UI+UO=23.616`, `average`/`running_max`/
  `lookback put` identicos byte a byte; en `09`, las 14 celdas de resumen markdown
  (`delta`/`gamma`/`theta_anual`/`vega`/`vanna`/`volga`/`charm_anual`, incluidos los `NaN` de
  `long_calendar_spread`/`short_calendar_spread`, sin relacion con esta fase) coinciden
  EXACTAMENTE con la version pre-Fase-6 -- confirmando que el cambio de builder no altero ningun
  precio/Greek.
- Ninguna tarea de C++/Rust hizo falta (confirmado, no solo asumido): `call_leg`/`put_leg`/
  `custom_strategy` son puro Python sobre nodos AST (`When`/`Cashflow`/`Both`) ya existentes y ya
  soportados por el compilador Monte Carlo.
- Nota para Fase 1 (tambien toca `02`/`09`): el `intrinsic_value` en NumPy de `09` (y el payoff
  intrinseco pintado en `02`) NO se tocaron en esta fase -- siguen siendo calculo manual en NumPy,
  fuera del alcance de Fase 6 (que es solo sobre el `Contract`/AST del motor, no sobre las curvas
  de referencia dibujadas localmente). Si Fase 1 unifica ese patron, debe hacerlo sin romper las
  nuevas celdas de verificacion cruzada de esta fase (`vanilla_call_contract_manual`,
  `build_contract_manual`, etc.), que dependen de las funciones `_manual` quedandose como estan.
- Nota para Fase 7 (auditoria final): revisar si algun otro notebook (`01`, `03`-`08`) tiene su
  propio patron `when(T, cashflow(qty*max(...)))`/combinador de estrategia ad-hoc que tambien
  deberia migrar a `call_leg`/`put_leg`/`custom_strategy` (esta fase solo audito los dos
  notebooks que el plan senalaba explicitamente, `02`/`09`).

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
