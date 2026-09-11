# PLAN_PRODUCTS.md — motor universal de productos mediante PayOff componibles

> Documento de arquitectura y plan de implementación. Su objetivo es sustituir el crecimiento
> de una clase C++/Rust y varias medidas por cada producto por un lenguaje declarativo de
> contratos, compilable y evaluable bajo escenarios deterministas, medida neutral al riesgo
> **Q** y, cuando se pida explícitamente, escenarios físicos **P**. Este documento no declara
> implementadas las fases: cada una se migrará a `PLAN.md` únicamente después de quedar
> construida y verificada de extremo a extremo.

## 0. Decisión ejecutiva

El motor no debe intentar enumerar todos los productos financieros. Debe representar el
**payoff y el ciclo de vida** de un producto como un programa inmutable compuesto por unas pocas
primitivas, y separar ese programa de:

1. los datos de mercado que consume;
2. el modelo que genera escenarios;
3. la medida probabilística (**Q** para precio sin arbitraje, **P** para forecast/P&L);
4. el algoritmo que lo evalúa;
5. la forma en que se presenta en C++, Python, Excel o C ABI.

La unidad universal será `PayoffProduct`, que contiene un `Contract` (AST). `IRSwap`,
`FXForward`, `EuropeanCall`, `BarrierOption`, `TakeProfit` o `StopLoss` pasan a ser
**constructores/plantillas** que producen ese AST, no jerarquías rígidas que obligan a añadir
ramas de `dynamic_cast` por toda la aplicación.

Se preservan las rutas especializadas actuales de IRS/Hull-White como optimizaciones. La
universalidad se introduce primero como una ruta correcta y auditable; un visitor de
reconocimiento podrá compilar determinados árboles a los kernels existentes sin cambiar su
semántica.

### 0.1 Qué significa y qué no significa «cualquier producto»

El objetivo realista es poder expresar cualquier producto cuyo contrato pueda describirse con:

- observables disponibles en el `MarketSnapshot` o generables por un `IModel`;
- fechas, calendarios, fijaciones y pagos definidos;
- expresiones escalares y booleanas;
- estado finito persistente y eventos de primera ocurrencia;
- derechos de ejercicio con una política de decisión soportada;
- cashflows en una o varias monedas.

No significa que todo producto sea valorable con cualquier modelo ni que toda cobertura sea
perfecta. El compilador deberá rechazar, antes de simular, un payoff que requiera observables,
correlaciones, granularidad de barrera o derechos de ejercicio que el modelo/algoritmo elegido
no pueda suministrar.

## 1. Estado actual y puntos de integración

El estado actual del repositorio condiciona el diseño:

- `cpp/engine/include/engine/product.hpp` contiene `IProduct` y el producto nominal
  `IrSwapProduct`.
- `cpp/engine/include/engine/params.hpp` define un `Params` plano (`double`, `vector<double>`,
  `bool`, `string`); no puede transportar de forma nativa un árbol anidado.
- `cpp/engine/include/engine/measure.hpp` y `cpp/engine/src/measure.cpp` calculan medidas mediante
  ramas específicas `dynamic_cast` a producto/modelo.
- `cpp/engine/src/price.cpp` ya resuelve medidas por registry, comparte cálculos repetidos y
  tiene rutas de lote especializadas para IRS.
- `MarketSnapshot` compone hoy una curva de descuento y datos de crédito. Es adecuado como
  fachada retrocompatible, pero no como catálogo cerrado de todos los observables futuros.
- el core Rust ya posee simulación Hull-White, rejillas y kernels CPU/GPU; la definición
  universal deberá compilarse a una representación plana que Rust pueda ejecutar por lotes.
- `engine_typed` es la fachada apropiada para builders Python seguros. El dict crudo debe
  seguir existiendo como transporte de bajo nivel.

### 1.1 Relación con `PLAN_FXFORWARD.md`

`PLAN_FXFORWARD.md` conserva sus fórmulas financieras, requisitos de mercado y casos de prueba,
pero queda **reemplazado por este plan en la forma de modelar productos**:

- no se recomienda crear `FxForwardProduct` más siete clases rígidas de medida;
- `FXForward` será una plantilla de `PayoffProduct`;
- PV y sensibilidades genéricas operarán sobre dependencias del árbol;
- `Carry`, `RollDown` y `PnL` serán escenarios/transformaciones de valoración, no propiedades
  de una clase nominal FX;
- la infraestructura de doble curva, spot y basis descrita allí sigue siendo necesaria como
  catálogo de observables.

## 2. Arquitectura objetivo

```text
Plantilla (IRSwap / FXForward / Barrier / estrategia)
                         |
                         v
                 Contract AST inmutable
                  /       |        \
       expresiones    eventos     cashflows
                         |
                         v
       ValidateVisitor + DependencyVisitor + Canonicalizer
                         |
                         v
             CompiledPayoff IR versionado
             /            |             \
   Scenario evaluator   Q pricer      P simulator
     (una ruta)        (precio/risk)  (forecast/P&L)
             \            |             /
                         v
                CashflowLedger / Result
```

Se distinguen tres representaciones:

1. **AST de autoría**: expresivo, tipado e inmutable; cómodo para builders y serialización.
2. **IR compilado**: nodos planos por índice, dependencias deduplicadas, schedules normalizados,
   preparado para transferencia C++↔Rust y ejecución vectorizada.
3. **estado de ruta**: mutable y efímero; contiene eventos disparados, fecha de primer hit,
   fijaciones y estado de ejercicio. Nunca se modifica el AST para representar la evolución.

La separación AST/estado evita que valorar el mismo trade en dos rutas o threads contamine una
ejecución con otra.

## 3. Modelo de dominio

### 3.1 Identificadores y tipos básicos

No se usarán strings ambiguos dentro del evaluador. En la API pública pueden entrar strings,
pero el validador los convierte a tipos:

```cpp
struct ObservableId { std::string value; };
struct EventId      { std::string value; };
struct Currency     { std::string code; };
struct TimePoint    { double year_fraction; };
struct NodeId       { std::uint32_t value; };
```

Reglas:

- los `ObservableId` son nombres canónicos y namespaced, por ejemplo `EQ.SPOT.AAPL`,
  `FX.SPOT.EURUSD`, `IR.DF.EUR.OIS`, `IR.FWD.EUR.EURIBOR3M`;
- una ausencia es error explícito, nunca `0.0` silencioso;
- todas las comparaciones de tiempo usan una tolerancia centralizada;
- moneda vacía, fechas no finitas, schedules desordenados o ids duplicados se rechazan al
  construir/validar, no durante el pricing.

### 3.2 Expresiones escalares (`ScalarExpr`)

Los observables forman su propio Composite. Conjunto mínimo v1:

| Nodo | Semántica |
|---|---|
| `Constant(x)` | literal finito |
| `Parameter(name)` | parámetro contractual inmutable (strike, nocional, barrera) |
| `Fixing(observable, time)` | valor del observable en una fecha concreta |
| `Current(observable)` | valor en el instante de evaluación del nodo |
| `Add/Sub/Mul/Div` | aritmética binaria; división por cero es error de ruta |
| `Neg/Abs/Exp/Log/Pow` | operaciones escalares básicas |
| `Min/Max/Clamp` | no linealidades de payoff |
| `Average(observable, schedule, weights)` | media asiática ponderada |
| `RunningMin/RunningMax` | extremos observados hasta el instante actual |
| `EventTime(event_id)` | fecha del primer disparo de un evento |
| `EventValue(event_id, observable)` | fixing capturado al dispararse |
| `DiscountFactor(curve_id, from, to)` | observable explícito de descuento |
| `FxConversion(from_ccy, to_ccy, time)` | conversión explícita, nunca implícita |

Los nodos deben exponer `accept(ScalarVisitor&)`; no contienen lógica de modelo ni consultan
singletons globales.

### 3.3 Predicados (`Predicate`)

| Nodo | Semántica |
|---|---|
| `Greater/Less/GreaterEqual/LessEqual/Eq` | comparación de expresiones escalares |
| `All/Any/Not` | composición booleana con cortocircuito definido |
| `Between` | rango con extremos inclusivos configurables |
| `EventOccurred(event_id)` | consulta el estado persistente de una ruta |
| `Before/After(time)` | condición temporal explícita |

La igualdad numérica exige tolerancia declarada; no se utilizará `double == double` como
semántica contractual por defecto.

### 3.4 Contratos (`Contract`)

| Nodo | Semántica |
|---|---|
| `Zero` | no genera cashflows |
| `Cashflow(currency, amount)` | genera un pago en el instante activo |
| `Give(child)` | cambia el signo de todos los cashflows del hijo |
| `Both(children...)` | posee todos los hijos; generaliza `And` binario |
| `Scale(factor, child)` | multiplica los cashflows del hijo |
| `If(predicate, if_true, if_false)` | elige una rama en el instante de evaluación |
| `When(time, child)` | evalúa el hijo en una fecha contractual |
| `Trigger(spec, on_hit, on_miss)` | evento path-dependent persistente |
| `Exercise(spec, exercise_value, continuation)` | derecho de decisión americano/bermuda |

`Or(c1, c2)` no debe confundirse con un booleano: representa un **derecho de elección** y por
tanto se modela como `Exercise`, con titular, fechas y política de decisión explícitos. Un
`max(value(c1), value(c2))` solo es equivalente bajo supuestos de valoración que el AST no debe
ocultar.

### 3.5 Cashflows, no solo un `double`

La salida pathwise es un ledger:

```cpp
struct Cashflow {
    TimePoint payment_time;
    Currency currency;
    double amount;
    std::optional<EventId> source_event;
    NodeId source_node;
};
using CashflowLedger = std::vector<Cashflow>;
```

No se agregan monedas automáticamente. El visitor de valoración decide cómo descontar y
convertir el ledger a la moneda de reporting. El visitor de auditoría puede devolverlo intacto.

## 4. Semántica de eventos y estado

### 4.1 `TriggerSpec`

```cpp
enum class Monitoring { Discrete, ContinuousApproximation };
enum class Settlement { AtHit, AtScheduledPayment };

struct TriggerSpec {
    EventId id;
    std::vector<TimePoint> monitoring_times;
    PredicatePtr condition;
    Monitoring monitoring;
    Settlement settlement;
    int priority;
    bool latch;              // true: una vez disparado permanece disparado
};
```

Cada ruta mantiene:

```cpp
struct EventState {
    bool occurred = false;
    std::optional<TimePoint> first_hit_time;
    std::unordered_map<ObservableId, double> captured_values;
};
```

El runtime recorre la unión ordenada de fechas requerida por el contrato. En cada fecha:

1. carga/genera los observables;
2. actualiza acumuladores (`Average`, `RunningMin`, `RunningMax`);
3. evalúa triggers activos por prioridad estable;
4. persiste el primer hit y sus valores capturados;
5. evalúa decisiones de ejercicio;
6. emite cashflows cuyo pago corresponde a esa fecha.

Dos triggers en el mismo instante se resuelven primero por `priority` y luego por orden
canónico de `EventId`. Esa regla forma parte del contrato serializado para evitar resultados
dependientes del orden de un `unordered_map`.

### 4.2 Barreras

Una barrera es una plantilla sobre `Trigger`, no un nodo especial obligatorio:

- up-and-in: `Trigger(S >= B, on_hit=underlying_contract, on_miss=Zero)`;
- down-and-out: `Trigger(S <= B, on_hit=rebate, on_miss=underlying_contract)`;
- double knock-out: predicado `Any(S <= B_low, S >= B_high)`;
- window barrier: schedule limitado a la ventana;
- Parisian: extensión posterior con acumulador de duración, no aproximación silenciosa.

La comparación (`>` frente a `>=`) y el schedule son datos contractuales. «Continua» no puede
significar mirar solo los puntos de una malla. `ContinuousApproximation` requiere que el modelo
declare soporte de Brownian bridge o una corrección equivalente. Si no lo soporta, el
preflight falla; el usuario puede elegir explícitamente `Discrete` con una malla más fina.

### 4.3 Take profit y stop loss

TP/SL son reglas de cierre de posición, no meros `max/min` terminales. Se modelan como un grupo
`FirstOf` compilado a triggers con estado compartido:

```text
event group POSITION_EXIT (latch=true)
  priority 10: TAKE_PROFIT si metric >= take_profit_level
  priority 20: STOP_LOSS   si metric <= stop_loss_level
  si ninguno ocurre: continuar hasta maturity
```

`metric` debe ser explícita:

- precio del subyacente;
- retorno desde `entry_price`;
- P&L monetario de una posición;
- mark-to-market de un contrato hijo bajo una política declarada.

La v1 soportará las tres primeras. Usar el mark-to-market de un contrato hijo introduce una
valoración anidada/recursiva y se pospone hasta disponer de caché y reglas claras de medida.

Ejemplo conceptual de posición larga con entrada `S0`, TP `+20%` y SL `-10%`:

```text
metric = Current(EQ.SPOT.AAPL) / Parameter(entry_price) - 1
TP = metric >= 0.20
SL = metric <= -0.10
settlement = Cashflow(USD, quantity * (EventValue(EXIT, spot) - entry_price))
```

El cierre usa el **primer** hit y liquida `AtHit` o en una fecha programada según el contrato.
Si hay gap, paga con el fixing observado, no mágicamente al nivel exacto del stop. Los costes de
transacción/slippage serán expresiones opcionales separadas.

## 5. Evaluadores y Visitors

### 5.1 Visitors obligatorios

| Visitor | Responsabilidad |
|---|---|
| `ValidationVisitor` | tipos, schedules, ids, monedas, referencias y ciclos |
| `DependencyVisitor` | observables, curvas, fixing dates, monedas y capacidades requeridas |
| `CanonicalVisitor` | orden estable, hash/fingerprint y serialización canónica |
| `ScenarioEvaluator` | una ruta conocida → `CashflowLedger` |
| `CompilerVisitor` | AST → `CompiledPayoff` plano para Rust/CPU/GPU |
| `ExplainVisitor` | árbol y cashflows legibles con procedencia por nodo/evento |
| `SpecializationVisitor` | reconoce patrones acelerables (IRS, vanilla, forward) |

Sensibilidades no se implementan como nodos del contrato. Son visitors/medidas que reprician el
mismo programa con AAD o bump-and-reval sobre dependencias extraídas.

### 5.2 Contextos separados

```cpp
struct EvaluationContext {
    const MarketPath& path;
    const FixingStore& historical_fixings;
    RuntimeState& state;
};

struct ValuationContext {
    ProbabilityMeasure measure;   // RiskNeutralQ o PhysicalP
    Currency reporting_currency;
    const DiscountingPolicy& discounting;
    const ModelCapabilities& capabilities;
};
```

`ScenarioEvaluator` no descuenta y no decide Q/P: solo ejecuta el contrato en una ruta. El
pricer genera rutas bajo la medida seleccionada, llama al evaluador y descuenta el ledger.

## 6. Q, P y contrato de cada medida

Black-Scholes y cualquier precio por no arbitraje usan **Q**. P se reserva para forecast,
probabilidades reales, stress y P&L. El motor debe impedir que la diferencia quede escondida en
un nombre ambiguo de modelo.

```cpp
enum class ProbabilityMeasure { RiskNeutralQ, PhysicalP, DeterministicScenario };
enum class MeasurePurpose { FairValue, Risk, Exposure, Forecast, PnL, Audit };

struct MeasureMetadata {
    MeasurePurpose purpose;
    std::vector<ProbabilityMeasure> allowed_measures;
    bool requires_discounting;
};
```

Reglas v1:

- `PV`, precio Black-Scholes, Greeks de pricing y exposición/CVA se ejecutan bajo Q;
- `Forecast`, probabilidad de hit, distribución de retornos y P&L esperado pueden ejecutarse
  bajo P, con drift/calibración física explícitos;
- `ScenarioPayoff` usa una ruta determinista y no finge ser ni Q ni P;
- `Carry`/`RollDown` deterministas declaran `DeterministicScenario`;
- si se pide `PV` con un modelo configurado solo para P, se lanza un error de compatibilidad;
- el resultado incluye medida, modelo, fecha, moneda y hash del payoff para trazabilidad.

Un modelo deberá declarar capacidades, no solo `type_name()`:

```cpp
struct ModelCapabilities {
    std::set<ObservableId> generated_observables;
    std::set<ProbabilityMeasure> supported_measures;
    bool supports_joint_paths;
    bool supports_continuous_barrier_bridge;
    bool supports_early_exercise_regression;
};
```

El `DependencyVisitor` y las capacidades se comparan antes de reservar paths o lanzar un kernel.

## 7. API de autoría y serialización

### 7.1 C++

La API idiomática usa builders y punteros inmutables compartidos:

```cpp
auto intrinsic = max(spot("EQ.SPOT.AAPL", T) - constant(100.0), constant(0.0));
auto call = when(T, cashflow("USD", 1'000.0 * intrinsic));
auto product = PayoffProduct("AAPL_CALL_100", call);
```

Los operadores sobrecargados serán azúcar sintáctico; los nodos reales siguen siendo tipos
explícitos visitables. Tras construir, `validate()` devuelve todos los errores con rutas de nodo
(`root.children[1].condition.left`), no solo el primero.

### 7.2 Transporte canónico

No se ampliará `ParamValue` con decenas de variantes recursivas. El registry existente podrá
crear el producto universal así:

```cpp
registries.products.create("Payoff", {{"spec", canonical_json}});
```

`spec` será JSON canónico versionado, validado contra un schema mantenido en el repositorio.
Ejemplo abreviado:

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
        "type": "max",
        "left": {
          "type": "sub",
          "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
          "right": {"type": "constant", "value": 100.0}
        },
        "right": {"type": "constant", "value": 0.0}
      }
    }
  }
}
```

Requisitos de serialización:

- `schema` obligatorio y migradores explícitos `v1 -> v2`;
- rechazo de campos desconocidos por defecto;
- límites de profundidad, nodos y tamaño antes de parsear/evaluar;
- números finitos; no se aceptan `NaN`/`Infinity` en JSON;
- claves ordenadas y floats normalizados para obtener un hash reproducible;
- el hash canónico forma parte de cachés y resultados.

Se evaluará una dependencia JSON pequeña y pinneada frente a un parser propio. Se recomienda no
escribir un parser JSON casero: aumenta superficie de seguridad sin aportar valor cuantitativo.

### 7.3 Python tipado

`engine_typed.payoff` contendrá modelos Pydantic discriminados (`type`), operadores/builders y
`PayoffProduct.to_params()`:

```python
call = q.when(
    1.0,
    q.cashflow("USD", 1_000 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100, 0)),
)
trade = q.PayoffProduct(id="AAPL_CALL_100", contract=call)
native = eng.create_product(trade.product_type, trade.to_params())
```

Las plantillas `q.EuropeanCall`, `q.IRSwap` y `q.FXForward` devolverán o contendrán el mismo
`PayoffProduct`; no necesitarán bindings C++ nominales nuevos.

### 7.4 Excel y C ABI

- Excel v1 recibe el JSON en `ENGINE.CREATE_PRODUCT("Payoff", {"spec", ...})`; una UDF helper
  podrá construir plantillas frecuentes, pero la semántica vive en el AST.
- C ABI recibe `const char* spec_json` mediante el `EngineParam` string ya existente; no cambia
  layout por introducir el producto.
- se añadirán funciones de validación/explicación que no crean handles, para dar errores útiles
  antes de valorar.

## 8. Compilación y ejecución en Rust

El AST de autoría vive inicialmente en C++/Python, pero el hot path se ejecutará en Rust. El IR
no transporta objetos polimórficos:

```rust
pub struct CompiledPayoff {
    pub version: u32,
    pub scalar_ops: Vec<ScalarOp>,
    pub predicate_ops: Vec<PredicateOp>,
    pub contract_ops: Vec<ContractOp>,
    pub schedules: Vec<Vec<f64>>,
    pub observable_slots: Vec<ObservableKey>,
    pub event_slots: Vec<EventSpec>,
    pub root: u32,
}
```

El compilador realiza:

1. validación y tipado;
2. unión/deduplicación de rejillas;
3. resolución de nombres a slots enteros;
4. common-subexpression elimination para fijaciones/expresiones repetidas;
5. orden topológico;
6. cálculo de buffers de estado necesarios;
7. fingerprint y detección de patrón especializado.

La primera implementación Rust será escalar `f64` y de referencia. Después se vectoriza por
paths/trades usando los backends existentes. CPU y GPU deben ejecutar el mismo IR y superar
tests diferenciales dentro de tolerancia.

## 9. Integración y migración de lo existente

### 9.1 `IProduct`

Evolución aditiva propuesta:

```cpp
class IProduct {
public:
    virtual ~IProduct() = default;
    virtual std::string type_name() const = 0;
    virtual const PayoffProgram* payoff_program() const { return nullptr; }
};
```

`PayoffProduct` devuelve su programa. Los productos legacy pueden devolver `nullptr` hasta ser
migrados. No se rompe `FakeProduct` ni consumidores existentes.

### 9.2 `IMeasure` y `price()`

Se elimina gradualmente el doble despacho manual producto×medida:

- medidas genéricas (`PV`, `ScenarioPayoff`, `Delta`, `Vega`, `HitProbability`, `Cashflows`)
  consumen `payoff_program()` y dependencias;
- adaptadores legacy mantienen IRS operativo durante la transición;
- `price()` sigue resolviendo por `Registry<IMeasure>` y conserva su caché por medida+params;
- `price_batch` deja de asumir IRS al existir el segundo producto universal: agrupa por
  fingerprint de IR, modelo, rejilla y configuración de medida;
- `price_many`/`price_grid` conservan orden e índices públicos.

### 9.3 Migración de `IRSwap`

El IRS se expresa como cashflows fijos y flotantes:

- pata fija: `Both(When(T_i, Cashflow(ccy, -N*K*accrual_i)))`;
- pata flotante: fijaciones de índice/proyección más sus pagos o, para el caso single-curve
  actual, réplica equivalente documentada;
- el sentinel PAR se resuelve al construir el trade usando la curva de mercado, o se representa
  como parámetro derivado explícito antes de congelar el contrato;
- `SpecializationVisitor` reconoce el patrón compatible y llama a los kernels actuales de
  Hull-White/exposición;
- tests comparan AST genérico, kernel especializado y resultados legacy.

La migración no debe degradar PV/DV01 deterministas hoy basados en curva observada.

### 9.4 Migración de `FXForward`

Será la primera prueba de que no hace falta una clase core nueva:

```text
When(T,
  Both(
    Cashflow(FOR,  sign * N_for),
    Cashflow(DOM, -sign * N_for * K)
  )
)
```

El visitor de reporting convierte la pata extranjera con FX y descuenta cada moneda con su
curva/basis correspondiente. Las sensibilidades salen de las dependencias de spot y curvas.
Las fórmulas y tests de `PLAN_FXFORWARD.md` siguen siendo valores de referencia.

### 9.5 Mercado universal sin convertir `MarketSnapshot` en un mega-struct

Se añade una interfaz interna:

```cpp
class MarketDataView {
public:
    virtual double fixing(ObservableId, TimePoint) const = 0;
    virtual double discount_factor(CurveId, TimePoint, TimePoint) const = 0;
    virtual bool contains(ObservableId) const = 0;
};
```

`MarketSnapshot` implementa/adapta esta vista y mantiene sus accessors actuales. Los nuevos
observables se almacenan en colecciones tipadas por id, no como un campo opcional nuevo por
cada clase de activo. Esto corrige a tiempo la dirección de «mega-snapshot» que resultaría de
añadir spot/curva/basis de forma singular por producto.

## 10. Ejercicio americano y bermuda

`Exercise` necesita un motor de decisión, no solo ejecutar una condición conocida:

- europea: una fecha; payoff intrínseco directo;
- bermuda: conjunto discreto de fechas;
- americana: rejilla/algoritmo declarado;
- holder exercise: maximiza valor para el comprador;
- issuer call: minimiza valor sujeto al contrato.

La primera política será Longstaff–Schwartz Monte Carlo para bermudas, con basis functions y
semilla en `PricingContext`. El AST solo declara el derecho; la política pertenece al pricer.
No se permitirá evaluar `Exercise` con `ScenarioEvaluator` sin una decisión suministrada, porque
escoger automáticamente por payoff intrínseco ignoraría el valor de continuación.

## 11. Síntesis y cobertura de portfolios

El mismo AST permite agregar un portfolio mediante `Both` y cambiar signo con `Give`. Para
buscar una cobertura con instrumentos disponibles:

1. construir `portfolio_payoff = Both(trades...)`;
2. definir el universo de hedges realmente negociables;
3. evaluar payoff/Greeks sobre una rejilla común de escenarios;
4. resolver pesos que minimicen error ponderado más costes/restricciones;
5. devolver hedge y riesgo residual; nunca prometer neutralización exacta sin comprobar rango.

La optimización puede ser least-squares/LP/QP según la norma y restricciones. Debe reportar:

- error terminal por escenario;
- Greeks residuales;
- riesgo de base, correlación y volatilidad;
- coste/prima y liquidez;
- dependencia de malla y de modelo.

Esto queda después del motor de payoff: sintetizar antes de tener semántica path-dependent
correcta produciría coberturas falsas para barreras, asiáticas o productos multi-activo.

## 12. Plan por fases

Cada fase termina con build limpio, tests unitarios y de integración; no se encadenan varias
fases sin un resultado verificable.

### Fase 0 — ADR y semántica congelada

- convertir las decisiones de §§2–6 en ADRs cortos;
- fijar nombres, igualdad temporal, signos de cashflow, moneda de reporting y reglas de hit;
- decidir librería JSON y publicar `engine.payoff/v1.schema.json`;
- crear fixtures de call, forward, swap, barrier y TP/SL con resultados manuales.

**Aceptación**: schema y ejemplos pasan validación; no quedan decisiones semánticas esenciales
implícitas en el código.

### Fase 1 — AST C++ + evaluator determinista puro

- añadir `payoff/expression.hpp`, `predicate.hpp`, `contract.hpp`, `visitor.hpp`;
- implementar nodos sin estado (`Zero`, `Cashflow`, `Give`, `Both`, `Scale`, `If`, `When`);
- implementar `MarketPath`, `CashflowLedger`, validación, dependencias y explain;
- testear call/put/forward/straddle/swap fijo sobre rutas conocidas.

**Aceptación**: el evaluador reproduce exactamente cashflows manuales multi-fecha/multi-moneda;
faltas de fixing o moneda fallan con ruta de nodo útil.

### Fase 2 — Estado, barreras, TP y SL

- añadir `Trigger`, event store, first-hit, prioridades y valores capturados;
- implementar templates up/down in/out, double barrier y `FirstOf` TP/SL;
- separar monitorización discreta de continua aproximada;
- añadir tests de gap, hit exacto, no-hit, dos hits simultáneos y orden de prioridad.

**Aceptación**: una ruta de test reproduce la fecha/valor del primer hit y nunca paga dos veces
un evento latch; CPU reference y evaluator C++ coinciden.

### Fase 3 — JSON canónico y `PayoffProduct` en el registry

- parser/schema/migrador v1;
- `PayoffProduct : IProduct`, factory `"Payoff"`, hash y explain;
- límites de recursos y errores agregados;
- round-trip AST→JSON→AST y canonical hash estable.

**Aceptación**: C++, Python dict, Excel y C ABI pueden crear el mismo producto sin añadir una
clase nominal.

### Fase 4 — Medidas deterministas genéricas

- `Cashflows`, `ScenarioPayoff`, `PV` por ledger y `DependencyReport`;
- descuento/conversión multi-moneda explícitos;
- bump-and-reval genérico por observable/curva;
- mantener y comparar rutas legacy IRS.

**Aceptación**: call/forward/FXForward deterministas y cashflows de IRS pasan casos dorados; el
resultado informa medida probabilística `DeterministicScenario`.

### Fase 5 — `CompiledPayoff` y kernel Rust Q

- representación plana versionada y bridge CXX;
- ejecución pathwise escalar de referencia en Rust;
- interfaz de capacidades/generación de observables de `IModel`;
- agregación `E_Q[discounted cashflows]`, error estándar e intervalos de confianza;
- Black-Scholes/GBM Q como primer modelo equity y call europea como caso cerrado.

**Aceptación**: Monte Carlo converge a Black-Scholes dentro del intervalo estadístico; cambiar
el drift físico no afecta un precio Q; pedir un observable no generado falla en preflight.

### Fase 6 — Barreras bajo Q y exposición

- barrera discreta vectorizada;
- Brownian bridge para modelos compatibles;
- hit probability Q como medida separada del PV;
- perfil de exposición pathwise a partir del mismo AST y netting explícito.

**Aceptación**: convergencia de barreras al refinar malla, comparación con fórmulas/benchmarks y
prueba que diferencia monitorización discreta/continua.

### Fase 7 — Modelos P y escenarios de estrategia

- configuración física separada (drift, distribución, calibración/estimación);
- `Forecast`, `HitProbabilityP`, distribución de P&L y expected shortfall de estrategia;
- TP/SL bajo rutas P sin reutilizar accidentalmente un modelo Q;
- provenance completa en resultado.

**Aceptación**: el motor rechaza combinaciones Q/P inválidas y muestra diferencias esperadas de
hit/P&L en un fixture con drift P distinto de `r-q`.

### Fase 8 — Migrar IRSwap y FXForward

- templates tipadas que generan AST;
- adaptador/specialization del IRS hacia kernels Hull-White actuales;
- FXForward multi-moneda según `PLAN_FXFORWARD.md`;
- equivalencia legacy vs AST para PV, DV01, exposición y CVA donde aplique;
- deprecar, sin borrar aún, constructores nominales públicos.

**Aceptación**: cero regresiones numéricas fuera de tolerancia y `price_many` mezcla IRS,
FXForward y payoff custom sin ramas de producto nuevas.

### Fase 9 — Exercise

- `Exercise` europeo/bermuda;
- Longstaff–Schwartz, diagnóstico de regresión y política de ejercicio exportable;
- tests de límites (americana ≥ europea para put sin dividendos, convergencia por fechas).

**Aceptación**: decisiones reproducibles con seed, sin look-ahead, y explain muestra la política.

### Fase 10 — bindings y experiencia de autoría

- `engine_typed.payoff` completo y plantillas;
- validación/explain en nanobind, C ABI y Excel;
- ejemplos call, barrier, asian, TP/SL, FXForward e IRS;
- documentación de errores y cookbook.

**Aceptación**: los mismos JSON fixtures producen mismo hash y resultados en las cinco capas.

### Fase 11 — optimización y síntesis de cobertura

- grouping por fingerprint, grid y modelo;
- CSE, buffers compactos, vectorización CPU/GPU y benchmark;
- AAD cuando el backend lo permita, fallback bump-and-reval;
- solver de hedge con riesgo residual y restricciones.

**Aceptación**: benchmark publicado, igualdad CPU/GPU y ninguna especialización cambia el
resultado más allá de tolerancia declarada.

## 13. Estrategia de pruebas

### 13.1 Unitarias

- cada nodo escalar, predicado y contrato;
- validación de ciclos, ids, schedules, monedas y números no finitos;
- first-hit y estado latch;
- ledger, signo de `Give`, suma de `Both`, scaling y ramas.

### 13.2 Propiedades/metamórficas

- `Both(x, Zero) == x`;
- `Give(Give(x)) == x`;
- `Scale(0, x) == Zero` en cashflows;
- permutar hijos de `Both` no cambia el ledger agregado;
- un knock-in + knock-out complementarios reproducen el underlying salvo rebates;
- aumentar el take-profit de una estrategia larga no puede adelantar su hit en la misma ruta;
- serializar/deserializar conserva hash y semántica.

### 13.3 Financieras

- call europea contra Black-Scholes;
- put-call parity;
- forwards y bonos contra fórmulas cerradas;
- IRS AST contra implementación actual;
- barreras contra referencia analítica o Monte Carlo de alta resolución;
- asiáticas contra fixtures independientes;
- TP/SL con rutas manuales que contengan gaps y hits simultáneos.

### 13.4 Diferenciales y no regresión

- evaluator C++ vs Rust reference;
- CPU vs GPU;
- generic IR vs specialized kernel;
- API C++ vs Python vs C ABI vs Excel fixture;
- snapshot de resultados legacy de todas las suites actuales.

### 13.5 Robustez

- fuzzing del parser JSON y del validator;
- límites de profundidad/tamaño/número de fechas;
- determinismo con seed;
- thread safety: el AST compartido no muta y cada ruta posee su `RuntimeState`;
- sanitizers en CI para el evaluator/bridge.

## 14. Observabilidad y auditabilidad

Cada valoración debe poder devolver, opcionalmente:

- hash y versión del payoff;
- dependencias de observables y fechas;
- medida Q/P/escenario;
- modelo y parámetros identificables (sin secretos);
- malla efectiva y aproximación de barrera;
- seed, paths, error estándar;
- cashflows agregados y procedencia por nodo/evento;
- especialización utilizada o ruta genérica;
- warnings explícitos de extrapolación, fixings históricos ausentes o aproximaciones.

`ExplainVisitor` será una función de producto, no texto ensamblado dentro de cada medida.

## 15. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| AST «universal» demasiado grande | núcleo mínimo + extensiones versionadas; no añadir nodos por comodidad |
| mega-`MarketSnapshot` | `MarketDataView`/catálogo por ids y adaptador retrocompatible |
| rendimiento del Composite virtual | compilar una vez a IR plano; no recorrer objetos en cada path |
| diferencias C++/Rust | un schema/IR canónico y tests diferenciales |
| barrera continua mal vendida | capability check + Brownian bridge; nunca fallback silencioso |
| TP/SL ambiguos | métrica, prioridad, fixing y settlement explícitos |
| confusión Q/P | metadata obligatoria y validación de compatibilidad |
| ejercicio con look-ahead | política separada, entrenamiento/evaluación controlados |
| rotura de APIs existentes | evolución aditiva, adaptadores y deprecación por fases |
| JSON hostil o enorme | schema estricto, límites y fuzzing |
| explosión de combinaciones | fingerprints, CSE, agrupación por dependencia y lotes |

## 16. Fuera de alcance inicial

- lenguaje Turing-completo o scripts arbitrarios dentro del payoff;
- llamadas de red o acceso a feeds desde nodos;
- calendarios jurídicos completos antes de integrar un motor de dates;
- colateral/CSA, close-out, FVA/MVA/KVA como nodos de payoff (pertenecen a netting y medidas);
- réplica perfecta garantizada de cualquier portfolio;
- ejercicio americano continuo en la primera versión;
- mark-to-market recursivo como métrica TP/SL v1;
- ocultar aproximaciones de malla o completar observables faltantes con cero.

## 17. Definition of Done del motor

El motor se considerará implantado, no solo prototipado, cuando:

- un producto nuevo vanilla o path-dependent pueda añadirse como template/JSON sin editar
  `product.hpp`, `measure.cpp`, `bootstrap.cpp` ni Rust FFI;
- call, forward, IRS, FXForward, barrier, asian y TP/SL estén expresados con las mismas
  primitivas;
- Q/P/escenario sean parte verificable del request y del resultado;
- barreras con estado y primer hit funcionen sobre paths y lotes;
- CPU/Rust y bindings compartan schema, hash y fixtures;
- los kernels legacy se usen solo como especializaciones comprobadas contra la ruta genérica;
- todos los errores de capacidad se detecten antes de simular;
- exista explain de dependencias, eventos y cashflows;
- suites C++, Rust, Python, C ABI y Excel estén en verde;
- benchmarks demuestren que la abstracción no impide la ejecución vectorizada.

## 18. Orden recomendado inmediato

El primer incremento útil debe ser **Fases 0–2**, limitado al evaluator determinista C++:

1. congela semántica y schema sin tocar pricing existente;
2. demuestra composición con call/forward/straddle;
3. demuestra path dependency real con barreras y TP/SL;
4. produce un ledger auditable antes de introducir Monte Carlo, Q/P o GPU.

Después, Fases 3–5 conectan el producto al registry actual y a Rust bajo Q. Migrar IRS o
implementar exercise antes de tener esa ruta de referencia mezclaría demasiadas fuentes de
riesgo en un único cambio.

---

*Decisión central: los nombres de producto sobreviven como ergonomía y compatibilidad, no como
unidad de implementación. La unidad de implementación es el programa de payoff; los modelos
generan sus observables, los visitors lo interpretan/compilan y las medidas declaran si operan
bajo Q, P o un escenario determinista.*
