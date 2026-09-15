# Cookbook de errores de PLAN_GREEKS.md

Catálogo de errores reales que produce el motor de Greeks (`engine::greeks::compute_greek`/
`compute_all_greeks`, `cpp/engine/src/greeks.cpp`) al pedir una sensibilidad inválida —
mensajes capturados literalmente ejecutando cada caso a través de `Engine.price`/
`Engine.all_greeks` en Python (misma superficie que Excel/C ABI, ver
[`clients/excel/README.md`](../../clients/excel/README.md#enginall_greeks-plan_greeksmd-85-92)
y `engine/abi.h`), no inventados. `PLAN_GREEKS.md §16` ("Definition of Done") exige exactamente
este catálogo para las cuatro categorías de abajo: factor desconocido, método no soportado,
combinación Q/P inválida, y Theta que cruza un fixing no disponible.

Con `Engine.price(...)` (una `"Greek"` concreta) el error cruza tal cual como excepción
(`ValueError` en Python, `#VALUE!` en Excel, código de retorno != 0 + `engine_abi_last_error`
en la C ABI). Con `Engine.all_greeks(...)` (el barrido automático, §8.5) un candidato inválido
**nunca** hace fallar la llamada entera — cae en `GreeksReport.skipped` con este mismo texto de
motivo (ver "best effort" en PLAN_GREEKS.md §8.5).

## 1. Factor de riesgo desconocido

`RiskFactor` se parsea de forma estricta desde el string namespaced (`"model.spot"`,
`"curve.parallel"`, ...) — un scope desconocido, o un parámetro que el modelo concreto no
declara en `to_params()`, es un error explícito, nunca `0.0` silencioso (PLAN_GREEKS.md §3.1).

| Condición | Mensaje | Cómo solucionarlo |
|---|---|---|
| Scope desconocido (ni `model`/`curve`/`credit`/`time`) | `RiskFactor: scope 'bogus' desconocido (validos: 'model', 'curve', 'credit', 'time')` | Usar uno de los cuatro scopes documentados (PLAN_GREEKS.md §3.1). |
| `risk_factor` sin el formato `<scope>.<nombre>` | `RiskFactor: '<texto>' no tiene el formato '<scope>.<nombre>' esperado (scopes validos: model, curve, credit, time)` | Incluir siempre el punto separador, p.ej. `"model.spot"` no `"spot"`. |
| Parámetro de modelo que ese `model_type` no declara | `compute_greek: el modelo 'GBM' no tiene el parametro de riesgo 'model.nope' (validos: model.spot, model.rate, model.dividend_yield, model.volatility)` | Pedir uno de los parámetros que ese modelo SÍ declara (el mensaje los nombra); revisar `IModel::to_params()` del modelo concreto para la lista completa (HullWhite1F/2F exponen `a`/`b`/`sigma`/`r0`[/`eta`/`rho`], sin alias). |
| `curve.pillar:<i>` fuera de rango | `compute_greek: 'curve.pillar:<i>' fuera de rango (la curva tiene <n> pillars)` | Usar un índice `0..n-1` sobre la curva del `MarketSnapshot` pasado. |

```python
# ValueError: RiskFactor: scope 'bogus' desconocido (validos: 'model', 'curve', 'credit', 'time')
eng.price(product, [greeks.Greek(metric="PayoffPriceQ", risk_factor="bogus.spot").to_spec()], ...)
```

## 2. Método no soportado para la combinación pedida

`method="pathwise"`/`"aad"` explícito sobre una combinación (modelo, métrica, factor) no
verificada contra bump-and-reval (tabla de capacidades, §5.4) es un error inmediato — nunca
degrada en silencio a `bump_and_reval` (esa degradación silenciosa solo ocurre bajo
`method="auto"`, y por diseño, no por error).

| Condición | Mensaje | Cómo solucionarlo |
|---|---|---|
| `method="pathwise"` sobre una métrica de tipo indicador/probabilidad (`PayoffHitProbabilityQ`/`P`) | `compute_greek: metodo 'pathwise' no soportado para 'PayoffHitProbabilityQ' (metrica de tipo indicador/probabilidad -- su derivada pathwise exacta es 0 en casi todo punto y no informa nada, PLAN_GREEKS.md §4.5; usa method='auto' o 'bump_and_reval')` | Usar `method="auto"` (cae a `bump_and_reval`, que sí agrega bien sobre muchas rutas) o pedirlo explícito. |
| `method="pathwise"`/`"aad"` sobre (modelo, métrica) fuera de `pathwise_capabilities()`/`aad_capabilities()` | `compute_greek: metodo 'pathwise' no soportado para (modelo='<M>', metrica='<m>') -- combinacion no verificada contra bump-and-reval todavia (PLAN_GREEKS.md §5.3)` | Usar `method="auto"` o `"bump_and_reval"`; solo Gbm/PayoffPriceQ, GbmP/PayoffForecastP (pathwise) y HullWhite1F·2F/HullWhiteModelNpv (aad) están verificados hoy. |
| `method="pathwise"` sobre un contrato con `ContractOp::Exercise` | `compute_greek: metodo 'pathwise' no soportado para un contrato con ContractOp::Exercise (re-decidir Longstaff-Schwartz bajo el parametro perturbado no es pathwise-diferenciable, PLAN_GREEKS.md §5.1; usa method='auto' o 'bump_and_reval')` | Usar `method="auto"` (cae a bump-and-reval automáticamente para este caso) o `"bump_and_reval"` explícito. |
| `method="aad"` sobre `model.rho` de HullWhite2F | `compute_greek: metodo 'aad' no soportado para 'model.rho' de HullWhite2F ('rho' no es un tensor diferenciable en la implementacion actual del modelo, PLAN_GREEKS.md §5.2; usa method='auto' o 'bump_and_reval')` | Usar `method="auto"`/`"bump_and_reval"` para `rho` — el resto de parámetros de HullWhite2F sí tienen AAD. |
| `order=2` o `cross_factor` con `method="pathwise"`/`"aad"` | `compute_greek: metodo '<m>' solo soporta order=1 sin cross_factor (Gamma/derivadas cruzadas se sirven por bump-and-reval, PLAN_GREEKS.md §5.2)` | Pedir Gamma/Vanna sin forzar `method`; siempre se calculan por bump-and-reval, aunque el Delta del mismo factor use una ruta especializada. |

```python
# ValueError: compute_greek: metodo 'pathwise' no soportado para 'PayoffHitProbabilityQ' (...)
forced = greeks.Greek(
    metric="PayoffHitProbabilityQ", risk_factor="model.spot", metric_params={"event": "UI"}, method="pathwise"
)
eng.price(product, [forced.to_spec()], ...)
```

## 3. Combinación Q/P inválida (heredada de la métrica base)

El motor de Greeks no decide Q/P — lo hereda de la propia métrica base (PLAN_GREEKS.md §6): si
la métrica ya rechaza el modelo por no declarar la `ProbabilityMeasure` requerida, `compute_greek`
simplemente propaga esa excepción sin duplicar el chequeo.

| Condición | Mensaje | Cómo solucionarlo |
|---|---|---|
| Métrica Q (`PayoffPriceQ`, ...) sobre un modelo que solo declara `PhysicalP` (`GBM_P`), o viceversa | `PayoffPriceQMeasure: modelo no soportado: GBM_P` (el nombre exacto de medida/modelo varía según el caso) | Usar el modelo Q (`GBM`, `HullWhite1F/2F`) para métricas `*Q`, y `GBM_P` para métricas `*P` (`PayoffForecastP`, `PayoffHitProbabilityP`, `PayoffPnlDistributionP`) — nunca mezclarlos. |

```python
# ValueError: PayoffPriceQMeasure: modelo no soportado: GBM_P
gbm_p = eng.create_model("GBM_P", {"s0": 100.0, "mu": 0.08, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"})
eng.price(product, ["PayoffPriceQ"], gbm_p, market, pricing, execution)
```

## 4. Theta (`time.theta`) que cruza un fixing no disponible

Theta puro (§7.1) reevalúa la métrica en `valuation_time = t + dt`; si `dt` hace que ese
instante alcance o supere un fixing/pago que el motor no puede reconstruir sin histórico (la
ruta Monte Carlo bajo Q no modela fixings históricos), se rechaza explícito en vez de
extrapolar (mismo principio que `FixingStore` sobre `MarketPath`, ADR-P0-08 de
PLAN_PRODUCTS.md).

| Condición | Mensaje | Cómo solucionarlo |
|---|---|---|
| `time.theta` con `bump`/`dt` que alcanza o supera un instante requerido por el contrato de payoff (p.ej. la maturity de una call) | `root: payoff: valuation_time (<t+dt>) alcanza o supera un instante requerido por el contrato (<t_requerido>) -- este motor no modela fixings historicos de la ruta Monte Carlo bajo Q (PLAN_GREEKS.md §7.4), Theta no puede cruzar ese instante` | Usar un `dt` (o el `bump` por defecto, un día) estrictamente menor a la distancia hasta el próximo instante requerido del contrato. |
| `time.theta` sobre una métrica que todavía no honra `PricingContext::pricing_date()` | `compute_greek: 'time.theta' todavia no esta cableado para la metrica '<m>' (solo honra PricingContext::pricing_date() en 'PV' y 'PayoffPriceQ'; pedirlo sobre otra metrica daria un Theta silenciosamente nulo, asi que se rechaza explicito en vez de aproximar)` | Pedir Theta solo de `"PV"`/`"PayoffPriceQ"` hoy (§11 Fase 5); para otra métrica, esperar a que una fase futura la cablee. |
| `time.theta` sobre un `IrSwapProduct` cuando `dt` cruza el primer reseteo de la pata flotante | Mismo mensaje que la primera fila, con el instante del primer reset como "instante requerido" | Usar un `dt` menor a la distancia hasta `start()`/el primer `payment_time`, o mover `start()` más al futuro. |

```python
# ValueError: root: payoff: valuation_time (2) alcanza o supera un instante requerido
# por el contrato (1) -- este motor no modela fixings historicos de la ruta Monte Carlo
# bajo Q (PLAN_GREEKS.md §7.4), Theta no puede cruzar ese instante
eng.price(product, [greeks.theta("PayoffPriceQ", bump=2.0).to_spec()], ...)  # maturity=1.0
```
