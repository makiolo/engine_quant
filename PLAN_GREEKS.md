# PLAN_GREEKS.md — motor universal de sensibilidades (Greeks) sobre cualquier métrica

> Documento de arquitectura y plan de implementación. Complementa a [PLAN_PRODUCTS.md](PLAN_PRODUCTS.md)
> (que resuelve "cualquier producto" mediante un AST de payoff componible) resolviendo el eje
> ortogonal: "cualquier sensibilidad de cualquier métrica, bajo Q o P". Este documento no declara
> implementadas las fases: cada una se migra a `PLAN.md` únicamente después de quedar construida y
> verificada de extremo a extremo, mismo criterio editorial que PLAN_PRODUCTS.md.

## 0. Decisión ejecutiva

El motor no debe implementar una clase/medida nueva por cada combinación de (producto, modelo,
métrica, parámetro de riesgo). Eso ya casi ocurrió una vez: `Dv01Measure` (bump paralelo/bucketed
de curva, solo para IRS y PV/Payoff), `payoff::sensitivity` (4 parámetros de `Gbm`, solo para el
precio del payoff), `irs_hull_white_npv_delta_r0` (AAD reverse-mode, pero solo respecto de `r0`,
solo para NPV) y las "Greeks residuales" del hedge (§11 de PLAN_PRODUCTS.md) son hoy CUATRO
implementaciones independientes del mismo concepto -- sensibilidad de una métrica respecto de un
parámetro -- con cobertura parcial y sin relación entre sí (ver §1).

Igual que PLAN_PRODUCTS.md separó "producto" de "payoff AST", este documento separa **sensibilidad**
en tres piezas ortogonales:

1. la **métrica base** (`PV`, `PayoffPriceQ`, `PayoffHitProbabilityQ`, `ExposureProfile`,
   `PayoffForecastP`, `PayoffPnlDistributionP`, ...) -- ya existe como `IMeasure` registrada;
2. el **factor de riesgo** respecto del cual se deriva (`RiskFactor`): un parámetro de modelo, un
   punto/tramo de curva, un parámetro de crédito, o el paso del tiempo;
3. el **método** de cálculo (`BumpAndReval` | `Pathwise` | `AadReverse`), elegido automáticamente
   o forzado.

La ruta **universal y correcta** es bump-and-reval genérico: repite la evaluación de la MISMA
métrica registrada, con el modelo/mercado desplazado en el factor de riesgo pedido, y siempre
funciona porque no asume nada sobre la métrica salvo que sea invocable dos o tres veces con
parámetros ligeramente distintos. Las rutas pathwise (`Dual`) y AAD reverse-mode (`Autodiff` de
Burn) son **especializaciones opcionales** que aceleran/reducen ruido para combinaciones concretas
de (modelo, métrica, factor) y que deben verificarse contra bump-and-reval antes de convertirse en
el método por defecto -- exactamente la misma disciplina que PLAN_PRODUCTS.md §0 aplicó a
"ruta genérica primero, especialización comprobada después".

### 0.1 Qué significa y qué no significa «cualquier griega de cualquier métrica»

Realista:

- cualquier `IMeasure` ya registrada puede pedirse derivada respecto de cualquier parámetro de
  modelo que ese modelo declare, de cualquier punto de la curva de descuento que el producto
  consuma, y (Fase 5) del paso del tiempo;
- primer orden (Delta, Vega, Rho, DV01, sensibilidad de una probabilidad de hit, de un forecast P,
  de un VaR/ES...) para toda combinación soportada, con verificación diferencial documentada;
- segundo orden (Gamma) y cruzado (Vanna, Volga, cross-gamma) vía el mismo motor genérico, con
  coste adicional documentado (no gratis: 2-4 evaluaciones extra por greek).

No significa:

- que toda métrica sea diferenciable en el sentido pathwise -- una probabilidad de hit o un payoff
  digital son funciones indicador: su derivada pathwise exacta es 0 en casi todo punto y no
  informa nada (§5.1); ahí el motor usa bump-and-reval (con más paths si hace falta), nunca finge
  una derivada pathwise inexistente;
- riesgo de correlación/base multi-activo -- el motor de payoff sigue siendo de un único
  observable por contrato compartido (ver `payoff::hedge`, PLAN_PRODUCTS.md §12 Fase 11); una
  "griega de correlación" real exige un modelo multi-activo que no existe hoy;
- sensibilidades a parámetros de CONTRATO (strike, nivel de barrera) como si fueran factores de
  mercado -- son útiles como herramienta de "qué pasa si" pero no son Greeks en el sentido de
  riesgo de mercado; se tratan aparte y fuera del alcance inicial (§15).

## 1. Estado actual y puntos de integración

El estado actual condiciona el diseño (cuatro islas, sin abstracción común):

- `cpp/engine/src/measure.cpp::Dv01Measure` -- bump paralelo o "bucketed" (un bump por pillar) de
  la curva de descuento, solo para `IrSwapProduct` (bucketed) y `PayoffProduct`/`IrSwapProduct`
  (paralelo). Pedir `bucketed=true` para un `PayoffProduct` lanza explícito ("Fase 8" pendiente) --
  el motor no expone hoy los pillars de la curva por separado para payoff.
- `cpp/engine/include/engine/payoff/measures.hpp::bump_and_reval_curve` /
  `bump_and_reval_fixing` -- bump-and-reval genérico pero **manual**: cada llamante construye a
  mano el `MarketPath` bumpeado y vuelve a evaluar `present_value`. No hay una función que, dado
  un nombre de métrica y un nombre de factor de riesgo, decida sola qué bumpear.
- `rust/crates/engine-core/src/payoff/sensitivity.rs` (`payoff::api::payoff_sensitivity_gbm_q`,
  medida `PayoffSensitivityQ`) -- método pathwise (`Dual`, forward-mode de un solo parámetro a la
  vez) para exactamente 4 parámetros de `Gbm` (`spot`/`rate`/`dividend_yield`/`volatility`),
  aplicado **solo a la métrica de precio** (`PV`/`PayoffPriceQ`) del payoff, con fallback
  bump-and-reval si el contrato tiene `Exercise`. No existe para `GbmP`
  (`PayoffForecastP`/`PayoffHitProbabilityP`/`PayoffPnlDistributionP`) ni para ninguna medida
  distinta del precio.
- `rust/crates/engine-core/src/api.rs::irs_hull_white_npv_delta_r0`(`_batch`) /
  `irs_hull_white_2f_npv_delta_r0`(`_batch`) -- AAD **reverse-mode** de verdad (tensores
  `Autodiff<CpuBackend>` de Burn), pero solo respecto de `r0` y solo para el NPV determinista
  replicado en bonos cero-cupón. Cruza a C++ (`engine::irs_hull_white_npv_delta_r0` en
  `engine.hpp`/`.cpp`) y de ahí incluso a Python (`hull_white_zero_coupon_bond_delta_r0` en
  `engine_py_ext.cpp`) como **función suelta**, sin pasar nunca por `Registry<IMeasure>`. Un
  reverse-mode ya construido para `r0` es, con el mismo grafo de tensores, prácticamente gratis de
  extender a `a`/`b`/`sigma`(/`eta`/`rho` en 2F) -- es la ruta más barata para "todas las Greeks de
  un modelo en una sola pasada", pero hoy solo se usa para un parámetro y solo en tests
  (`tests/aad_vs_bump_reval.rs`).
- `rust/crates/engine-core/src/payoff/hedge.rs::HedgeResidualGreeks` (PLAN_PRODUCTS.md §12 Fase
  11) -- bump-and-reval de un PORTFOLIO agregado (target + instrumentos con pesos ya resueltos)
  respecto de los mismos 4 parámetros de `Gbm`, con números aleatorios comunes. Es, en espíritu,
  exactamente el motor genérico que este documento propone, aplicado a un único caso (la cartera
  cubierta) -- prueba de que el patrón funciona y de que vale la pena generalizarlo.
- No existe Theta (paso del tiempo) en ningún punto del motor.
- No existen segundas derivadas (Gamma) ni derivadas cruzadas (Vanna/Volga) en ningún punto.
- `engine::MeasureSpec{name, Params}` (`price.hpp`) ya es el mecanismo por el que una medida
  recibe configuración (`Dv01Measure::bump_`/`bucketed_`, `PayoffHitProbabilityQMeasure::event_`,
  `PayoffPnlDistributionPMeasure::confidence_`) sin que `price()`/`price_batch`/`price_many`/
  `price_grid`/Python/Excel/C ABI necesiten ningún cambio adicional -- es el punto de extensión
  natural para una medida genérica de Greeks (§8), evitando repetir el riesgo ya señalado en
  PLAN_PRODUCTS.md §15 ("explosión de combinaciones").
- `Params` (`params.hpp`) es un `variant<double, vector<double>, bool, string>` plano, sin
  anidamiento -- mismo motivo por el que PLAN_PRODUCTS.md §7.2 decidió no ampliar `ParamValue` con
  variantes recursivas para el AST de payoff. Este documento hereda esa misma restricción (§8.2).

## 2. Arquitectura objetivo

```text
                    Metrica base (IMeasure registrada: "PV", "PayoffPriceQ",
                    "PayoffHitProbabilityQ", "ExposureProfile", "PayoffForecastP", ...)
                                        |
                                        v
                              GreekRequest
                  (risk_factor, orden, cruce opcional, metodo)
                                        |
                                        v
                    RiskFactor (catalogo tipado, namespaced)
                 /            |              |             \
        ModelParameter   CurveRisk     CreditRisk      TimeShift
        (Gbm.spot,       (parallel,    (hazard_rate,   (theta:
        HW1F.sigma...)   pillar[i])    recovery_rate)  valuation_date+dt)
                                        |
                                        v
                              GreekEngine (dispatch)
                  /                     |                      \
     BumpAndReval generico      Pathwise (Dual)          AadReverse (Burn Autodiff)
     (SIEMPRE aplicable,        (verificado, solo        (verificado, solo
      metodo de referencia)     donde la metrica es      donde el modelo expone
                                 Lipschitz en el          un grafo de tensores
                                 parametro, no            diferenciable -- HW hoy)
                                 indicador/discontinua)
                                        |
                                        v
                              GreekResult
        (valor, orden, factor, metodo usado, medida Q/P/Deterministic,
         std_error si Monte Carlo, bump usado si aplica, warnings)
```

Tres piezas, igual que PLAN_PRODUCTS.md distinguía AST/IR/estado:

1. **Catálogo de factores de riesgo** (`RiskFactor`): tipado, con parseo desde string namespaced y
   validado antes de evaluar (mismo espíritu que `ObservableId` en PLAN_PRODUCTS.md §3.1: "una
   ausencia es error explícito, nunca `0.0` silencioso").
2. **Motor de bump-and-reval genérico**: no conoce el tipo concreto de modelo/producto/métrica;
   solo sabe reconstruir un modelo/mercado bumpeado a partir de sus `Params` y volver a invocar la
   medida por nombre a través de `Registry<IMeasure>`.
3. **Especializaciones verificadas**: pathwise (`Dual`) y AAD reverse-mode (`Autodiff`), activadas
   solo cuando una tabla de capacidades declara que (modelo, métrica, factor) las soporta, y
   siempre con un test diferencial que las compara contra bump-and-reval.

## 3. Modelo de dominio

### 3.1 `RiskFactor`

Strings namespaced en la API pública (igual que `ObservableId`/nombres de medida), convertidos a
un tipo validado antes de evaluar:

```cpp
enum class RiskFactorKind { ModelParameter, CurveParallel, CurvePillar, CreditParameter, TimeShift };

struct RiskFactor {
    RiskFactorKind kind;
    std::string scope;   // "model" | nombre de curva | "credit" | "time"
    std::string name;    // "spot" | "sigma" | "hazard_rate" | "" (parallel/time no lo usan)
    std::optional<std::size_t> pillar_index;  // solo CurvePillar
};
```

Convención de strings (parseo estricto, rechaza lo desconocido con mensaje que nombra la cadena
recibida y las alternativas válidas, mismo criterio que `GbmGreek::parse`):

| String | `RiskFactor` |
|---|---|
| `"model.spot"`, `"model.rate"`, `"model.dividend_yield"`, `"model.volatility"` | `ModelParameter` (Gbm/GbmP) |
| `"model.a"`, `"model.b"`, `"model.sigma"`, `"model.r0"` | `ModelParameter` (HullWhite1F) |
| `"model.eta"`, `"model.rho"` | `ModelParameter` (HullWhite2F, además de a/b/sigma/r0) |
| `"curve.parallel"` | `CurveParallel` (bump paralelo de `zero_rates`) |
| `"curve.pillar:<i>"` | `CurvePillar` (bump de un único pillar `i`) |
| `"credit.hazard_rate"`, `"credit.recovery_rate"` | `CreditParameter` |
| `"time.theta"` | `TimeShift` (§7) |

`ModelParameter` no fija de antemano la lista de nombres válidos por tipo de modelo: los valida
`ModelParameterRegistry` (§3.1.1), una tabla `model_type -> {nombres válidos}` construida a partir
de `IModel::to_params()` (§4.2) -- añadir un modelo nuevo con parámetros nuevos no exige tocar el
parser de `RiskFactor`, solo declarar sus params como ya hace hoy `HullWhite1FModel(const
Params&)`.

### 3.2 `GreekOrder`

```cpp
struct GreekOrder {
    int order = 1;                              // 1 (Delta/Vega/Rho/DV01...) o 2 (Gamma)
    std::optional<RiskFactor> cross_factor;      // presente => derivada cruzada (Vanna, cross-gamma)
};
```

Reglas: `order == 2` con `cross_factor` ausente es una segunda derivada pura (Gamma respecto del
mismo factor); `order == 1` con `cross_factor` presente es una derivada cruzada de primer orden en
cada variable (Vanna = `d^2V/dSpot.dVol`); `order == 2` con `cross_factor` presente se rechaza en
v1 (tercera derivada, fuera de alcance, ver §15).

### 3.3 `GreekMethod`

```cpp
enum class GreekMethod { Auto, BumpAndReval, Pathwise, AadReverse };
```

`Auto` (default): el motor consulta una tabla de capacidades (§5.4) y usa la especialización más
rápida disponible y verificada para (tipo de modelo, nombre de métrica, `RiskFactor`, `GreekOrder`);
si no hay ninguna, cae a `BumpAndReval`. Pedir explícitamente `Pathwise`/`AadReverse` sobre una
combinación no soportada es un error explícito (nunca degrada en silencio a bump-and-reval sin que
el llamante lo sepa: si además quiere el resultado, debe pedir `Auto` o `BumpAndReval`).

### 3.4 `GreekRequest` / `GreekResult`

```cpp
struct GreekRequest {
    std::string metric_name;        // nombre ya registrado en Registry<IMeasure>, p.ej. "PayoffPriceQ"
    Params metric_params;           // los propios de esa medida (event/confidence/exposure_times...)
    RiskFactor risk_factor;
    GreekOrder order;
    GreekMethod method = GreekMethod::Auto;
    std::optional<double> bump_override;   // tamaño de bump explícito; ausente = política por defecto (§4.3)
};

struct GreekResult {
    double value = 0.0;
    std::optional<double> std_error;        // presente si la métrica base es Monte Carlo
    GreekOrder order;
    RiskFactor risk_factor;
    GreekMethod method_used;                // el que realmente se ejecutó, nunca ambiguo
    ProbabilityMeasure measure;             // heredada de la métrica base (Q/P/DeterministicScenario)
    std::optional<double> bump_used;        // ausente si method_used == Pathwise/AadReverse
    std::vector<std::string> warnings;      // p.ej. "bump-and-reval sobre una metrica tipo indicador: ruido Monte Carlo domina para N paths bajo"
};
```

`GreekResult` vive como struct C++ rico (análogo a `QValuationResult`/`ExercisePolicyResult` de
PLAN_PRODUCTS.md) devuelto por una función libre `engine::greeks::compute_greek(...)`; la medida
registrada `"Greek"` (§8) es un envoltorio delgado sobre esa función que aplana el resultado a
`MeasureResult`, exactamente el mismo patrón "función libre rica + `IMeasure` delgado" que
`payoff::risk_neutral_price_gbm` / `PayoffPriceQMeasure` ya establecen.

## 4. Motor de bump-and-reval genérico (la ruta universal)

### 4.1 Idea central

`IMeasure::evaluate(model, product, market, pricing, execution)` YA ES el "evaluador de métrica"
genérico: no hace falta inventar una interfaz nueva, hace falta poder invocarlo dos o tres veces
con un `model`/`market` ligeramente distinto y sin que el llamante necesite conocer el tipo
concreto de `IModel`/`MarketSnapshot`.

```text
compute_greek_bump_and_reval(registries, request, product, model, market, pricing, execution):
    metric = registries.measures.create(request.metric_name, request.metric_params)
    base   = metric->evaluate(model, product, market, pricing, execution)   # solo si se necesita (orden 2 con stencil centrado no siempre lo necesita)

    if request.risk_factor.kind in {ModelParameter}:
        model_up   = bump_model(model, request.risk_factor, +h)
        model_down = bump_model(model, request.risk_factor, -h)
        up   = metric->evaluate(model_up,   product, market,     pricing, execution)
        down = metric->evaluate(model_down, product, market,     pricing, execution)
    else:  # CurveParallel, CurvePillar, CreditParameter
        market_up   = bump_market(market, request.risk_factor, +h)
        market_down = bump_market(market, request.risk_factor, -h)
        up   = metric->evaluate(model, product, market_up,   pricing, execution)
        down = metric->evaluate(model, product, market_down, pricing, execution)

    value = (up.scalar - down.scalar) / (2*h)                      # orden 1
    if request.order.order == 2:
        value = (up.scalar - 2*base.scalar + down.scalar) / (h*h)  # orden 2 (mismo stencil, reusa up/down/base)
```

Diferencia central (no adelantada) por defecto: mismo coste (2 evaluaciones) que una diferencia
adelantada, cancela el error de primer orden en `h` y es el estándar de facto para Greeks
financieras -- y es el estencil que además da Gamma "gratis" con una única evaluación extra
(`base`), en vez de tener que pedir 4 evaluaciones para Delta+Gamma por separado.

### 4.2 `bump_model` / `bump_market`: reconstrucción vía `Params`, no dynamic_cast por modelo

Para no repetir un `dynamic_cast` a cada tipo concreto de modelo por cada parámetro nuevo (el
mismo riesgo de combinatoria que PLAN_PRODUCTS.md §15 señala para productos), se añade **una única
extensión aditiva** a `IModel`:

```cpp
class IModel {
public:
    virtual ~IModel() = default;
    virtual std::string type_name() const = 0;
    virtual std::optional<payoff::ModelCapabilities> capabilities() const { return std::nullopt; }
    // Nuevo (PLAN_GREEKS.md §4.2): serializa los Params con los que se reconstruiria un modelo
    // identico via Registry<IModel>::create(type_name(), to_params()). Todo modelo YA se
    // construye desde un Params (HullWhite1FModel(const Params&), GbmModel(const Params&)...);
    // to_params() es la operacion inversa, y con ella "bumpear un parametro" se reduce a
    // "leer el double de esa clave, sumarle h, reconstruir" -- sin un metodo virtual nuevo por
    // parametro ni por modelo.
    virtual Params to_params() const = 0;
};
```

```text
bump_model(model, risk_factor, h):
    params = model.to_params()
    if risk_factor.name not in params or not holds double:
        throw invalid_argument("modelo '<type_name>' no tiene el parametro '<risk_factor.name>'")
    params[risk_factor.name] += h
    return registries.models.create(model.type_name(), params)
```

`bump_market` es más simple porque hoy solo existe una clase `MarketSnapshot` concreta (no hace
falta polimorfismo): construye una copia con `zero_rates` desplazados (paralelo o un único pillar)
o `hazard_rate`/`recovery_rate` desplazados, reutilizando el constructor `MarketSnapshot(pillars,
zero_rates, hazard_rate, recovery_rate)` que ya existe.

Este diseño es **aditivo**: cada `IModel` existente (`HullWhite1FModel`, `HullWhite2FModel`,
`GbmModel`, `GbmPModel`) ya guarda sus parámetros como miembros `double` construidos desde
`Params` -- implementar `to_params()` es mecánico y no cambia ningún comportamiento existente
(mismo patrón que `payoff_program()` devolviendo `nullptr` por defecto en PLAN_PRODUCTS.md §9.1).

### 4.3 Política de tamaño de bump

Tabla de defaults por tipo de factor (unifica los valores que hoy están dispersos y hardcodeados
en tres sitios distintos del código: `Dv01Measure::bump_` default `0.0001`,
`payoff_sensitivity_bump_and_reval::RELATIVE_BUMP`/`MIN_ABSOLUTE_BUMP` = `1e-2`/`1e-4`, y el mismo
par de constantes duplicado en `hedge::hedge_residual_greeks_on`):

| `RiskFactorKind` | Bump por defecto | Relativo/absoluto |
|---|---|---|
| `ModelParameter` (spot, sigma, s0, a, b...) | `max(1e-2 * |valor|, 1e-4)` | relativo con piso absoluto |
| `CurveParallel` / `CurvePillar` | `0.0001` (1 punto básico) | absoluto |
| `CreditParameter` (hazard/recovery) | `0.0001` | absoluto |
| `TimeShift` | `1/365` (un día) | absoluto, ver §7 |

`GreekRequest::bump_override` sustituye el default para ese request; el motor SIEMPRE reporta
`bump_used` en `GreekResult` -- nunca deja que el llamante adivine qué bump se aplicó.

### 4.4 Números aleatorios comunes (obligatorio para métricas Monte Carlo)

Toda evaluación bumpeada de una métrica Monte Carlo (payoff bajo Q/P, exposición IRS) DEBE usar el
mismo `seed`/`n_paths` de `PricingContext` que la evaluación base -- ya es la práctica de
`bump_and_reval_curve`/`bump_and_reval_fixing`/`payoff_sensitivity_bump_and_reval`/
`hedge_residual_greeks_on`, aquí se convierte en invariante del motor genérico, no una convención
que cada sitio nuevo tiene que recordar. Sin números aleatorios comunes, el ruido Monte Carlo de
dos simulaciones independientes puede dominar por completo la señal de un bump pequeño (ver
`payoff_sensitivity_bump_and_reval`, que documenta exactamente este problema).

### 4.5 Métricas tipo indicador/discontinuas: bump-and-reval sigue funcionando, pathwise no

Una probabilidad de hit (`PayoffHitProbabilityQ`/`P`) o un payoff digital son, ruta a ruta, una
función indicador de un umbral: su derivada exacta es 0 en casi todo punto y no informa nada (el
método pathwise de §5.1 exige que la cantidad sea Lipschitz/diferenciable en el parámetro,
condición que un vainilla/asian/barrier cumple pero un indicador puro no). Bump-and-reval sigue
siendo correcto ahí porque no diferencia la ruta individual: agrega sobre MUCHAS rutas, y el
promedio de un indicador SÍ varía suavemente con el parámetro (es, de hecho, la probabilidad
misma). El motor debe:

- declarar `Pathwise` como NO soportado para toda métrica de tipo probabilidad/indicador (tabla de
  capacidades, §5.4) -- pedirlo explícitamente falla, nunca se aproxima en silencio;
- documentar en `GreekResult::warnings` cuando `BumpAndReval` se usa sobre una métrica de este
  tipo con pocos paths, porque el ruido relativo de una probabilidad estimada por indicador es
  mayor que el de un precio (regla práctica, no un cálculo exacto de potencia estadística).

## 5. Rutas especializadas (aceleración verificada)

### 5.1 Pathwise (`Dual`, forward-mode de un parámetro)

Generaliza `payoff::sensitivity` existente:

- se reemplaza el enum cerrado `GbmGreek` (4 variantes) por el `RiskFactor::ModelParameter`
  unificado de §3.1, sin cambiar el mecanismo interno (`GbmDualPath`, `eval_scalar_dual`,
  `eval_contract_dual` siguen igual);
- se EXTIENDE a `GbmP` (mismo mecanismo, mismos 3 parámetros `s0`/`mu`/`sigma`, para
  `PayoffForecastP`) -- hoy solo existe para `Gbm`/Q;
- capacidad declarada explícitamente por (tipo de modelo, familia de payoff): un contrato con
  `ContractOp::Exercise` sigue cayendo al fallback bump-and-reval (razón ya documentada en
  `payoff::sensitivity`: re-decidir Longstaff-Schwartz bajo el parámetro perturbado cambia la
  política completa, el método pathwise no aplica limpiamente ahí);
- un contrato cuyo `DependencyReport` indica que la métrica pedida es de tipo
  probabilidad/indicador (`PayoffHitProbabilityQ/P`) NUNCA declara soporte pathwise (§4.5).

### 5.2 AAD reverse-mode (`Autodiff<CpuBackend>` de Burn)

Generaliza `irs_hull_white_npv_delta_r0`: hoy el grafo de tensores ya existe y ya se diferencia en
reverse-mode respecto de `r0`; extenderlo a `a`/`b`/`sigma` (y `eta`/`rho` en HullWhite2F) es
marcar esos mismos tensores como variables del grafo en vez de constantes -- el coste de una
pasada `backward()` no depende de CUÁNTOS parámetros se piden (esa es la ventaja frente al método
pathwise forward-mode, que necesita una pasada por parámetro): una sola llamada puede devolver
Delta+Rho+Vega+... de Hull-White de una vez. Se expone como:

```rust
pub fn irs_hull_white_npv_all_greeks(a: f64, b: f64, sigma: f64, r0: f64, /* trade+market... */)
    -> HullWhiteGreeks { pub d_a: f64, pub d_b: f64, pub d_sigma: f64, pub d_r0: f64 }
```

y el motor genérico, cuando detecta `method=Auto` y modelo `HullWhite1F`/`2F` con métrica `PV`,
llama a esta función UNA vez y sirve cualquiera de sus componentes en vez de repetir bump-and-reval
por cada `RiskFactor` pedido -- optimización de "una pasada, todas las Greeks del modelo" que no
existe hoy ni siquiera en su forma limitada a `r0`.

Gamma/cross vía AAD reverse-mode de segundo orden (Hessiano completo) queda **fuera de alcance
inicial** (§15): Burn no expone segundo orden de forma directa y forward-over-reverse es
significativamente más trabajo; Gamma se calcula vía bump-and-reval (§4) también para HullWhite.

### 5.3 Verificación obligatoria

Cada combinación (modelo, métrica, factor, método especializado) que se active para `method=Auto`
necesita, antes de mezclarla, un test diferencial contra `BumpAndReval` con tolerancia declarada
(mismo criterio que PLAN_PRODUCTS.md §13.4). Sin ese test, la combinación se sirve solo bajo
`method` explícito (nunca como default silencioso).

### 5.4 Tabla de capacidades

```cpp
struct GreekCapabilities {
    std::set<std::pair<std::string /*metric*/, RiskFactorKind>> pathwise_supported;
    std::set<std::pair<std::string /*metric*/, RiskFactorKind>> aad_supported;
    std::set<std::string /*metric*/> indicator_type_metrics;   // nunca pathwise, ver §4.5
};
```

Poblada explícitamente por fase (§11), nunca inferida por reflexión -- mismo espíritu que
`ModelCapabilities` en PLAN_PRODUCTS.md §6 ("un modelo deberá declarar capacidades, no solo
`type_name()`").

## 6. Q, P y trazabilidad

`GreekResult::measure` hereda la `ProbabilityMeasure` de la métrica base evaluada (§3.4) -- el
motor de Greeks no decide Q/P, la métrica ya lo decide (mismas reglas de PLAN_PRODUCTS.md §6):

- pedir la Greek de una métrica Q (`PV`, `PayoffPriceQ`, `PayoffExerciseQ`,
  `PayoffHitProbabilityQ`, `PayoffExposureProfileQ`) con un modelo que solo declara `PhysicalP`
  falla en el preflight de la propia métrica -- el motor de Greeks no necesita duplicar ese
  chequeo, solo propagarlo (la llamada a `metric->evaluate(...)` ya lanza);
- simétricamente para métricas P (`PayoffForecastP`, `PayoffHitProbabilityP`,
  `PayoffPnlDistributionP`) con un modelo `RiskNeutralQ`;
- una "Vega bajo P" (sensibilidad del forecast físico a la volatilidad) es una pregunta legítima y
  distinta de la Vega de precio bajo Q -- el motor las trata como dos `GreekRequest` distintos
  (`metric_name` distinto), nunca las mezcla bajo el mismo resultado.

## 7. Theta (paso del tiempo) -- no existe hoy, se introduce en Fase 5

### 7.1 Convención (ADR, fijada antes de implementar)

`Theta = Metric(valuation_time = t + dt, market SIN cambios, fixings hasta t+dt tomados del
histórico) - Metric(valuation_time = t)` -- "theta puro": todo lo demás (curva, vol, spot) se
mantiene constante, solo avanza el reloj. Esto es DISTINTO de "Carry"/"RollDown"
(PLAN_PRODUCTS.md §1.1, heredado de `PLAN_FXFORWARD.md`), que además asume una evolución esperada
del mercado (curva rodada, forward realizado) -- Carry/RollDown quedan fuera de alcance de este
documento (ya estaban fuera de alcance de PLAN_PRODUCTS.md, que los trata como "escenarios de
valoración" de un scope separado) y se pueden construir DESPUÉS como composición de un Theta puro
más un escenario de mercado, no como una Greek nueva.

### 7.2 Para `PayoffProduct`

`scenario_payoff`/`present_value` (`engine/payoff/measures.hpp`) YA reciben un parámetro
`TimePoint valuation_time = TimePoint{0.0}` -- Theta de payoff es, literalmente, la misma función
con `valuation_time` desplazado; no hace falta ninguna infraestructura nueva del lado C++, solo
que `GreekRequest`/`bump_model`-equivalente para `TimeShift` reconozca que el "parámetro" a
desplazar no es un campo de `IModel`/`MarketSnapshot` sino el propio `valuation_time` de la
llamada. Para las medidas Monte Carlo (`PayoffPriceQ`...), el bridge cxx (`ffi::price_payoff_gbm_q`
y compañía) no recibe hoy un `valuation_time` -- Fase 5 debe añadirlo como parámetro opcional
(default `0.0`, aditivo) a esas funciones.

### 7.3 Para `IrSwapProduct`/Hull-White

`PresentValueMeasure` descuenta desde `t=0` con `MarketSnapshot::discount_factor`. Theta puro
reevalúa el mismo `MarketSnapshot` (misma curva absoluta de zero rates) pero contando el tiempo
transcurrido desde `t=dt` en vez de `t=0` -- requiere que `present_value`/`compute_dv01` acepten
una fecha de valoración explícita en vez de asumir `0.0` implícito (cambio aditivo, mismo patrón
que un parámetro opcional con default hacia atrás compatible).

### 7.4 Fixings que caen entre `t` y `t+dt`

Si algún fixing/pago contractual ocurre exactamente en ese intervalo, Theta puro debe usar el
valor observado (histórico), nunca el simulado/estimado -- mismo principio de precedencia que
`FixingStore` sobre `MarketPath` en PLAN_PRODUCTS.md ADR-P0-08 ("`FixingStore` tiene prioridad
sobre `MarketPath` cuando ambas tienen valor"). Un producto que requiere un fixing futuro no
disponible en el histórico para ese `dt` rechaza el cálculo de Theta explícitamente (nunca
extrapola).

## 8. API y registro

### 8.1 `Registry<IMeasure>`: una medida genérica `"Greek"`

```cpp
class GreekMeasure : public IMeasure {
public:
    explicit GreekMeasure(const Params& params);   // parsea GreekRequest desde params, ver §8.2
    std::string type_name() const override { return "Greek"; }
    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market,
        const PricingContext& pricing, const ExecutionContext& execution
    ) const override;   // delega en engine::greeks::compute_greek(...) y aplana a MeasureResult
private:
    GreekRequest request_;
};
```

Registrada en `bootstrap.cpp` como cualquier otra: `registries.measures.register_type<GreekMeasure>("Greek")`.
Por construcción de `price()`/`price_batch`/`price_many`/`price_grid` (resuelven cualquier nombre
de `Registry<IMeasure>` sin lista cerrada, PLAN_PRODUCTS.md §12 Fase 8), queda alcanzable desde
Python/Excel/C ABI sin ningún cambio adicional en esas capas -- mismo argumento que ya se usó para
las 8 medidas de payoff.

### 8.2 Params planos: convención de prefijo para la métrica interior

`Params` es un `variant` plano sin anidamiento (§1); una `Greek` necesita configurar DOS cosas a
la vez: sus propios campos (`metric`, `risk_factor`, `order`, `method`, `bump`) y, cuando la
métrica interior los necesita, los suyos (`event` de `PayoffHitProbabilityQ`, `confidence` de
`PayoffPnlDistributionP`, `exposure_times` de `PayoffExposureProfileQ`). Se resuelve por
**convención de prefijo** en vez de ampliar `ParamValue` con un variante recursivo -- mismo
argumento que PLAN_PRODUCTS.md §7.2 ya usó para rechazar un `ParamValue` recursivo en favor de un
JSON canónico aparte; aquí no hace falta ni JSON, basta un prefijo de clave:

```cpp
registries.measures.create("Greek", Params{
    {"metric", std::string("PayoffHitProbabilityQ")},
    {"metric.event", std::string("UI")},          // -> se reenvia como Params{{"event","UI"}} a PayoffHitProbabilityQ
    {"risk_factor", std::string("model.volatility")},
    {"order", 1.0},
    {"method", std::string("auto")},
});
```

`GreekMeasure` separa las claves `metric.*` (quita el prefijo, arma un `Params` interior) del
resto (sus propios campos, sin prefijo) al construirse. Documentado explícitamente en el
doc-comment de `GreekMeasure` para que quede claro que no es un `Params` anidado real, es
azúcar de nombres sobre el mismo bag plano.

### 8.3 Función libre rica

```cpp
namespace engine::greeks {
GreekResult compute_greek(
    const Registries& registries, const GreekRequest& request, const IModel& model,
    const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
    const ExecutionContext& execution
);
}
```

Es la que de verdad decide método/bump/verificación y arma `GreekResult` completo (con
`method_used`, `bump_used`, `warnings`); `GreekMeasure::evaluate` es un envoltorio delgado que la
llama y aplana el resultado.

### 8.4 `engine_typed`/Python

`engine_typed.greeks` (Fase 9): builder tipado (`pydantic`) sobre `GreekRequest`, con constantes
para los `RiskFactor` más comunes (`greeks.delta("spot")`, `greeks.vega`, `greeks.rho`,
`greeks.dv01()`, `greeks.theta()`) que se traducen a la convención de Params de §8.2 -- mismo
patrón que `q.EuropeanCall`/`q.PayoffProduct` ya usan para el AST de payoff.

### 8.5 Barrido completo: `compute_all_greeks`/`GreeksReport`

`compute_greek`/`"Greek"` (§8.1-8.3) resuelven UNA Greek a la vez (un `RiskFactor` explícito). El
caso de uso real que motiva este documento (§9: "generar todas las Greeks de una métrica") no debe
obligar al cliente a enumerar a mano cada parámetro del modelo, cada pillar de curva y el tiempo --
eso reproduciría exactamente el problema que §0 quiere evitar (una combinación distinta por cada
factor, mantenida a mano en cada lenguaje cliente). Se añade una función de nivel superior que
**enumera automáticamente** los `RiskFactor` candidatos aplicables a un (modelo, mercado, métrica)
y calcula todos los que apliquen:

```cpp
// Reporte de TODAS las Greeks de primer orden (por defecto) aplicables a `metric_name` sobre
// `model`/`market`/`product`. "Best effort": un factor candidato que no aplique a esta metrica
// concreta se omite (con motivo) en `skipped`, NUNCA aborta el reporte completo -- pedir "todas
// las Greeks" no debe fallar solo porque una de ellas no tenga sentido aqui (p.ej.
// `credit.hazard_rate` sobre una metrica que no consume crédito: se reporta con valor `0.0` si el
// bump-and-reval genuinamente no cambia nada, no se omite artificialmente -- una derivada nula es
// una respuesta valida, distinta de "no aplica").
struct GreeksReport {
    std::vector<GreekResult> greeks;
    std::vector<std::string> skipped;   // "<risk_factor>: <motivo>", para auditar que NO se calculo
};

GreeksReport compute_all_greeks(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    bool include_curve_buckets = false,   // false: solo curve.parallel: true: +1 candidato por pillar
    bool include_second_order = false     // false: solo orden 1. true: +Gamma pura de cada factor (NO cruzadas, ver abajo)
);
```

Política de enumeración de candidatos (determinista, sin heurísticas ocultas):

1. cada clave `double` de `model.to_params()` -> `model.<clave>` (Delta/Vega/Rho/... según el
   modelo: para `Gbm` salen `spot`/`rate`/`dividend_yield`/`volatility`; para `HullWhite1F`,
   `a`/`b`/`sigma`/`r0`; para `HullWhite2F`, además `eta`/`rho`);
2. `curve.parallel` siempre; `curve.pillar:i` por cada `i` de `market.pillars()` solo si
   `include_curve_buckets`;
3. `credit.hazard_rate`/`credit.recovery_rate` siempre (ver nota de "derivada nula" arriba);
4. `time.theta` siempre, salvo que la métrica todavía no acepte `valuation_time` explícito
   (limitación transitoria hasta que la Fase 5 la generalice -- se reporta en `skipped`, no se
   omite en silencio);
5. si `include_second_order`, además la Gamma pura (`order=2`, sin `cross_factor`) de cada factor
   de (1). Las derivadas CRUZADAS (Vanna, cross-gamma) nunca se enumeran automáticamente --
   serían `n*(n-1)/2` combinaciones por defecto, la combinatoria que §14 quiere evitar; quien las
   necesite las pide una a una vía `compute_greek`/`GreekRequest::order.cross_factor`.

`GreeksReport` **no** pasa por `Registry<IMeasure>`/`MeasureResult` (esa forma es numérica -- un
eje de tiempos y dos series paralelas -- sin sitio para nombrar N factores heterogéneos por
resultado). Se expone como API de primera clase, al mismo nivel que
`ENGINE.EXPLAIN_PRODUCT`/`ENGINE.VALIDATE_PAYOFF_SPEC` (que tampoco son medidas registradas):
`Engine.all_greeks(...)` en Python, `ENGINE.ALL_GREEKS(...)` en Excel (rango dinámico, una fila por
factor) y `engine_abi_all_greeks(...)` en la C ABI (mismo patrón array+count con función `_free_*`
dedicada que ya usa `engine_abi_price`/`EnginePriceResultEntry`) -- ver §9 para el flujo completo.

## 9. Flujo end-to-end multi-cliente: autoría programática → precio → todas las Greeks

Requisito explícito de este documento, no solo una consecuencia incidental de §8: desde **cada**
cliente (Python, Excel, C ABI) tiene que poder completarse, sin escribir código nuevo por
combinación de factor de riesgo, la secuencia completa:

1. **autoría programática** del payoff con los builders del AST (`when`/`cashflow`/`maximum`/...,
   PLAN_PRODUCTS.md §7) -- nunca una plantilla nominal nueva por producto;
2. **creación** del producto (`create_product("Payoff", ...)`) y del modelo/mercado;
3. **valoración**: una o varias métricas en una sola llamada (`Engine.price(...)`, ya existente);
4. **todas las Greeks** de esas métricas, sin enumerar factores de riesgo a mano
   (`compute_all_greeks`/`GreeksReport`, §8.5).

El motivo de tratarlo como sección propia, no como un ejemplo suelto: es el criterio de aceptación
que demuestra que §§2-8 realmente resuelven el problema para el que se diseñaron (una fachada por
cliente, no una por cliente×factor) -- mismo papel que cumple el ejemplo de PLAN_PRODUCTS.md §7.3
para la autoría de payoff. Se incorpora explícitamente a la Fase 9 (§11) como criterio de
aceptación obligatorio, en las tres capas.

### 9.1 Python (`engine_typed`)

```python
import engine
from engine_typed import payoff as q

# 1. Autoria programatica (When/Cashflow/Maximum -- PLAN_PRODUCTS.md §7.3)
call = q.when(
    1.0,
    q.cashflow("USD", 1_000 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100, 0)),
)
trade = q.PayoffProduct(id="AAPL_CALL_100", contract=call)

eng = engine.Engine()
product = eng.create_product(trade.product_type, trade.to_params())
model = eng.create_model(
    "GBM", {"s0": 100.0, "r": 0.05, "q": 0.0, "sigma": 0.2, "observable": "EQ.SPOT.AAPL"}
)
market = engine.MarketSnapshot({"pillars": [1.0], "zero_rates": [0.05]})
pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 200_000, "n_steps": 1, "seed": 7})
execution = engine.ExecutionContext({"backend": "cpu", "precision": "fp64"})

# 2-3. Valoracion: una o varias metricas en una sola llamada (ya existente hoy)
result = eng.price(product, ["PayoffPriceQ"], model, market, pricing, execution)

# 4. TODAS las Greeks de esa metrica, sin enumerar spot/rate/dividend_yield/volatility a mano
report = eng.all_greeks("PayoffPriceQ", {}, model, market, pricing, execution)
for greek in report.greeks:
    print(greek.risk_factor, greek.value, greek.method_used, greek.measure)
for reason in report.skipped:
    print("omitido:", reason)
```

### 9.2 Excel

```text
=ENGINE.CREATE_PRODUCT("Payoff", spec_json)
=ENGINE.CREATE_MODEL("GBM", {"s0",100; "r",0.05; "q",0; "sigma",0.2; "observable","EQ.SPOT.AAPL"})
=ENGINE.CREATE_MARKET({"pillars",{1}; "zero_rates",{0.05}})
=ENGINE.PRICE(product, {"PayoffPriceQ"}, model, market, pricing, execution)
=ENGINE.ALL_GREEKS(product, "PayoffPriceQ", model, market, pricing, execution)   ' UDF nueva, array dinamico:
                                                                                  ' risk_factor | value | method | measure
```

`spec_json` se construye con el mismo JSON canónico `engine.payoff/v1` de PLAN_PRODUCTS.md §7.2 --
Excel no necesita un lenguaje de autoría propio, solo saber ensamblar (o pegar) ese JSON, igual que
ya hace hoy para `ENGINE.CREATE_PRODUCT("Payoff", ...)`.

### 9.3 C ABI

```c
EngineProduct* product = engine_abi_create_product("Payoff", spec_params, n_spec_params);
EngineModel* model = engine_abi_create_model("GBM", model_params, n_model_params);
EngineMarketSnapshot market = { /* pillars/zero_rates planos */ };
EnginePricingContext* pricing = /* ... */;
EngineExecutionContext* execution = /* ... */;

EnginePriceResultEntry* price_entries; size_t n_price;
engine_abi_price(product, measure_names, 1, model, &market, pricing, execution, &price_entries, &n_price);

EngineGreekResultEntry* greek_entries; size_t n_greeks;
EngineGreekSkipped* skipped; size_t n_skipped;
engine_abi_all_greeks(
    product, "PayoffPriceQ", /*metric_params=*/NULL, 0, model, &market, pricing, execution,
    /*include_curve_buckets=*/0, /*include_second_order=*/0,
    &greek_entries, &n_greeks, &skipped, &n_skipped
);
/* ... */
engine_abi_free_price_results(price_entries, n_price);
engine_abi_free_greeks_report(greek_entries, n_greeks, skipped, n_skipped);
```

`EngineGreekResultEntry` es un struct plano análogo a `EnginePriceResultEntry`
(`risk_factor`/`value`/`method_used`/`measure` como `char*` owned + `has_std_error`/`std_error` +
`has_bump`/`bump_used`), y `engine_abi_free_greeks_report` sigue el mismo patrón
alloc/free-dedicado que `engine_abi_free_price_results` (`abi.h` ya establece esa convención para
todo resultado con memoria owned por la librería).

### 9.4 Criterio de aceptación cruzado

Los tres flujos de arriba, sobre el MISMO `spec_json`/modelo/mercado, deben producir:

- el mismo precio (`PayoffPriceQ`) dentro de la tolerancia estadística de `n_paths`/`seed`
  compartidos (ya lo garantiza PLAN_PRODUCTS.md §10 Fase 10 para la parte de precio);
- el mismo conjunto de `risk_factor` en `GreeksReport.greeks` (mismos candidatos enumerados,
  ver §8.5) y el mismo valor por factor dentro de la misma tolerancia estadística;
- el mismo `skipped` (mismos factores omitidos, con el mismo motivo) en las tres capas.

Esto se verifica con UN fixture (`docs/schema/engine.payoff/examples/*.json` reutilizado de
PLAN_PRODUCTS.md, no uno nuevo) ejercitado desde C++, Python y la sonda C ABI en un único test de
integración, análogo a `test_payoff_fixtures_cross_layer.cpp`.

## 10. Integración con lo existente (aditiva, sin romper nada)

- `Dv01Measure` sigue existiendo tal cual (nombre y comportamiento) -- internamente puede
  reimplementarse como un caso particular de `GreekMeasure` (`metric=PV`,
  `risk_factor=curve.parallel` o `curve.pillar:i`), pero el nombre público `"DV01"` y su forma de
  `Params` (`bump`/`bucketed`) no cambian: es una fachada retrocompatible, igual que
  `price_measure_names()` mantiene los alias heredados `"ExpectedExposure"`/`"PFE95"`.
- `PayoffSensitivityQ` sigue existiendo tal cual; internamente se convierte en la especialización
  pathwise del motor genérico para `metric=PayoffPriceQ`, `risk_factor=model.*` de `Gbm`. Se añade
  además la cobertura que hoy falta (Gbm P, curva, Theta) a través de `"Greek"`, sin deprecar el
  nombre corto.
- `irs_hull_white_npv_delta_r0`(`_batch`) deja de ser la única forma de obtener esa derivada:
  sigue existiendo como función de bajo nivel (no se borra nada), pero `Registry<IMeasure>` gana
  la vía `"Greek"` con `metric=PV`, `risk_factor=model.r0`, `method=aad`, que además generaliza a
  `a`/`b`/`sigma` con el mismo grafo (§5.2).
- `payoff::hedge::HedgeResidualGreeks` puede, en una fase posterior no crítica, delegar en
  `engine::greeks::compute_greek` en vez de reimplementar su propio bump-and-reval -- no es
  requisito de este plan (el hedge vive en Rust puro, el motor genérico vive en C++ orquestando
  `IMeasure`); se deja como nota de consistencia futura, no como fase obligatoria.

## 11. Plan por fases

Cada fase termina con build limpio, tests unitarios y de integración; no se encadenan fases sin un
resultado verificable (mismo criterio que PLAN_PRODUCTS.md §12).

### Fase 0 — ADRs y semántica congelada

- fijar la sintaxis de `RiskFactor` (tabla de §3.1), la convención de prefijo `metric.*` (§8.2), la
  política de bump por defecto (§4.3) y la definición de Theta puro (§7.1);
- fijar `Params` -> `RiskFactor` como parseo estricto (rechaza nombres desconocidos con mensaje
  explícito, nunca `0.0` silencioso);
- fixtures manuales: Delta/Vega/Rho de una call europea (comparables a Black-Scholes cerrado, ya
  usado como oráculo en `test_gbm_measures.cpp`), DV01 paralelo de un swap, r0-delta de un HW1F.

**Aceptación**: ADRs escritos, sin decisiones semánticas implícitas en el código.

### Fase 1 — `IModel::to_params()` + motor bump-and-reval mínimo (solo `ModelParameter`, orden 1)

- añadir `to_params()` a `HullWhite1FModel`/`HullWhite2FModel`/`GbmModel`/`GbmPModel`;
- `engine::greeks::compute_greek` con `method=BumpAndReval` únicamente, `RiskFactorKind::ModelParameter`
  únicamente, `order=1` únicamente, sobre métricas SIN parámetros propios (`PV`, `PayoffPriceQ`,
  `PayoffForecastP`);
- medida `"Greek"` registrada, alcanzable vía `Engine.price(...)`;
- versión mínima de `compute_all_greeks` (§8.5): enumera solo `ModelParameter` (aún sin curva,
  crédito ni tiempo, que llegan en Fases 3-5) -- ya es suficiente para "todas las Greeks de `Gbm`"
  end-to-end en C++.

**Aceptación**: Delta/Vega/Rho de una call vía `"Greek"` coinciden (dentro de tolerancia MC) con
`PayoffSensitivityQ` existente y con Black-Scholes cerrado; `bump_used`/`method_used` correctos en
`GreekResult`; `compute_all_greeks("PayoffPriceQ", ..., Gbm)` devuelve exactamente
`{spot, rate, dividend_yield, volatility}`, ninguno en `skipped`.

### Fase 2 — convención de prefijo `metric.*` para métricas con parámetros propios

- extender `GreekMeasure`/`compute_greek` para reenviar `metric.*` a `PayoffHitProbabilityQ`(`P`),
  `PayoffExposureProfileQ`, `PayoffPnlDistributionP`.

**Aceptación**: sensibilidad de una probabilidad de hit a la volatilidad, de un VaR/ES a `mu`, de
un perfil de exposición a `sigma`, todas vía `"Greek"` sin código nuevo por combinación.

### Fase 3 — `CurveParallel`/`CurvePillar` genéricos

- `bump_market` para curva paralela y por pillar, aplicable a CUALQUIER producto que descuenta con
  `MarketSnapshot` (IRS y `PayoffProduct` vía `market_snapshot_bridge`), quitando la limitación
  actual "`Dv01Measure` bucketed no soportado para `PayoffProduct`".

**Aceptación**: DV01 bucketed de un `PayoffProduct` coincide (suma de buckets) con el DV01
paralelo existente; `Dv01Measure` reimplementado sobre `compute_greek` sin cambiar su
comportamiento público (test de no-regresión byte a byte de resultados).

### Fase 4 — `CreditParameter` (CVA/exposición)

- bump de `hazard_rate`/`recovery_rate` de `MarketSnapshot`, aplicado a `UnilateralCVA`/
  `ExposureProfile`.

**Aceptación**: sensibilidad de CVA a hazard rate tiene el signo esperado (sube el hazard rate,
sube CVA) en un fixture manual.

### Fase 5 — `TimeShift` (Theta)

- `valuation_time` opcional en el bridge cxx de payoff Monte Carlo (aditivo, default `0.0`);
- `valuation_time` opcional en `present_value`/`compute_dv01` para IRS/Hull-White;
- convención de precedencia de fixings históricos (§7.4).

**Aceptación**: Theta de una call europea sin dividendos es negativo (pierde valor temporal, caso
estándar); Theta de un swap a la par sigue siendo (aproximadamente) cero un día después si la
curva no cambia.

### Fase 6 — segundo orden y cruzadas (Gamma, Vanna, Volga, cross-gamma)

- estencil de 3 puntos (reutiliza `base`/`up`/`down` ya computados para orden 1) para Gamma puro;
- estencil de 4 puntos para derivadas cruzadas (`up_up`, `up_down`, `down_up`, `down_down`);
- cache por hash de `(metric_name, metric_params, model.to_params() bumpeado)` para no repetir una
  evaluación ya hecha cuando dos `GreekRequest` comparten un mismo punto bumpeado (mismo principio
  de deduplicación por fingerprint que `price_batch_generic`, PLAN_PRODUCTS.md §12 Fase 11).

**Aceptación**: Gamma de una call europea coincide con la derivada segunda cerrada de
Black-Scholes dentro de tolerancia; Vanna tiene el signo/orden de magnitud esperado en un fixture
de referencia.

### Fase 7 — rutas especializadas verificadas (Pathwise extendido + AAD reverse-mode)

- generalizar `payoff::sensitivity` de `GbmGreek` (4 variantes cerradas) a `RiskFactor` unificado,
  sin cambiar el mecanismo `Dual` interno; extender a `GbmP`;
- `irs_hull_white_npv_all_greeks` (reverse-mode, un solo `backward()` para `a`/`b`/`sigma`/`r0`,
  equivalente en HullWhite2F con `eta`/`rho`), cableado como `method=aad` de `"Greek"`;
- tabla de capacidades (§5.4) poblada explícitamente; tests diferenciales pathwise/AAD vs
  bump-and-reval para cada entrada de la tabla, con tolerancia declarada.

**Aceptación**: `method=auto` elige pathwise/AAD cuando existe y coincide con `BumpAndReval` dentro
de tolerancia; pedir `method=pathwise` sobre `PayoffHitProbabilityQ` falla explícito (§4.5), nunca
aproxima en silencio.

### Fase 8 — lote (`price_batch`/`price_many`/`price_grid`)

- Greeks en lote reutilizando el agrupamiento por fingerprint ya existente (mismas rutas
  simuladas para varias Greeks del mismo trade cuando comparten modelo/mercado base).

**Aceptación**: `price_many` mezcla `"PV"`, `"DV01"` y `"Greek"` (varios factores) sobre el mismo
trade sin recalcular la simulación base más de una vez cuando es compartible.

### Fase 9 — bindings, `compute_all_greeks` y flujo end-to-end multi-cliente

- `engine_typed.greeks` (Python tipado), validación/explain, Excel, C ABI, ejemplos y cookbook de
  errores (factor desconocido, método no soportado, combinación Q/P inválida);
- `compute_all_greeks`/`GreeksReport` (§8.5) cableado en las tres capas:
  `Engine.all_greeks(...)` (Python), `ENGINE.ALL_GREEKS(...)` (Excel, rango dinámico),
  `engine_abi_all_greeks`/`engine_abi_free_greeks_report` (C ABI, mismo patrón alloc/free que
  `engine_abi_price`/`engine_abi_free_price_results`);
- **requisito explícito de esta fase** (§9): el flujo completo "autoría programática del payoff
  (`when`/`cashflow`/...) → `create_product` → `price(...)` → `all_greeks(...)`" debe completarse
  sin código nuevo por factor de riesgo en LAS TRES capas -- no solo en C++ como prueba de
  concepto. Los ejemplos de §9.1-9.3 pasan a ser ejemplos reales del repo (`examples/`,
  notebooks, sonda C ABI), no solo pseudocódigo del plan.

**Aceptación**: los mismos `GreekRequest` producen el mismo resultado (dentro de tolerancia
estadística) en C++, Python, Excel y C ABI; el criterio de aceptación cruzado de §9.4 (mismo
`spec_json`, mismo conjunto de `risk_factor` y `skipped`, mismo valor por factor en las tres capas)
pasa como test de integración único, análogo a `test_payoff_fixtures_cross_layer.cpp`.

## 12. Estrategia de pruebas

### 12.1 Financieras (oráculo independiente, mismo criterio que PLAN_PRODUCTS.md §13.3)

- Delta/Gamma/Vega/Rho/Theta de una call/put europea contra las fórmulas cerradas de
  Black-Scholes (ya usadas como oráculo en `test_gbm_measures.cpp`/`test_gbm_sensitivity_and_hedge.cpp`);
- identidad put-call parity aplicada a Greeks (Delta_call - Delta_put == `e^{-qT}`, etc.);
- DV01 de un swap a la par frente a bump-and-reval manual ya existente (`test_registry.cpp`);
- relación Theta-Gamma-Vega de Black-Scholes (identidad de la EDP) como test de consistencia
  cruzada entre tres Greeks calculadas de forma independiente.

### 12.2 Diferenciales

- `BumpAndReval` vs `Pathwise` vs `AadReverse` para cada entrada de la tabla de capacidades (§5.4);
- CPU vs GPU cuando el backend de payoff lo soporte (reutiliza la infraestructura de
  PLAN_PRODUCTS.md §12 Fase 11, no la reimplementa).

### 12.3 Robustez

- convergencia del bump: reducir `h` a la mitad, comprobar que el resultado no cambia más allá de
  lo esperado por el ruido Monte Carlo (detecta un bump demasiado grande) y que no explota por
  cancelación catastrófica (detecta uno demasiado pequeño);
- determinismo con seed (números aleatorios comunes, §4.4);
- rechazo explícito de: `risk_factor` desconocido para el modelo, `method` no soportado para la
  combinación, orden `>2`, combinación Q/P inválida heredada de la métrica base, Theta que cruza
  un fixing no disponible en el histórico (§7.4).

## 13. Observabilidad

Cada `GreekResult` (§3.4) es auto-explicativo: factor de riesgo, orden, método REALMENTE
ejecutado, medida Q/P/Deterministic heredada, bump usado (si aplica), error estándar (si la
métrica base es Monte Carlo) y warnings explícitos (§4.5). Ningún resultado de Greek se sirve sin
saber de dónde salió -- mismo principio que PLAN_PRODUCTS.md §14.

## 14. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| explosión de combinaciones (métrica × factor × orden × método) | dispatch genérico sobre `IMeasure`/`Params`, no una clase por combinación (§8.1-8.2) |
| ruido Monte Carlo domina un bump pequeño | números aleatorios comunes obligatorios (§4.4) + política de bump documentada (§4.3) |
| pathwise aplicado a una métrica indicador da una "derivada" sin sentido | tabla de capacidades explícita, nunca inferida (§4.5, §5.4) |
| convención de Theta ambigua (¿roll de curva o no?) | ADR fija "theta puro" antes de implementar (§7.1); Carry/RollDown quedan fuera, se construyen después por composición |
| `Params` plano no admite parámetros anidados de la métrica interior | convención de prefijo `metric.*`, no un `ParamValue` recursivo nuevo (§8.2) |
| romper `Dv01Measure`/`PayoffSensitivityQ` existentes al generalizar | fachadas retrocompatibles, mismo nombre/forma pública, reimplementadas por dentro sobre el motor genérico (§10) |
| `GreeksReport` (barrido automático, §8.5) dispara muchas evaluaciones sin que el usuario lo pida explícitamente | `include_curve_buckets`/`include_second_order` en `false` por defecto; factores candidatos limitados a los parámetros reales del modelo (`to_params()`), nunca una combinatoria abierta |
| AAD reverse-mode de segundo orden no disponible en Burn | Gamma/cruzadas vía bump-and-reval también para modelos con AAD de primer orden (§5.2) |
| riesgo de correlación multi-activo pedido pero no soportable | rechazo explícito: no existe `RiskFactor` de correlación en el catálogo (§3.1), documentado como arquitectónicamente fuera de alcance (§15), igual que en `payoff::hedge` |

## 15. Fuera de alcance inicial

- Greeks respecto de parámetros de CONTRATO (strike, nivel de barrera, fecha de ejercicio) como si
  fueran factores de mercado -- son una herramienta de "qué pasa si" (re-cotizar el mismo contrato
  con otro strike), no un riesgo de mercado; si se necesitan, se modelan como una funcionalidad
  aparte ("scenario repricing"), no como `RiskFactor`;
- riesgo de correlación/base multi-activo (exige un modelo multi-activo que no existe, mismo límite
  ya documentado en `payoff::hedge`, PLAN_PRODUCTS.md §12 Fase 11);
- Carry/RollDown (evolución esperada de mercado) -- distinto de Theta puro (§7.1), se pospone;
- AAD de segundo orden (Hessiano completo vía reverse-over-reverse o forward-over-reverse) --
  Gamma/cruzadas se sirven por bump-and-reval (§5.2, §6);
- método de razón de verosimilitud (likelihood ratio)/Malliavin para Greeks de payoffs
  discontinuos -- bump-and-reval ya cubre ese caso con un método más simple, aunque más costoso en
  paths (§4.5);
- vectorización GPU específica del motor de Greeks -- reutiliza (no reimplementa) la
  infraestructura CPU/GPU ya construida en PLAN_PRODUCTS.md §12 Fase 11 cuando exista una ruta
  Monte Carlo vectorizada que bumpear.

## 16. Definition of Done

El motor de Greeks se considera implantado, no solo prototipado, cuando:

- cualquier `IMeasure` registrada puede pedirse derivada de cualquier `RiskFactor` que su modelo
  declare (vía `to_params()`) o que su mercado exponga (curva, crédito), sin código nuevo por
  combinación;
- Theta y Gamma/cruzadas están disponibles con la misma interfaz (`"Greek"`) que Delta/Vega/Rho;
- Q/P quedan trazados en cada `GreekResult`, heredados de la métrica base, nunca decididos por el
  motor de Greeks;
- toda especialización (pathwise/AAD) activa en `method=Auto` tiene un test diferencial contra
  bump-and-reval en verde;
- `Dv01Measure`/`PayoffSensitivityQ` siguen funcionando sin cambios de comportamiento observable;
- desde Python, Excel y la C ABI puede completarse, sin código nuevo por factor de riesgo, el
  flujo "autoría programática del payoff → `create_product` → `price(...)` → `all_greeks(...)`"
  (§9), con el mismo `spec_json` produciendo el mismo conjunto de Greeks en las tres capas (§9.4);
- suites C++, Rust, Python, C ABI y Excel están en verde;
- existe cookbook de errores (factor desconocido, método no soportado, combinación Q/P inválida,
  Theta sin fixing disponible) con mensajes que nombran la causa exacta.

---

*Decisión central: la Greek es siempre "derivada de una métrica ya registrada respecto de un
factor de riesgo tipado"; el método de cálculo (bump-and-reval universal, pathwise o AAD
especializados y verificados) es un detalle de rendimiento, nunca una fuente de resultados
distintos sin que quede trazado en `GreekResult`.*
