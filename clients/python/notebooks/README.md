# Notebooks

`demo_registry.ipynb` es la demo/tutorial del registry (PLAN.md §5.4/§7.6) desde Python.

A partir de ahí, una batería de notebooks centrada en valoración de opciones, obtención de
métricas y visualizaciones financieras — cada uno explota una parte distinta de la librería
(`engine`/`engine_typed`), siempre con `matplotlib`/`numpy` para las gráficas:

1. `01_vanilla_options_black_scholes.ipynb` — calls/puts europeas vía `PayoffPriceQ` sobre
   `GBM`, contraste contra Black-Scholes cerrado, paridad put-call, curvas de precio/Greeks
   frente a strike y spot (`engine_typed.greeks`).
2. `02_exotic_and_path_dependent_options.ipynb` — barreras (knock-in/out, corredor double
   knock-out), digital, asiático aritmético nativo (`q.average`, réplica manual como celda de
   verificación cruzada), lookback real (`q.running_max`/`q.running_min`), take-profit/stop-loss,
   straddle/strangle.
3. `03_bermudan_exercise.ipynb` — ejercicio bermuda vía Longstaff-Schwartz (`PayoffExerciseQ`),
   diagnósticos de ejercicio por fecha, convergencia hacia el límite americano.
4. `04_greeks_and_risk_surfaces.ipynb` — barrido automático (`Engine.all_greeks`), Hessiana
   completa y HVP (`Engine.hessian`/`Engine.hvp`: gamma/vanna/volga vía likelihood-ratio,
   AAD forward-over-forward en Hull-White), superficie precio/delta vía `price_grid`.
5. `05_physical_measure_forecasting.ipynb` — medida física P (`GBM_P`): forecast, función de
   supervivencia empírica de `S_T`, VaR/ES (`PayoffPnlDistributionP`), mapa de riesgo de caída.
6. `06_exposure_cva_portfolio.ipynb` — perfil de exposición EE/PFE y CVA (nativo para IRS,
   integrado a mano desde `PayoffExposureProfileQ` para una opción), portfolio de opciones vía
   `price_grid` bajo escenarios de mercado.
7. `07_montecarlo_paths_q_vs_p.ipynb` — el motor no expone las trayectorias Monte Carlo por el
   binding Python (solo medidas agregadas), así que este notebook replica en NumPy la misma
   SDE exacta de `GBM`/`GBM_P` para dibujar TODAS las trayectorias (fan chart Q vs P, mismo
   horizonte), calcula esperanza/varianza/asimetría/curtosis/percentiles de cada medida, y
   valida esos números contra las medidas reales del motor (`PayoffPriceQ`, `PayoffForecastP`,
   `PayoffHitProbabilityQ`/`P`).
8. `08_multi_asset_options.ipynb` — modelo multi-activo correlacionado (`GbmBasket`,
   PLAN_IMPROVE_NOTEBOOK.md Fase 3): basket call, spread option, worst-of/best-of, y un compo
   option ("quanto-style" — un quanto de tipo fijo real necesitaría un ajuste de drift que
   `GbmBasket` no implementa, ver la nota del notebook), comparando tres niveles de correlación
   y razonando el signo del efecto producto a producto (no es el mismo para los cinco).
9. `09_option_strategies_and_greeks.ipynb` — 14 estrategias custom construidas via `q.both`
   (buy/sell call/put, straddle, butterfly, condor, calendar spread, ratio spread, cada una
   larga y corta) bajo 6 escenarios de volatilidad: valor hoy vs payoff intrínseco, y las
   griegas delta/gamma/theta/vega más vanna/volga/charm (`Engine.all_greeks`/`Engine.hessian`,
   más una diferencia finita entre dos `PricingContext` para charm porque el método pathwise de
   `Greek` no usa `pricing_date`). Análisis escrito antes y después de cada bloque de gráficos.

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
