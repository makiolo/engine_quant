# Notebooks

`demo_registry.ipynb` es la demo/tutorial del registry (PLAN.md §5.4/§7.6) desde Python.

A partir de ahí, una batería de notebooks centrada en valoración de opciones, obtención de
métricas y visualizaciones financieras — cada uno explota una parte distinta de la librería
(`engine`/`quantdesk`, PLAN_API_REFACTOR.md Fase 5: `quantdesk` sustituye a `engine_typed` como
fachada tipada recomendada), siempre con `matplotlib`/`numpy` para las gráficas:

1. `01_vanilla_options_black_scholes.ipynb` — calls/puts europeas vía `PayoffPriceQ` sobre
   `GBM`, contraste contra Black-Scholes cerrado, paridad put-call, curvas de precio/Greeks
   frente a strike y spot (`quantdesk.greeks`).
2. `02_exotic_and_path_dependent_options.ipynb` — barreras (knock-in/out, corredor double
   knock-out), digital, asiático aritmético nativo (`q.average`, réplica manual como celda de
   verificación cruzada), lookback real (`q.running_max`/`q.running_min`), take-profit/stop-loss,
   straddle/strangle vía `q.call_leg`/`q.put_leg`/`q.custom_strategy`
   (PLAN_IMPROVE_NOTEBOOK2.md Fase 6, réplica manual como celda de verificación cruzada).
3. `03_bermudan_exercise.ipynb` — ejercicio bermuda vía Longstaff-Schwartz (`PayoffExerciseQ`),
   diagnósticos de ejercicio por fecha, convergencia hacia el límite americano. La pata europea
   que sirve de rama de continuación (`european_put_contract`) delega en `q.put_leg`
   (PLAN_IMPROVE_NOTEBOOK2.md Fase 6, auditoría de Fase 7) en vez de horneado a mano.
4. `04_greeks_and_risk_surfaces.ipynb` — barrido automático (`Engine.all_greeks`), Hessiana
   completa y HVP (`Engine.hessian`/`Engine.hvp`: gamma/vanna/volga vía likelihood-ratio,
   AAD forward-over-forward en Hull-White), superficie precio/delta vía `price_grid`.
5. `05_physical_measure_forecasting.ipynb` — medida física P (`GBM_P`): forecast, función de
   supervivencia empírica de `S_T`, VaR/ES (`PayoffPnlDistributionP`), mapa de riesgo de caída.
6. `06_exposure_cva_portfolio.ipynb` — perfil de exposición EE/PFE y CVA (nativo para IRS,
   integrado a mano desde `PayoffExposureProfileQ` para una opción), portfolio de opciones vía
   `price_grid` bajo escenarios de mercado. Los contratos de call (opción única y cesta de
   strikes) se construyen con `q.call_leg` (PLAN_IMPROVE_NOTEBOOK2.md Fase 6, auditoría de
   Fase 7) en vez de horneado a mano.
7. `07_montecarlo_paths_q_vs_p.ipynb` — nació de una limitación ya cerrada (el binding Python
   no exponía las trayectorias Monte Carlo, solo medidas agregadas); desde
   `PLAN_IMPROVE_NOTEBOOK.md` Fase 0, `Engine.simulate_paths(model, market, pricing)` devuelve
   la matriz de trayectorias REAL que `GBM`/`GBM_P` calculan por dentro, y es la fuente de TODO
   lo que dibuja/calcula este notebook (fan chart Q vs P, estadísticas terminales,
   esperanza/varianza/asimetría/curtosis/percentiles). La reimplementación NumPy de la SDE
   (antes la única fuente) se conserva como test de regresión cruzada explícito (sección 1b,
   por momentos agregados, no ruta a ruta — friccion 7 de `PLAN_IMPROVE_NOTEBOOK2.md`, RNG
   distinto entre motor y NumPy, documentada como fuera de alcance), no como fuente de datos.
   Valida además esos números contra las medidas reales del motor (`PayoffPriceQ`,
   `PayoffForecastP`, `PayoffHitProbabilityQ`/`P`).
8. `08_multi_asset_options.ipynb` — modelo multi-activo correlacionado (`GbmBasket`,
   PLAN_IMPROVE_NOTEBOOK.md Fase 3): basket call, spread option, worst-of/best-of, y un compo
   option ("quanto-style" — un quanto de tipo fijo real necesitaría un ajuste de drift que
   `GbmBasket` no implementa, ver la nota del notebook), comparando tres niveles de correlación
   y razonando el signo del efecto producto a producto (no es el mismo para los cinco). Sección 2b
   (PLAN_IMPROVE_NOTEBOOK2.md Fase 4): `Engine.simulate_paths` generalizado a `GbmBasket` (fan
   chart de trayectorias correlacionadas por activo), delta por activo del basket call
   (`greeks.delta("PayoffPriceQ", "spot_0"/"spot_1")`, verificado contra bump-and-reval manual) y
   la cross-gamma real entre los dos activos (`greeks.cross_gamma(...)`, estencil genérico de 4
   puntos) — con una nota explícita de que su signo (positivo) y su dirección frente a la
   correlación (BAJA, al contrario que el precio) no son la misma pregunta: el precio es monótono
   en varianza, la cross-gamma mide curvatura concentrada en el strike, que se diluye con más
   volatilidad efectiva (misma lógica que la gamma de Black-Scholes decreciendo con la
   volatilidad).
9. `09_option_strategies_and_greeks.ipynb` — 14 estrategias custom construidas via
   `q.call_leg`/`q.put_leg`/`q.custom_strategy` (PLAN_IMPROVE_NOTEBOOK2.md Fase 6, misma fuente
   que `02`; réplica manual con `q.both` a mano como celda de verificación cruzada)
   (buy/sell call/put, straddle, butterfly, condor, calendar spread, ratio spread, cada una
   larga y corta) bajo 6 escenarios de volatilidad: valor hoy vs payoff intrínseco, y las
   griegas delta/gamma/theta/vega más vanna/volga/charm. Delta/vega/theta se piden en una única
   llamada a `Engine.price(...)` con tres entradas `"Greek"` distinguidas por alias
   (`Measure.to_spec(alias=...)`, PLAN_IMPROVE_NOTEBOOK2.md Fase 3, opción (b)) en vez de
   `Engine.all_greeks` — no comparte pasada Monte Carlo entre las tres (eso sigue pendiente como
   Fase 3b, ver el ADR de esa fase), pero evita las 3 simulaciones que `all_greeks` desperdiciaba
   por punto del grid (`curve.parallel`/`credit.hazard_rate`/`credit.recovery_rate`, sin sentido
   económico para este producto). Gamma/vanna/volga siguen viniendo de `Engine.hessian` (una
   única pasada Monte Carlo compartida por las tres, sin cambios en esta fase). Charm sigue una
   diferencia finita entre dos `PricingContext` con `method="bump_and_reval"` explícito — por
   diseño, no como workaround: ninguna especialización pathwise/likelihood-ratio cubre
   `RiskFactorKind::TimeShift`, y desde PLAN_IMPROVE_NOTEBOOK2.md Fase 0 el motor además rechaza
   explícitamente la especialización pathwise de spot/vega/rho bajo `pricing_date != 0` en vez de
   ignorarlo en silencio (las dos llamadas de charm no se pueden fusionar con alias porque usan
   `PricingContext` distintos — `Engine.price` solo acepta uno por llamada). El payoff intrínseco
   a vencimiento (`intrinsic_value`) ya no se
   reimplementa a mano en NumPy: delega en `Engine.evaluate_scenario` (PLAN_IMPROVE_NOTEBOOK2.md
   Fase 1, `ScenarioEvaluator` nativo sobre un escenario de spot fijo, sin modelo ni Monte Carlo);
   la fórmula NumPy se conserva como `intrinsic_value_manual`, celda de verificación cruzada que
   confirma coincidencia exacta para las 14 estrategias. Análisis escrito antes y después de cada
   bloque de gráficos. Para los dos calendarios, `gamma`/`vanna`/`volga` salen `NaN` (contrato de
   más de una fecha terminal, fuera del alcance de la Hessiana vía likelihood-ratio) — desde
   PLAN_IMPROVE_NOTEBOOK2.md Fase 2 el motivo exacto lo reporta el propio motor en
   `HessianReport.skipped` (celda de verificación dedicada justo antes de analizar
   `long_calendar_spread`), no una explicación inferida a mano.

Varios notebooks documentan, en la celda donde aparece, un workaround motivado por una
limitación concreta del motor (no del binding Python) — cada uno enlaza a la fase
correspondiente de [`PLAN_IMPROVE_NOTEBOOK.md`](../../../PLAN_IMPROVE_NOTEBOOK.md) (raíz del
repo), el plan de mejoras al motor que salió de construir esta batería.

Requiere haber compilado el proyecto con CMake antes (ver `PLAN.md` en la raíz) — el módulo
compilado (`engine.cp3XX-....pyd` en Windows, `engine*.so` en Linux/Mac) debe existir en
`build/clients/python`.

Para lanzarlos:

```bash
pip install jupyterlab matplotlib
cd clients/python/notebooks
jupyter lab
```

Para regenerar las salidas de todos (por ejemplo tras un cambio en el motor):

```bash
jupyter nbconvert --to notebook --execute --inplace *.ipynb
```
