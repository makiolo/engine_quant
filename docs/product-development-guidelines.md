# Guía de productos y roadmap de valoración profesional

## 1. Objetivo y criterio de clasificación

Esta guía describe qué productos puede representar y valorar hoy el engine, qué productos
pueden expresarse con el DSL de `Payoff` pero aún no valorarse correctamente, y qué modelos y
métricas conviene implementar por fases para convertirlo en un engine de uso profesional.

La distinción fundamental es:

1. **Expresable**: el árbol de payoff puede describir los cashflows y condiciones del contrato.
2. **Simulable**: un modelo disponible genera todos los observables y estados que usa el árbol.
3. **Valorable**: existe un algoritmo compatible con el producto y el modelo bajo la medida
   correcta, con descuento, ejercicio y eventos tratados correctamente.
4. **Apto para producción**: además hay calibración, datos de mercado, convenciones, métricas de
   riesgo, diagnósticos numéricos y validación independiente.

Un payoff componible resuelve principalmente el primer punto. No convierte por sí solo cualquier
contrato en valorable. Por ejemplo, un swaption se puede aproximar mediante un `Exercise`, pero no
se puede valorar profesionalmente sin una dinámica de tipos calibrada, una representación del swap
subyacente y un algoritmo de ejercicio compatible.

Esta guía refleja el código actual, no solo los planes del repositorio. Las referencias principales
son `quantdesk.payoff`, `quantdesk.model`, las medidas registradas en `bootstrap.cpp` y los
preflights de `payoff/measures.cpp`.

## 2. Capacidades actuales

### 2.1 Lenguaje de payoff

El DSL permite componer:

- expresiones: constantes, parámetros, `Fixing`, `Current`, suma, resta, producto, división,
  negación, valor absoluto, exponencial, logaritmo, potencia, mínimo, máximo, clamp, media
  ponderada, mínimo/máximo acumulado, tiempo/valor de evento, factor de descuento y conversión FX;
- predicados: comparaciones, igualdad con tolerancia, conjunción, disyunción, negación, rango,
  evento ocurrido y comparaciones temporales;
- contratos: cero, cashflow, posición corta (`Give`), composición (`Both`), escala, condicional,
  pago en fecha, trigger y ejercicio.

Hay plantillas Python para call/put europea, patas call/put, estrategias, IRS single-curve y FX
forward. Para las recetas de bajo nivel y las reglas estructurales véase también
[`schema/engine.payoff/cookbook.md`](schema/engine.payoff/cookbook.md).

### 2.2 Matriz real producto-modelo-métrica

| Ruta | Producto | Modelo | Resultado | Límites importantes |
|---|---|---|---|---|
| Determinista | `IRSwap` | El modelo no interviene en `PV`/`DV01` | PV, DV01 paralelo y bucketed | Una sola curva; pata flotante replicada como `P(start)-P(end)`; sin fixings ni calendarios |
| Tipos MC | `IRSwap` | `HullWhite1F`, `HullWhite2F` | NPV de modelo, EE, PFE95, CVA unilateral | Parámetros constantes; curva inicial simplificada; CVA con hazard/recovery escalares |
| Payoff determinista | `PayoffProduct` sin observables estocásticos | Cualquiera | PV y DV01 | Solo cashflows descontables desde `Market`; una moneda en la ruta genérica |
| Payoff Q univariante | `PayoffProduct` | `Gbm` | Precio MC Q | Un spot, `r/q/sigma` constantes |
| Payoff Q multi-activo | `PayoffProduct` | `GbmBasket` | Precio MC Q | Correlación constante; todos los activos deben compartir `r`; sin ejercicio ni bridge continuo |
| Ejercicio | Un único nodo `Exercise` | `Gbm` | Precio Q y diagnóstico LSM | Exactamente un derecho de ejercicio; regresión cuadrática; no cesta |
| Eventos Q | `Trigger` | `Gbm` | Probabilidad de hit | No está cableado para `GbmBasket` |
| Exposición de payoff | `PayoffProduct` | `Gbm` | EE, PFE95 y CVA unilateral | Valor restante realizado por path, no valoración condicional/anidada |
| Forecast/riesgo P | `PayoffProduct` | `GbmP` | Media, hit probability, VaR y ES | Un activo; deriva y volatilidad constantes; no es un precio arbitrage-free |
| Escenario fijo | `PayoffProduct` | Sin modelo | Ledger sin descuento | Una trayectoria determinista conocida |

`GBM` declara soporte Q, bridge browniano y regresión de ejercicio. `GBM_P` declara soporte P y
bridge, pero no ejercicio. `GbmBasket` declara paths conjuntos bajo Q, pero no bridge continuo ni
regresión de ejercicio. `HullWhite1F/2F` no suministran observables al DSL moderno: actualmente
solo sirven a la ruta especializada de `IRSwap`.

### 2.3 Medidas registradas hoy

- deterministas/tipos: `PV`, `DV01`, `HullWhiteModelNpv`, `ExposureProfile`,
  `ExpectedExposure`, `PFE95` y `UnilateralCVA`;
- payoff bajo Q: `PayoffPriceQ`, `PayoffExerciseQ`, `PayoffHitProbabilityQ`,
  `PayoffExposureProfileQ`, `PayoffUnilateralCvaQ` y `PayoffSensitivityQ`;
- payoff bajo P: `PayoffForecastP`, `PayoffHitProbabilityP` y `PayoffPnlDistributionP`;
- riesgo genérico: `Greek`, además de `all_greeks`, Hessian y HVP en las combinaciones de
  modelo/métrica declaradas por el motor.

Algunas medidas requieren parámetros: `event`, `exposure_times`, `confidence` o `greek`. Que una
medida aparezca en el registry no significa que acepte todos los modelos y productos; la matriz
anterior y el preflight son la fuente de compatibilidad.

## 3. Lista 1: productos

### 3.1 Productos valorables hoy

#### Renta fija y FX deterministas

- cashflows fijos y carteras de cashflows en una moneda;
- bonos cupón cero y bonos de cupón fijo construidos como suma de cashflows;
- IRS vanilla fijo-flotante single-curve, payer o receiver;
- FX forward cash-settled si el tipo FX se modela como observable GBM;
- FX forward físico de dos monedas solo como contrato expresable/ledger. El `PV` genérico actual
  lo rechaza porque no dispone de infraestructura multi-curva y conversión FX completa.

#### Equity/FX univariante bajo GBM

- forwards y posiciones lineales;
- calls y puts europeas, digitales y payoffs piecewise mediante `If`, `Min`, `Max` y `Clamp`;
- estrategias estáticas: spreads, straddle, strangle, butterfly, condor, risk reversal y cualquier
  suma algebraica de patas;
- asiáticas aritméticas con calendario y pesos explícitos;
- lookbacks y payoffs sobre running min/max;
- barreras up/down, in/out, dobles barreras discretas y, para patrones reconocidos, bridge
  browniano de monitorización continua aproximada;
- take-profit/stop-loss y first-hit mutuamente excluyente mediante triggers y eventos;
- productos bermuda sencillos con exactamente un nodo `Exercise`, mediante Longstaff–Schwartz;
- combinaciones de cashflows y eventos como rebates, bonus certificates y estructuras simples de
  capital protegido, siempre que toda la dinámica dependa del único spot GBM.

#### Multi-activo bajo `GbmBasket`

- basket options de media o suma;
- spread options;
- best-of, worst-of, outperformance y exchange-style payoffs;
- payoffs rainbow europeos o con monitorización discreta;
- estrategias que combinan varios spots correlacionados.

Estas estructuras son valorables solo cuando bastan volatilidades y correlaciones constantes,
una tasa Q común y no requieren ejercicio temprano ni corrección continua de barrera.

#### Medida física y riesgo

- forecast de cashflows bajo un `GbmP` univariante;
- probabilidad física de eventos;
- distribución de P&L, VaR y Expected Shortfall para payoffs expresables sobre ese único activo.

### 3.2 Expresables, pero no valorables profesionalmente con el stack actual

| Familia | Qué falta |
|---|---|
| Americanas | Ejercicio en tiempo continuo; hoy solo se puede usar una malla bermuda |
| Bermudas multi-activo o con varios derechos | LSM multi-factor robusto, varios nodos de ejercicio, bases configurables y control de sesgo |
| Autocallables complejos | Son parcialmente expresables con triggers, pero faltan modelos de skew/local vol, dividendos y tipos/FX conjuntos; también métricas de call probability por fecha |
| Barreras multi-activo continuas | Bridge multivariante y tratamiento coherente de first passage con correlación |
| Cliquets y ratchets | Parte del payoff se puede expandir con fixings, pero faltan estado acumulativo general, superficies forward y modelos de smile dinámico |
| Variance/volatility swaps, options sobre realized variance | Observable de varianza realizada y modelos de volatilidad; el DSL no tiene hoy un acumulador cuadrático nativo |
| Opciones con dividendos discretos | Calendario de dividendos y dinámica ex-dividendo |
| Opciones quanto/compo reales | Modelo conjunto spot-FX-tipos, correlaciones cross-asset y conversión monetaria estocástica |
| Caps/floors, swaptions, CMS | Curvas de proyección, índices y fixings, volatilidad de tipos, annuity/forward measures y pricers especializados |
| Callable bonds y callable swaps | Cashflows de renta fija completos y ejercicio sobre factores de tipos |
| FRN, OIS, basis/cross-currency swaps | Motor de schedules, day count, fixing store, múltiples curvas y colateral/CSA |
| Bonos amortizables o inflación | Índices, lags, seasonality, interpolation y cashflow conventions |
| CDS | Curva de supervivencia por plazos, fechas de default, accrued-on-default y calibración a spreads |
| CDO/tranches y basket credit | Dependencia de default, copula/intensity multi-name y recuperación coherente |
| Commodities | Curvas forward, convenience yield, seasonality y modelos multifactor |

### 3.3 No abordables sin técnicas avanzadas o especializadas

- derivados con volatilidad estocástica, local o rough cuando el smile es material;
- productos sensibles a gaps o colas que requieren saltos, Lévy o mezclas;
- híbridos equity-FX-rates-credit con wrong-way risk;
- derivados de volatilidad y correlación: variance options, dispersion, correlation swaps;
- mortgage-backed securities, prepayment y productos dependientes de comportamiento;
- opciones sobre energía con restricciones físicas, swing, storage y despacho óptimo;
- convertibles con interacción entre equity, crédito, calls/puts y recuperación;
- XVA bilateral completo con netting sets, CSA, margin period of risk, funding, initial margin y
  wrong-way risk;
- productos con decisiones múltiples o control estocástico que requieren dynamic programming,
  trees/PDE de alta dimensión, stochastic mesh, dual methods o nested Monte Carlo;
- modelos con memoria o alta dimensionalidad donde sea necesario deep BSDE, regresión neuronal o
  técnicas específicas. Estas técnicas deben ser una elección validada, no el default del engine.

## 4. Guidelines para crear productos en Python con `quantdesk`

### 4.1 Elegir primero la ruta de valoración

Antes de construir el AST, decidir:

- `PV`/`DV01`: cashflows deterministas descontables desde `Market`;
- `PayoffPriceQ`: precio arbitrage-free bajo `Gbm` o `GbmBasket`;
- `PayoffExerciseQ`: un producto bermuda univariante;
- `PayoffForecastP`, `PayoffHitProbabilityP`, `PayoffPnlDistributionP`: forecast o riesgo bajo P,
  nunca un precio Q;
- `evaluate_scenario`: explicación de payoff sobre una ruta fija, sin descuento ni probabilidad.

No debe elegirse un modelo solo porque acepta el AST. El modelo debe representar los factores de
riesgo materiales del producto y calibrarse a instrumentos líquidos relevantes.

### 4.2 Reglas de autoría

1. Usar primero una plantilla (`european_call`, `irs`, `fx_forward`, `call_leg`, `put_leg`) y
   bajar al AST solo cuando la estructura lo exija.
2. Usar identificadores estables y namespaced, por ejemplo `EQ.SPOT.AAPL` o `FX.SPOT.EURUSD`.
   El nombre debe coincidir exactamente con `model.observable`/`model.observables`.
3. Expresar todos los tiempos como year fractions coherentes. El engine aún no calcula fechas,
   calendarios, business-day adjustments ni day-count conventions.
4. Envolver todo `Cashflow` en `when(...)` o en un trigger con settlement `at_hit`.
5. Mantener únicos los ids de `Trigger` y `Exercise`, y ordenar estrictamente todos los schedules.
6. Declarar moneda en cada cashflow, incluso si todo el trade usa la misma.
7. Separar parámetros contractuales (`q.parameter`) de datos de mercado y de parámetros del modelo.
8. Construir portfolios neteables dentro de un mismo árbol con `both(...)` solo cuando comparten
   realmente netting set, moneda y reglas legales. La composición algebraica no prueba netting legal.
9. Validar estructura, dimensiones, signos y límites antes de aumentar `n_paths`.
10. Para MC, reportar semilla, número de paths, discretización, error estándar e intervalo de
    confianza; hoy parte de esos diagnósticos existe internamente pero no toda llega a `MeasureResult`.

### 4.3 Ejemplo mínimo: call europea

```python
import quantdesk as q

trade = q.european_call(
    id="AAPL_CALL_100",
    observable="EQ.SPOT.AAPL",
    strike=100.0,
    notional=1_000.0,
    maturity=1.0,
)

model = q.Gbm(
    s0=102.0,
    r=0.03,
    q=0.01,
    sigma=0.22,
    observable="EQ.SPOT.AAPL",
)

market = q.Market(pillars=[1.0], zero_rates=[0.03])
engine = q.Engine(n_paths=200_000, n_steps=252, seed=7, backend="auto")

result = engine.price(trade, model, market, ["PayoffPriceQ"])
print(result["PayoffPriceQ"].scalar)
```

### 4.4 Ejemplo: estrategia componible y Greeks

```python
import quantdesk as q
from quantdesk import greeks

obs = "EQ.SPOT.SPX"
maturity = 0.5

butterfly = q.custom_strategy(
    "SPX_BUTTERFLY",
    [
        q.call_leg(obs, strike=4_900.0, qty=1.0, maturity=maturity),
        q.call_leg(obs, strike=5_000.0, qty=-2.0, maturity=maturity),
        q.call_leg(obs, strike=5_100.0, qty=1.0, maturity=maturity),
    ],
)

model = q.Gbm(s0=5_000.0, r=0.03, q=0.015, sigma=0.20, observable=obs)
market = q.Market(pillars=[0.5, 1.0], zero_rates=[0.03, 0.03])
engine = q.Engine(n_paths=300_000, n_steps=126, seed=19)

metrics = [
    "PayoffPriceQ",
    greeks.delta("PayoffPriceQ", "spot").to_spec(alias="delta"),
    greeks.gamma("PayoffPriceQ", "spot").to_spec(alias="gamma"),
    greeks.vega("PayoffPriceQ").to_spec(alias="vega"),
]

result = engine.price(butterfly, model, market, metrics)
for name in ("PayoffPriceQ", "delta", "gamma", "vega"):
    print(name, result[name].scalar)
```

### 4.5 Ejemplo: asiática

```python
import quantdesk as q

obs = "EQ.SPOT.NVDA"
times = [0.25, 0.50, 0.75, 1.00]

trade = q.PayoffProduct(
    id="NVDA_ASIAN_CALL",
    contract=q.when(
        1.0,
        q.cashflow(
            "USD",
            100.0 * q.maximum(
                q.average(obs, schedule=times, weights=[0.25] * 4) - 120.0,
                0.0,
            ),
        ),
    ),
)

model = q.Gbm(s0=118.0, r=0.04, q=0.0, sigma=0.35, observable=obs)
market = q.Market(pillars=[1.0], zero_rates=[0.04])
engine = q.Engine(n_paths=250_000, n_steps=252, seed=23)

price = engine.price(trade, model, market, ["PayoffPriceQ"])["PayoffPriceQ"].scalar
```

### 4.6 Ejemplo: cesta europea

```python
import quantdesk as q

a = "EQ.SPOT.A"
b = "EQ.SPOT.B"
t = 1.0
basket = 0.6 * q.fixing(a, t) + 0.4 * q.fixing(b, t)

trade = q.PayoffProduct(
    id="BASKET_CALL",
    contract=q.when(t, q.cashflow("USD", q.maximum(basket - 100.0, 0.0))),
)

model = q.GbmBasket(
    observables=[a, b],
    s0=[100.0, 100.0],
    r=[0.03, 0.03],  # Debe ser la misma tasa para todos los activos en esta fase.
    q=[0.01, 0.02],
    sigma=[0.20, 0.25],
    correlation=[[1.0, 0.45], [0.45, 1.0]],
)

market = q.Market(pillars=[1.0], zero_rates=[0.03])
engine = q.Engine(n_paths=300_000, n_steps=252, seed=31)
price = engine.price(trade, model, market, ["PayoffPriceQ"])["PayoffPriceQ"].scalar
```

No usar con `GbmBasket` actualmente `PayoffExerciseQ`, `PayoffHitProbabilityQ`,
`PayoffExposureProfileQ`, bridge continuo o Theta MC.

### 4.7 Checklist de aceptación de un producto

- payoff comparado con ejemplos manuales y escenarios límite;
- invariantes económicos: paridades, monotonicidad, bounds y signos;
- convergencia por paths, pasos temporales y semilla;
- comparación con fórmula cerrada o implementación independiente cuando exista;
- modelo calibrado y unidades/convenciones documentadas;
- Greeks estables frente al tamaño de bump y al ruido MC;
- tratamiento explícito de moneda, descuento, fixings pasados y ejercicio;
- tests cross-layer Python/C++/Rust para las rutas críticas;
- errores explícitos para combinaciones producto-modelo-métrica no soportadas;
- `explain`/serialización canónica persistible para auditoría.

## 5. Lista 2: modelos a implementar, por fases

La prioridad no debe ser acumular muchos modelos, sino maximizar cobertura con pocos modelos bien
calibrados y una infraestructura común. “Modelo” incluye aquí las piezas de mercado sin las cuales
el proceso estocástico no puede usarse profesionalmente.

### Fase M0 — consolidar la base actual

- calibración robusta y versionada de Hull–White 1F/2F;
- calibración de GBM/GBM basket a curvas, dividendos y superficies observadas;
- curvas por moneda con bootstrapping, interpolación/extrapolación configurable;
- fixing store, calendarios, day count, schedules y dividendos discretos;
- resultados MC con error estándar, intervalo de confianza, antithetic/control variates y
  convergencia reproducible;
- hacer uniforme `ModelCapabilities` para Hull–White y el DSL de payoff.

**Desbloquea**: valoración fiable de lo ya soportado y elimina la principal deuda de producción.

### Fase M1 — cobertura vanilla multi-asset y multi-curva

- framework determinista multi-curve: OIS discounting, curvas IBOR/RFR de proyección y basis;
- Black–Scholes con curvas y dividendos term-structure;
- Garman–Kohlhagen para FX;
- Black-76 y Bachelier/normal para caps, floors y swaptions;
- superficies de volatilidad con strikes/deltas, smiles, arbitrage checks e implied vol;
- hazard curves piecewise y recuperación por entidad para crédito single-name.

**Desbloquea**: bonos/FRN, FRA, OIS/IRS/basis, FX swaps/forwards, vanillas equity/FX, caps/floors,
swaptions europeas y CDS.

### Fase M2 — smile y exóticos líquidos

- local volatility de Dupire;
- Heston y, si el caso de negocio lo exige, Bates con saltos;
- SABR para smiles de rates/FX y normal-SABR para mercados de tipos negativos;
- LMM/BGM o HJM multi-factor para productos dependientes de varios forwards;
- Hull–White extendido/calibrado a la curva inicial y volatilidades, no solo parámetros constantes;
- modelos multi-activo con curvas distintas, correlación term-structure y quanto adjustment;
- árboles/PDE 1D-2D y MC con bridge/generalized LSM seleccionables por pricer.

**Desbloquea**: barreras y asiáticas con smile, cliquets, autocallables estándar, callable bonds,
Bermudan swaptions, CMS aproximados y quantos.

### Fase M3 — híbridos, crédito y XVA

- framework cross-asset rates–FX–equity con factores correlacionados y numeraires explícitos;
- intensidad de default estocástica y dependencia exposición-default;
- copula/intensity multi-name para índices y tranches;
- collateral/CSA model, margin period of risk, funding curves e initial-margin model;
- exposure engine con valoración condicional/regresión o nested simulation;
- simulación histórica/real-world multivariante para VaR/ES, además de modelos Q.

**Desbloquea**: cross-currency exotics, convertibles simplificados, CDS options, basket credit y
CVA/DVA/FVA/MVA con netting y collateral.

### Fase M4 — especialización

- rough volatility o stochastic-local volatility cuando la evidencia de calibración lo justifique;
- modelos commodity multifactor con seasonality y convenience yield;
- inflación nominal-real con seasonality y lags;
- prepayment/default conductual para mortgages;
- optimal control especializado para swing/storage;
- surrogate models/deep BSDE solo con benchmark, bounds y política de validación de modelo.

**Desbloquea**: volatility derivatives complejos, energía, inflación, MBS y productos de control.

## 6. Lista 3: métricas a implementar, por fases

### Fase R0 — cerrar y homogeneizar lo existente

- PV/NPV, DV01 paralelo y bucketed, delta, gamma, vega, rho, theta y cross-gamma;
- EE, PFE95, CVA unilateral, hit probability, forecast P, VaR y ES;
- error estándar MC, intervalos de confianza, número de paths efectivo y diagnostics de LSM en la
  API pública tipada;
- cashflow/ledger, explain del payoff, dependencias y modelo/método utilizados;
- métricas con alias y resultados con unidades, moneda, medida Q/P y fecha de valoración;
- cobertura consistente de `all_greeks`, Hessian y HVP, con lista explícita de factores omitidos.

### Fase R1 — métricas de desk vanilla

- clean/dirty price, accrued, cashflows, yield, duration, convexity y carry/roll-down;
- par rate, fair spread, forward rate, basis y quote/PV conversion;
- implied volatility e implied correlation;
- delta/gamma por spot y FX, vega por tenor/strike, theta, charm, vanna, volga/vomma;
- PV01/DV01/KR01 y bucketed curve risk por curva de descuento y proyección;
- CS01 y recovery sensitivity para crédito;
- P&L explain diario por market move, carry, theta, new trades y residual;
- scenarios y stress tests deterministas con agregación de portfolio.

### Fase R2 — exóticos y control numérico

- probability of touch/no-touch, call probability por fecha y expected redemption time;
- barrier/event Greeks y discontinuity diagnostics;
- exercise boundary, continuation value, regression quality y upper/lower bounds;
- expected cashflows y distribución por fecha/moneda;
- pathwise/AAD Greeks y likelihood-ratio Greeks con fallback declarado;
- convergence por discretización, bias estimate, variance reduction gain y model-vs-pricer cross-check;
- smile risk: sticky-strike/sticky-delta, skew, curvature y surface buckets;
- correlation Greeks y cross-asset cross-gammas.

### Fase R3 — riesgo de contraparte y capital

- EPE, ENE, PFE por cuantiles configurables, expected positive/negative exposure y peak exposure;
- CVA, DVA, FVA, MVA y KVA, incrementales y por netting set;
- collateral profile, margin calls, initial margin, MPOR exposure y wrong-way-risk attribution;
- sensitivities XVA: CVA01/CS01, IR/FX/spot/vol sensitivities y XVA Greeks;
- VaR/ES histórico, paramétrico y MC, stressed VaR/ES, backtesting y exceptions;
- incremental/component/marginal VaR y ES;
- limits, concentration, liquidity horizon y stress aggregation.

### Fase R4 — validación y model risk

- reservas de modelo y prudent valuation/valuation uncertainty;
- comparación entre modelos/pricers, challenger model y benchmark independiente;
- calibration error, stability temporal y parameter uncertainty;
- Greeks por AAD vs bump-and-reval y reconciliación CPU/GPU;
- backtesting de precios, hedges y P&L attribution;
- audit trail completo: market snapshot, versión de modelo, calibración, semilla, hardware/backend,
  tolerancias y hash canónico de producto.

## 7. Roadmap integrado recomendado

| Fase | Productos prioritarios | Modelos/infraestructura | Métricas mínimas para Definition of Done |
|---|---|---|---|
| 0. Industrializar lo actual | IRS, vanillas, estrategias, barreras, asiáticas, baskets y bermudas ya existentes | M0 | R0, paridades, bounds, convergencia y benchmarks |
| 1. Vanilla profesional | Bonos/FRN, FRA, OIS/IRS/basis, FX, caps/floors, swaptions EU, CDS | M1 | R1 completo, calibración y P&L explain |
| 2. Exóticos líquidos | Autocallables, cliquets, callable/bermuda, barriers smile-aware, quantos | M2 | R2, smile/correlation risk y diagnostics de ejercicio |
| 3. Counterparty y portfolio | Netting sets, collateralized trades, híbridos y crédito | M3 | R3, escenarios a nivel cartera y atribución XVA |
| 4. Especializados | Volatilidad, commodities, inflación, convertibles, MBS, swing/storage | M4 por demanda | R4 y validación específica por modelo |

Cada fase debe considerarse terminada solo cuando producto, modelo y métricas llegan juntos. Añadir
un tipo de payoff sin calibración y riesgo no aumenta de forma material la cobertura profesional.

## 8. Prioridad práctica

El mayor incremento de cobertura por unidad de trabajo no vendrá inicialmente de añadir más nodos
al DSL. La secuencia recomendada es:

1. infraestructura de fechas, fixings, curvas y superficies;
2. multi-curve y modelos vanilla calibrables;
3. resultados numéricos y riesgo de desk completos;
4. modelos de smile y ejercicio robusto;
5. portfolio/netting/collateral y XVA;
6. modelos especializados guiados por demanda real.

Con M0/R0 se obtiene un engine demostrable y controlado. Con M1/R1 se alcanza un desk vanilla
serio. Con M2/R2 se cubre un desk de exóticos líquidos. M3/R3 es el salto a riesgo de cartera y
counterparty. M4/R4 debe tratarse como una familia de extensiones especializadas, no como requisito
previo para llamar profesional al núcleo vanilla.

## 9. Prototipos de DSL compatibles con el AST de payoff

### 9.1 Arquitectura recomendada

El AST `engine.payoff/v1` debe continuar siendo la única semántica y el único formato wire
versionado. Los distintos DSL son frontends de autoría, no engines de evaluación independientes:

```text
Python builders ─┐
                 ├─> AST tipado ─> JSON engine.payoff/v1 ─> ValidationVisitor ─> pricer
PayoffScript  ───┘
```

Esto permite ofrecer sintaxis más cómoda sin duplicar reglas de eventos, prioridad, settlement,
ejercicio o descuento. Cualquier frontend nuevo debe generar exactamente los nodos existentes,
pasar el mismo schema JSON y producir el mismo hash canónico que el frontend Python.

Se proponen tres representaciones compatibles:

1. **Python DSL**, ya implementado y recomendado para notebooks, tests y aplicaciones Python.
2. **JSON canónico**, ya implementado y recomendado para persistencia, REST, ABI y auditoría.
3. **PayoffScript**, prototipo textual opcional para usuarios de negocio. Debe ser solo un parser y
   desugarer hacia `engine.payoff/v1`.

### 9.2 Sistema de tipos mínimo

El lenguaje tiene tres categorías de nodo que no deben mezclarse implícitamente:

- `ScalarExpr -> float`: cantidades, precios observados, factores y resultados aritméticos;
- `Predicate -> bool`: condiciones;
- `Contract -> CashflowLedger`: obligaciones y derechos que generan cero o más cashflows.

Solo se permiten estas promociones implícitas:

- un `int`/`float` de Python se convierte en `Constant` al entrar en una expresión escalar;
- ninguna expresión escalar se convierte implícitamente en predicado;
- ningún escalar se convierte implícitamente en contrato: siempre se necesita `cashflow`;
- un `Cashflow` necesita un instante activo proporcionado por `When` o por un `Trigger` con
  settlement `at_hit`.

### 9.3 Compatibilidad de ejecución actual

| Marca | Significado |
|---|---|
| MC | Compila en `CompiledPayoff` y se puede usar en las rutas GBM Q/P compatibles |
| Escenario | Lo evalúa `ScenarioEvaluator` si el `MarketPath`, fixings y estado contienen los datos necesarios |
| AST | Se puede construir, validar y serializar, pero aún no tiene ejecución completa en los pricers públicos |

Los nodos `Parameter`, `EventTime`, `DiscountFactor`, `FxConversion`, `Before` y `After` forman
parte del schema, pero el compilador MC actual los rechaza en preflight. `ScenarioEvaluator` puede
resolver los cinco últimos si recibe el contexto completo. La API Python simplificada
`evaluate_scenario(trade, {observable: spot})` no permite todavía inyectar curvas, FX, parámetros
ni una trayectoria distinta por fecha. `Parameter` siempre debe sustituirse por un `Constant`
antes de evaluar.

### 9.4 Catálogo completo de `ScalarExpr`

El siguiente bloque construye un ejemplo de cada uno de los 23 nodos escalares actuales. Los
ejemplos muestran autoría; algunos necesitan estar dentro de un `When`/`Trigger` o disponer del
estado indicado para poder evaluarse.

```python
import quantdesk as q

S = "EQ.SPOT.AAPL"
T = 1.0

scalar_examples = {
    # Literales, parámetros y observaciones
    "Constant": q.constant(100.0),
    "Parameter": q.parameter("strike"),                 # AST; sustituir antes de valorar
    "Fixing": q.fixing(S, T),
    "Current": q.current(S),                            # requiere instante activo

    # Aritmética
    "Add": q.add(q.fixing(S, T), 10.0),
    "Sub": q.sub(q.fixing(S, T), 100.0),
    "Mul": q.mul(1_000.0, q.fixing(S, T)),
    "Div": q.div(q.fixing(S, T), 100.0),
    "Neg": q.neg(q.fixing(S, T)),
    "Abs": q.abs(q.fixing(S, T) - 100.0),
    "Exp": q.exp(0.03 * T),
    "Log": q.log(q.fixing(S, T) / 100.0),
    "Pow": q.pow(q.fixing(S, T), 2.0),
    "Min": q.minimum(q.fixing(S, T), 120.0),
    "Max": q.maximum(q.fixing(S, T) - 100.0, 0.0),
    "Clamp": q.clamp(q.fixing(S, T), 80.0, 120.0),

    # Agregados de trayectoria
    "Average": q.average(
        S,
        schedule=[0.25, 0.50, 0.75, 1.00],
        weights=[0.25, 0.25, 0.25, 0.25],
    ),
    "RunningMin": q.running_min(S),                     # requiere instante activo
    "RunningMax": q.running_max(S),                     # requiere instante activo

    # Estado de eventos y mercado
    "EventTime": q.event_time("KO"),                   # AST/Escenario; no MC todavía
    "EventValue": q.event_value("KO", S),              # requiere que KO capture S
    "DiscountFactor": q.discount_factor("USD.OIS", 0.0, T),
    "FxConversion": q.fx_conversion("EUR", "USD", T),
}
```

La sobrecarga de operadores produce exactamente los mismos nodos explícitos:

```python
explicit = q.mul(1_000.0, q.maximum(q.sub(q.fixing(S, T), 100.0), 0.0))
with_sugar = 1_000.0 * q.maximum(q.fixing(S, T) - 100.0, 0.0)

assert explicit.model_dump() == with_sugar.model_dump()
```

Notas semánticas:

- `Average` calcula la suma ponderada; los pesos no se normalizan implícitamente.
- `RunningMin`/`RunningMax` recorren las observaciones conocidas hasta el cursor activo. En MC,
  ese conjunto procede de los tiempos requeridos por el contrato, no de todos los pasos internos.
- `EventValue` captura el valor del observable en el primer hit del evento.
- `Div`, `Log` y otras operaciones con dominio restringido producen error explícito; nunca deben
  usarse suponiendo que el engine convertirá silenciosamente infinito/NaN en cero.

### 9.5 Catálogo completo de `Predicate`

```python
import quantdesk as q

S = "EQ.SPOT.AAPL"
spot = q.current(S)

predicate_examples = {
    "Greater": q.greater(spot, 100.0),
    "Less": q.less(spot, 100.0),
    "GreaterEqual": q.greater_equal(spot, 100.0),
    "LessEqual": q.less_equal(spot, 100.0),
    "Eq": q.eq(spot, 100.0, tolerance=1e-8),
    "All": q.all_of([
        q.greater_equal(spot, 90.0),
        q.less_equal(spot, 110.0),
    ]),
    "Any": q.any_of([
        q.less(spot, 80.0),
        q.greater(spot, 120.0),
    ]),
    "Not": q.negate(q.greater(spot, 100.0)),
    "Between": q.between(
        spot, 90.0, 110.0,
        low_inclusive=True,
        high_inclusive=False,
    ),
    "EventOccurred": q.event_occurred("KO"),
    "Before": q.before(0.5),                            # AST/Escenario; no MC todavía
    "After": q.after(0.5),                             # AST/Escenario; no MC todavía
}
```

`All` y `Any` tienen cortocircuito y exigen al menos un operando. `Eq` siempre exige una
tolerancia finita y no negativa.

### 9.6 Catálogo completo de `Contract`

```python
import quantdesk as q

S = "EQ.SPOT.AAPL"
T = 1.0
call_amount = q.maximum(q.fixing(S, T) - 100.0, 0.0)

zero_node = q.zero()
cashflow_node = q.cashflow("USD", 100.0)  # necesita When o Trigger(at_hit) como padre
give_node = q.give(q.when(T, q.cashflow("USD", 100.0)))
both_node = q.both([
    q.when(0.5, q.cashflow("USD", 10.0)),
    q.when(1.0, q.cashflow("USD", 110.0)),
])
scale_node = q.scale(2.0, q.when(T, q.cashflow("USD", call_amount)))
if_node = q.if_(
    q.greater_equal(q.current(S), 100.0),
    q.cashflow("USD", 10.0),
    q.zero(),
)
when_node = q.when(T, q.cashflow("USD", call_amount))

trigger_node = q.trigger(
    id="KO",
    monitoring_times=[0.25, 0.50, 0.75, 1.00],
    condition=q.greater_equal(q.current(S), 130.0),
    monitoring="discrete",
    settlement="at_hit",
    priority=0,
    latch=True,
    on_hit=q.cashflow("USD", 5.0),
    on_miss=q.when(T, q.cashflow("USD", call_amount)),
)

exercise_node = q.exercise(
    id="BERM_PUT",
    dates=[0.25, 0.50, 0.75],
    exercise_value=q.maximum(100.0 - q.current(S), 0.0),
    continuation=q.when(
        T,
        q.cashflow("USD", q.maximum(100.0 - q.fixing(S, T), 0.0)),
    ),
)

contract_examples = {
    "Zero": zero_node,
    "Cashflow": cashflow_node,
    "Give": give_node,
    "Both": both_node,
    "Scale": scale_node,
    "If": if_node,
    "When": when_node,
    "Trigger": trigger_node,
    "Exercise": exercise_node,
}
```

Consideraciones importantes:

- `Give` invierte todos los cashflows del hijo; no representa por sí mismo una compra/venta con
  fecha de contratación.
- `Both` combina ledgers y habilita netting matemático, pero no declara netting legal.
- `If` se evalúa en el cursor heredado. Por eso el ejemplo aislado debe insertarse dentro de un
  `When` o una rama `at_hit` antes de ser un contrato raíz válido.
- `Trigger(latch=True)` conserva el primer hit. `priority` resuelve eventos simultáneos junto con
  el orden canónico de ids.
- `settlement="at_hit"` proporciona como cursor el instante del hit. Con
  `settlement="at_scheduled_payment"`, la rama debe declarar sus propios `When`.
- La ruta pública de ejercicio admite actualmente exactamente un nodo `Exercise` y solo `Gbm`.

### 9.7 Ejemplo que combina expresiones, predicados, eventos y contratos

Este producto paga un rebate inmediato si toca la barrera y, si no la toca, paga al vencimiento
una call asiática limitada. Es completamente compatible con el AST y con la ruta GBM Q.

```python
import quantdesk as q

S = "EQ.SPOT.AAPL"
T = 1.0
schedule = [0.25, 0.50, 0.75, 1.00]
asian = q.average(S, schedule=schedule, weights=[0.25] * 4)
capped_call = q.minimum(q.maximum(asian - 100.0, 0.0), 30.0)

trade = q.PayoffProduct(
    id="AAPL_KO_ASIAN_CAPPED_CALL",
    contract=q.trigger(
        id="KO",
        monitoring_times=schedule,
        condition=q.greater_equal(q.current(S), 140.0),
        monitoring="discrete",
        settlement="at_hit",
        priority=0,
        latch=True,
        on_hit=q.cashflow("USD", 2.0),
        on_miss=q.when(T, q.cashflow("USD", 100.0 * capped_call)),
    ),
)

model = q.Gbm(s0=105.0, r=0.03, q=0.01, sigma=0.25, observable=S)
market = q.Market(pillars=[1.0], zero_rates=[0.03])
engine = q.Engine(n_paths=250_000, n_steps=252, seed=41)

metrics = [
    "PayoffPriceQ",
    ("PayoffHitProbabilityQ", {"event": "KO"}),
]
result = engine.price(trade, model, market, metrics)
```

### 9.8 JSON `engine.payoff/v1`

El equivalente JSON de una call es:

```json
{
  "schema": "engine.payoff/v1",
  "id": "AAPL_CALL_100",
  "contract": {
    "type": "when",
    "time": 1.0,
    "child": {
      "type": "cashflow",
      "currency": "USD",
      "amount": {
        "type": "mul",
        "left": { "type": "constant", "value": 1000.0 },
        "right": {
          "type": "max",
          "left": {
            "type": "sub",
            "left": { "type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0 },
            "right": { "type": "constant", "value": 100.0 }
          },
          "right": { "type": "constant", "value": 0.0 }
        }
      }
    }
  }
}
```

La correspondencia es mecánica: cada clase Python aporta `type` y sus campos. Puede inspeccionarse
el documento producido por cualquier trade sin reimplementar la serialización:

```python
import json
import quantdesk as q

trade = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", 100.0, 1_000.0, 1.0)
document = json.loads(trade.to_params()["spec"])
assert document["schema"] == "engine.payoff/v1"
```

El JSON es la API estable. No se deben persistir `repr()` de Python ni el texto PayoffScript sin
guardar también la versión del parser que lo compiló.

### 9.9 Prototipo `Payoff-S`: sintaxis lossless

Una sintaxis S-expression es el prototipo más pequeño y seguro porque mantiene una relación 1:1
con el AST. No pretende ser más cómoda, sino facilitar debugging, diffs y round-trip:

```lisp
(product "AAPL_CALL_100"
  (when 1.0
    (cashflow "USD"
      (mul (constant 1000.0)
           (max
             (sub (fixing "EQ.SPOT.AAPL" 1.0) (constant 100.0))
             (constant 0.0))))))
```

Reglas del prototipo:

- el primer símbolo es exactamente el discriminador `type` del JSON;
- los argumentos siguen el orden documentado de campos;
- listas como `Both.children`, `All.operands`, schedules y dates usan `[...]`;
- ids, observables, curvas y monedas siempre son strings;
- el parser solo construye el AST; validación y evaluación siguen en el engine.

Ejemplos adicionales:

```lisp
; Asiática
(product "ASIAN"
  (when 1.0
    (cashflow "USD"
      (max
        (sub (average "EQ.SPOT.AAPL" [0.25 0.50 0.75 1.00] [0.25 0.25 0.25 0.25])
             (constant 100.0))
        (constant 0.0)))))

; Barrera discreta
(product "UP_AND_OUT"
  (trigger "KO" [0.25 0.50 0.75 1.00]
    (greater_equal (current "EQ.SPOT.AAPL") (constant 130.0))
    discrete at_scheduled_payment 0 true
    (zero)
    (when 1.0
      (cashflow "USD"
        (max (sub (fixing "EQ.SPOT.AAPL" 1.0) (constant 100.0))
             (constant 0.0))))))

; Bermuda
(product "BERM_PUT"
  (exercise "EX" [0.25 0.50 0.75]
    (max (sub (constant 100.0) (current "EQ.SPOT.AAPL")) (constant 0.0))
    (when 1.0
      (cashflow "USD"
        (max (sub (constant 100.0) (fixing "EQ.SPOT.AAPL" 1.0))
             (constant 0.0))))))
```

### 9.10 Prototipo `PayoffScript`: sintaxis ergonómica

PayoffScript puede desazucarar variables locales, operadores y construcciones de negocio sin
añadir nodos al core:

```text
product AAPL_KO_ASIAN {
  observable S = "EQ.SPOT.AAPL";
  schedule obs = [0.25, 0.50, 0.75, 1.00];
  let asian = average(S, obs, equal_weights);
  let payoff = 100 * min(max(asian - 100, 0), 30);

  trigger KO monitor obs
    when current(S) >= 140
    mode discrete latch priority 0
    settle at_hit {
      pay USD 2;
    } else {
      at 1.00 pay USD payoff;
    }
}
```

Desugaring esperado:

- `let` expande el subárbol; no introduce estado mutable;
- `100`, `140`, etc. se convierten en `Constant`;
- operadores se convierten en `Add/Sub/Mul/Div` y comparaciones en predicados;
- `at T pay CCY expr` se convierte en `When(T, Cashflow(CCY, expr))`;
- `equal_weights` se materializa en un vector explícito antes de generar JSON;
- `trigger` se convierte en el nodo `Trigger` existente.

También puede haber macros de librería:

```text
product SPX_BUTTERFLY {
  include long_call("EQ.SPOT.SPX", 4900, 1.0, 0.5);
  include short_call("EQ.SPOT.SPX", 5000, 2.0, 0.5);
  include long_call("EQ.SPOT.SPX", 5100, 1.0, 0.5);
}
```

`long_call`/`short_call` no deberían convertirse en nuevos nodos: son macros que generan
`When + Cashflow + Max + Sub`. El resultado final debe poder mostrarse siempre como JSON o
Payoff-S para auditoría.

### 9.11 Contrato de un frontend DSL

Todo frontend nuevo debe cumplir:

- round-trip estable hacia `engine.payoff/v1`;
- source spans para traducir un `NodePath` de error a línea/columna del DSL;
- expansión de macros visible mediante `explain`;
- límites de profundidad/tamaño antes de materializar árboles;
- ids de evento únicos tras expandir macros;
- ausencia de evaluación propia: no calcular payoffs en el parser;
- misma canonicalización y mismo fingerprint para AST equivalentes;
- versionado independiente de sintaxis y schema (`payoffscript/v1 -> engine.payoff/v1`);
- tests golden DSL -> JSON y tests negativos contra el mismo catálogo de validación.

## 10. Posibles mejoras del lenguaje

### 10.1 Antes de añadir nodos: completar los existentes

La primera mejora de expresividad efectiva es cerrar la ejecución de nodos que ya están en el
schema:

1. **Binding de `Parameter`**: añadir `parameters: dict[str, float]` al producto o una fase de
   sustitución tipada. Debe fallar por parámetro ausente/no usado y quedar incluido en el hash.
2. **`EventTime` en MC**: exponer el tiempo del primer hit al intérprete compilado.
3. **`DiscountFactor` y `FxConversion` en MC**: incorporar curvas/FX al `MarketPath` y declarar
   sus dependencias/capacidades en el modelo.
4. **`Before`/`After` en MC**: el cursor ya existe; falta compilar estos predicados.
5. **Fixings históricos**: permitir valorar después de alguna fecha de observación sin rechazar
   Theta o recrear el contrato.
6. **`Average` y running extrema explícitos**: aclarar weighted sum vs weighted average y hacer
   explícita la malla de `RunningMin/RunningMax`.

Estas tareas aportan más que crear sinónimos de nodos que el evaluator tampoco podría ejecutar.

### 10.2 Nuevos `ScalarExpr` recomendados

| Prioridad | Nodo propuesto | Semántica | Productos que desbloquea o simplifica |
|---|---|---|---|
| P1 | `Sum(items)` / `Product(items)` | Reducción n-aria | Legs grandes, coupons, estrategias; evita árboles binarios profundos |
| P1 | `WeightedSum(values, weights)` | Suma ponderada general | Baskets y promedios de expresiones, no solo de un observable |
| P1 | `At(time, expr)` | Evalúa con cursor explícito | Elimina dependencia implícita de `Current` y facilita composición |
| P1 | `Select(condition, a, b)` | Condicional escalar | Digitales, rebates y fórmulas piecewise sin crear contratos artificiales |
| P1 | `FixingOr(observable, time, fallback)` | Fixing histórico o fallback declarado | Trades seasoned y valoración intradía |
| P1 | `IndexFixing(index, time)` | Fixing tipado de índice de tipos/inflación | FRN, OIS, inflación y coupons flotantes |
| P1 | `ForwardRate(curve, start, end, day_count)` | Forward derivado de curva | FRA, IRS multi-curve, caps/floors |
| P1 | `AccrualFactor(start, end, convention)` | Year fraction contractual | Elimina accruals precalculados opacos |
| P2 | `Fold(schedule, initial, update)` o acumuladores tipados | Estado path-dependent explícito | Cliquets, ratchets, TARN y coupons acumulativos |
| P2 | `Count(predicate, schedule)` | Número de hits | Parisian discretos, target coupons y conditions de persistencia |
| P2 | `RealizedVariance` / `RealizedVolatility` | Suma de retornos cuadrados | Variance/volatility swaps y options |
| P2 | `RealizedCovariance` / `Correlation` | Co-movimiento realizado | Correlation swaps y dispersion |
| P2 | `Return` / `LogReturn` | Retorno entre dos fechas | Cliquets, variance y performance notes |
| P2 | `NthOrderStatistic(values, n)` | k-th best/worst | Rainbow, best-of/worst-of generalizados |
| P2 | `SwapRate` / `Annuity` | Rate y numerario de swap | Swaptions/CMS y ejercicio sobre swap subyacente |
| P3 | `SurvivalProbability` / `DefaultIndicator` | Estado crediticio | CDS, credit-linked notes y XVA |
| P3 | `InflationRatio` | Índice con lag/interpolación | ZC/YoY inflation swaps y caps |

`Fold` es potente pero peligroso: necesita un lenguaje de transición restringido, determinista y
analizable por `DependencyVisitor`. No debe aceptar callbacks Python ni código arbitrario.

### 10.3 Nuevos `Predicate` recomendados

| Prioridad | Nodo propuesto | Uso |
|---|---|---|
| P1 | `CrossesAbove` / `CrossesBelow` | Distinguir cruce de mera permanencia sobre una barrera |
| P1 | `AtLeast(n, predicates)` | K-of-N, baskets de condiciones y triggers de crédito |
| P1 | `HasFixing(observable, time)` | Fallback explícito entre histórico y forecast |
| P2 | `Consecutive(predicate, count, schedule)` | Barreras Parisian y condiciones de persistencia |
| P2 | `OccurredBefore(event_a, event_b)` | Productos first-to-default/first-hit sin guardas manuales |
| P2 | `CountBetween(predicate, min, max, schedule)` | Coupons digitales y range accruals |
| P2 | `InWindow(start, end)` | Ventanas de ejercicio/observación más legibles |
| P3 | `Defaulted(entity)` / `RatingIn(...)` | Crédito y rating triggers |

Los comparadores aproximados (`Eq`) deben seguir exigiendo tolerancia. No conviene añadir igualdad
float exacta como azúcar.

### 10.4 Nuevos `Contract` recomendados

| Prioridad | Nodo propuesto | Semántica | Productos |
|---|---|---|---|
| P1 | `Transfer(asset, quantity)` | Entrega física, no cashflow monetario | Forwards físicos, options physical-settled, bonos convertibles |
| P1 | `Leg(schedule, amount_expr, currency)` | Generador compacto de cashflows | Bonos, swaps, FRN; puede ser macro si no necesita semántica propia |
| P1 | `Choice(id, dates, alternatives)` | Elección entre varios contratos | Callable/putable bonds y opciones con settlement alternativo |
| P1 | `Cancel(id, dates, child)` | Derecho a terminar cashflows futuros | Cancellable swaps y callable structures |
| P2 | `MultipleExercise(id, dates, rights, child)` | Varias unidades ejercitables | Swing y ejercicio parcial |
| P2 | `Accumulate(target, child, on_target)` | Termina al alcanzar un target | TARN y target redemption notes |
| P2 | `Notice(exercise_date, settlement_date, child)` | Separa decisión y settlement | Bermudas y callables reales |
| P2 | `DefaultSettlement(entity, recovery, child)` | Closeout contractual por default | CDS/CLN y XVA |
| P3 | `Collateralized(csa_id, child)` | Anota reglas de colateral | Exposición/XVA; probablemente metadata, no payoff económico |

`Leg`, `Barrier`, `Asian`, `Call`, `Put`, `Autocall` o `IRS` deberían empezar como plantillas o
macros. Solo merecen un nodo core si aportan una semántica que no puede preservarse al expandirlos,
si el patrón expandido es demasiado grande, o si un pricer especializado necesita reconocerlo de
forma estable. El `SpecializationVisitor` puede reconocer patrones canónicos sin contaminar el
núcleo con un nodo por producto comercial.

### 10.5 Mejoras transversales del AST

- **Fechas reales**: `LocalDate`, calendarios, business-day convention, day count, lags y schedule
  generation; mantener year fractions como resultado compilado, no como contrato fuente.
- **Tipos dimensionales**: `Money<CCY>`, `Rate`, `Price`, `Quantity`, `Date` y `YearFraction` para
  detectar sumas de USD con EUR o rates con notionals antes de simular.
- **Observables tipados**: spot, índice, fixing, curva, vol, hazard y recovery, en lugar de que
  todo sea un string libre.
- **Moneda de reporting y settlement** explícitas, separadas de la moneda de cada cashflow.
- **Ámbito de variables** y `Let` como construcción de autoría con DAG/CSE interno; el hash debe
  ser independiente del nombre local elegido.
- **Metadata no semántica**: labels de negocio, source spans y comentarios excluidos del hash
  económico pero conservados para `explain` y errores.
- **Feature/version negotiation**: cada modelo/pricer publica nodos, medidas y versiones que
  soporta; el preflight informa todos los gaps de una vez.
- **Cost model**: estimar número de observaciones, estados, paths y memoria antes de ejecutar.
- **Normalización**: aplanar `Both`, `All`, `Any`, `Sum`; eliminar `Zero`; plegar constantes y
  detectar subárboles comunes sin cambiar ids/eventos.
- **Migraciones de schema** explícitas y testeadas entre `engine.payoff/v1`, `v2`, etc.

### 10.6 Fases sugeridas para evolucionar el DSL

| Fase | Trabajo | Criterio de aceptación |
|---|---|---|
| DSL-0 | Catálogo formal, golden files y matriz nodo-pricer | Cada nodo tiene round-trip Python/JSON, ejemplo positivo y error negativo |
| DSL-1 | Ejecutar nodos existentes pendientes, parameters y fixings históricos | Ningún nodo de v1 queda en estado “serializable pero no ejecutable” sin capability explícita |
| DSL-2 | `Sum`, `WeightedSum`, `At`, `Select`, rates/index fixings y fechas | Bonos, FRN, FRA e IRS multi-curve se expresan sin plantillas opacas |
| DSL-3 | Acumuladores, realized variance, crossing/persistence y ejercicio generalizado | Cliquets, TARN, variance y callables tienen semántica nativa y tests de path |
| DSL-4 | Transferencias físicas, crédito, collateral y tipos dimensionales | Híbridos/XVA fallan en preflight ante cualquier inconsistencia de unidad o capability |
| DSL-5 | PayoffScript y tooling IDE | Parser con source spans, formatter, linter, autocomplete, explain y golden DSL -> JSON |

La regla de evolución debe ser: **primero semántica y ejecución, después azúcar sintáctico**. Un
nodo nuevo solo se considera implementado cuando lo entienden schema, Python, parser JSON,
validación, dependencias, canonicalización, explain, evaluator de escenario, compilador/pricer,
sensibilidades y tests cross-layer; de lo contrario debe publicarse como macro experimental.
