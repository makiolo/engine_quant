# PLAN_FXFORWARD.md — FXForward: producto, medidas Q y medidas P

> Documento de análisis + plan de implementación, mismo espíritu que `PLAN_REAPI.md`: no es
> código, es la referencia para decidir diseño antes de tocar `cpp/engine`. Se migra a
> `PLAN.md` como fase(s) `§7.24+` solo cuando quede implementado y verificado end-to-end (mismo
> criterio que ya aplica `PLAN.md` a cada fase cerrada). Estado del motor en el momento de este
> análisis: v0.18, Fase 7.23 cerrada (consistencia de documentación de `engine_typed`).

## 0. Origen y encuadre

Se pide un segundo producto real del motor — `FXForward` — con dos ángulos de uso explícitos:

- **Dealer XVA**: valorar y cubrir el forward, es decir, sensibilidades estáticas a los
  observables de mercado (spot, curva doméstica, curva extranjera, basis cross-currency).
- **Take risk** (mesa/gestor de posición): cuánto se espera ganar por el mero paso del tiempo
  y por el "roll" del mercado hacia lo que la propia curva ya implica, más un P&L explicado
  frente a un escenario futuro concreto.

Medidas pedidas, agrupadas exactamente por ese criterio:

| Ángulo | Medida pedida | Naturaleza |
|---|---|---|
| Dealer XVA (**Q**) | `q.FXDelta()` | sensibilidad a spot |
| Dealer XVA (**Q**) | `q.DomesticPV01()` | sensibilidad a la curva doméstica |
| Dealer XVA (**Q**) | `q.ForeignPV01()` | sensibilidad a la curva extranjera |
| Dealer XVA (**Q**) | `q.BasisDelta()` | sensibilidad al basis cross-currency |
| Take risk (**P**) | `q.Carry()` | P&L de puro paso del tiempo |
| Take risk (**P**) | `q.RollDown()` | P&L de que el spot "ruede" al forward implícito |
| Take risk (**P**) | `q.PnL()` | P&L proyectado/realizado contra un mercado futuro dado |

`q` aquí es el alias de importación habitual de `engine_typed` (`import engine_typed as q`,
ver `clients/python/src/engine_typed/__init__.py`), no una medida de probabilidad — para evitar
la ambigüedad de notación con "Q" y "P" como medidas de probabilidad, este documento llama
**"medidas Q"** a las cuatro sensibilidades estáticas (comparten metodología: bump-and-reval
sobre el `MarketSnapshot` observado *hoy*, exactamente igual que `PV`/`DV01` desde
`PLAN_REAPI.md` Fase 4) y **"medidas P"** a las tres de P&L (comparten metodología: repreciar el
mismo forward en una fecha de valoración futura, bajo un mercado que puede ser "el de hoy sin
cambios" —`Carry`—, "el de hoy rodado a su propio forward implícito" —`RollDown`— o "uno
explícitamente distinto, dado por el usuario" —`PnL`—). Ver §5 para la justificación completa de
por qué esta interpretación (determinista, sin Monte Carlo) es la que se recomienda, y qué se
deja fuera de alcance a propósito.

## 1. Resumen ejecutivo

- **Cero cambios en Rust.** Las siete medidas son funciones deterministas de curvas de
  descuento observadas — ninguna necesita simular una dinámica de tipo corto ni depende de
  ningún `IModel`, igual que `PV`/`DV01` de `IRSwap` dejaron de depender del modelo en
  `PLAN_REAPI.md` Fase 4. Todo el cálculo nuevo vive en `cpp/engine` puro.
- **Un producto nuevo** (`FxForwardProduct`, `product.hpp`/`.cpp`) y **siete medidas nuevas**
  (`measure.hpp`/`.cpp`), registradas en `bootstrap.cpp` — mismo patrón que cualquier
  producto/medida existente (`PLAN.md` §5.4).
- **La decisión de diseño real** no es el producto ni las medidas (mecánicas, ver §4) — es
  **cómo le llegan dos curvas + spot + basis a un `IMeasure::evaluate()` que hoy solo recibe un
  `MarketSnapshot`**. Se analiza en §3 y se recomienda extender `MarketSnapshot` con campos FX
  opcionales (aditivo, cero cambios en la firma de `IMeasure`/`price()`/registry) en vez de un
  tipo de mercado paralelo.
- **Excel no necesita código nuevo** (`ENGINE.CREATE_MARKET`/`ENGINE.CREATE_PRODUCT` ya son
  genéricos sobre un rango clave/valor) — solo el texto de ayuda de la UDF. **C ABI y Python
  (nanobind) sí** necesitan cambios, porque `EngineMarketSnapshot` (struct plano) y el
  constructor de `engine.MarketSnapshot` (kwargs nombrados) no son genéricos como el Excel/dict
  de Python. Detalle completo en §4.4-§4.5.
- **Alcance deliberadamente fuera de este documento**: perfil de exposición FX y CVA/XVA de un
  FXForward bajo Monte Carlo (necesitaría un modelo de difusión de spot nuevo en Rust, el
  equivalente FX de Hull-White) — ver §7.

## 2. Definición financiera y fórmulas

### 2.1 El instrumento

Contrato de intercambio, en la fecha `T` (madurez, años desde `pricing_date`), de un nocional en
divisa extranjera `N_for` por un nocional en divisa doméstica `N_dom = N_for · K`, donde `K` es
el tipo forward contratado (unidades de doméstica por unidad de extranjera). `buy_foreign=true`
(default) significa "recibe `N_for`, paga `N_dom`"; `buy_foreign=false` invierte el signo.

Observables de mercado necesarios (todos "fuera del trade", igual que `hazard_rate`/
`recovery_rate` ya son observables fuera de `IrSwapProduct` — `market.hpp` líneas 63-66):

- `S`: spot FX, unidades de doméstica por unidad de extranjera.
- `DF_dom(t)`: factor de descuento de la curva doméstica (la `Curve` que `MarketSnapshot` ya
  tiene hoy — sin cambios).
- `DF_for(t)`: factor de descuento de la curva extranjera **local** (nueva).
- `spread_basis(t)`: spread cross-currency (nuevo, opcional — ausente equivale a 0 en todo `t`),
  aplicado a la curva extranjera para obtener el descuento extranjero **efectivo** (el que hace
  el forward de mercado consistente con paridad de tipos cubierta cuando el colateral está en
  doméstica):

  ```
  DF_for_eff(t) = exp(-(foreign_curve.zero_rate(t) + basis_curve.zero_rate(t)) · t)
  ```

  Separar `foreign_curve` (riesgo de tipo extranjero puro) de `basis_curve` (riesgo de basis) es
  lo que permite que `ForeignPV01` y `BasisDelta` midan dos riesgos distintos en vez de uno
  mezclado — es la misma razón por la que un swap básis real se cotiza y se cubre por separado
  del tipo extranjero.

### 2.2 PV (medida Q base, no pedida explícitamente pero necesaria como bloque común)

```
F_mkt(T) = S · DF_for_eff(T) / DF_dom(T)                    // forward implícito de mercado

PV = sign · N_for · (F_mkt(T) - K) · DF_dom(T)
   = sign · [ N_for · S · DF_for_eff(T)  -  N_dom · DF_dom(T) ]
```

`sign = +1` si `buy_foreign`, `-1` si no. `K` "a la par" (`forward_rate` ausente, mismo sentinel
que `IrSwapProduct::use_par_rate()`) es `K_par = F_mkt(T)` → `PV = 0` algebraicamente, igual que
un IRS a la par.

Esta fórmula es el único cálculo real; todas las medidas de abajo son bump-and-reval o
desplazamientos de fecha de valoración sobre esta misma fórmula — se implementa **una vez** como
función libre reutilizada por las siete medidas (mismo principio de composición que
`UnilateralCvaMeasure` reutilizando `ExposureProfileMeasure::evaluate`, `measure.hpp` línea 66).

### 2.3 Medidas Q — sensibilidades estáticas (bump-and-reval sobre el mercado de hoy)

Mismo patrón exacto que `Dv01Measure` (`measure.hpp` líneas 118-133): un `bump` configurable vía
`Params`, default razonable, `bucketed` opcional para desglosar por pillar en vez de un bump
paralelo.

- **`FXDelta(bump=0.01)`** — bump **relativo** de spot (1% por defecto; un bump aditivo fijo no
  tiene sentido entre pares con pip sizes muy distintos, p.ej. EURUSD vs USDJPY):
  `FXDelta = PV(S·(1+bump)) - PV(S)`.
- **`DomesticPV01(bump=0.0001, bucketed=false)`** — bump paralelo de `zero_rates` de la curva
  **doméstica**, foreign/basis/spot fijos. `bucketed=true`: un delta por pillar de la curva
  doméstica (`times`=`market.pillars()`), igual forma que `DV01(bucketed=True)` hoy.
- **`ForeignPV01(bump=0.0001, bucketed=false)`** — igual pero bump de la curva **extranjera
  local** (`foreign_curve`), basis fija — aísla el riesgo de tipo extranjero del riesgo de
  basis.
- **`BasisDelta(bump=0.0001, bucketed=false)`** — bump paralelo de `basis_curve`. Lanza
  `std::invalid_argument` si el `MarketSnapshot` no trae `basis_curve` configurada — pedir
  `BasisDelta` sin haber puesto una curva de basis es un error de uso, no un cero silencioso
  (ver §3.3).

`DV01` "a secas" (el nombre genérico ya registrado) **no se extiende** a `FxForwardProduct`:
lanza `std::invalid_argument` pidiendo `DomesticPV01`/`ForeignPV01` explícitamente — con dos
curvas de tipo en juego, "la" sensibilidad de curva es ambigua y elegir una en silencio sería
peor que obligar a ser explícito. `PV` sí se extiende (dynamic_cast adicional a
`FxForwardProduct`, ver §4.1) porque no hay ambigüedad posible en un valor presente.

### 2.4 Medidas P — P&L de paso del tiempo y de rodar hacia el forward implícito

Todas requieren `horizon` (años, `0 < horizon < T`, si no `std::invalid_argument`). Se define
`PV(t; S, curvas)` como la fórmula de §2.2 evaluada con madurez residual `t` en vez de `T` (las
curvas no cambian de forma, solo se leen a una madurez residual distinta — nada de esto simula
nada, es releer la misma curva observada hoy con menos tiempo por delante).

- **`Carry(horizon)`** — puro paso del tiempo, spot fijo en el de hoy:

  ```
  Carry = PV(T - horizon; S, curvas) - PV(T; S, curvas)
  ```

- **`RollDown(horizon)`** — incremento *adicional* a `Carry` de que el spot se mueva al valor
  que la propia curva ya implica para esa fecha (`S_fwd(horizon) = S · DF_for_eff(horizon) /
  DF_dom(horizon)`):

  ```
  RollDown = PV(T - horizon; S_fwd(horizon), curvas) - PV(T - horizon; S, curvas)
  ```

  Consecuencia útil como test de consistencia: `Carry + RollDown` es el P&L esperado si el
  mercado se mueve exactamente como la curva de hoy ya lo predice — ni una ganancia ni una
  sorpresa, es la partición estándar de mesa "cuánto gano si no pasa nada inesperado".

- **`PnL(horizon, fx_spot_t1, domestic_zero_rates_t1=None, foreign_zero_rates_t1=None, basis_zero_rates_t1=None)`**
  — P&L proyectado/realizado contra un escenario de mercado futuro explícito, el único de los
  tres que puede reflejar una sorpresa real (spot o curvas distintos de lo que la curva de hoy
  implicaba):

  ```
  PnL = PV(T - horizon; fx_spot_t1, curvas_t1) - PV(T; S, curvas)
  ```

  `curvas_t1` por defecto igual a las de hoy si no se pasan (mismos pillars — **no se admite
  cambiar la malla de vencimientos en v1**, limitación documentada explícitamente, mismo
  criterio que excluye `day_count` de `IRSwap` en `PLAN_REAPI.md` §6 Fase 1: no prometer un
  campo que el cálculo no puede honrar todavía).

## 3. Decisión de arquitectura: cómo le llegan dos curvas a una medida

### 3.1 El problema

`IMeasure::evaluate(model, product, market, pricing, execution)` (`measure.hpp` línea 38-41)
recibe **un** `MarketSnapshot` — una curva de descuento. Un `FxForward` necesita dos curvas
(doméstica + extranjera), un spot y opcionalmente una curva de basis a la vez. Cambiar la firma
de `IMeasure` rompe las cinco medidas existentes de IRS y toda la cadena `price()`/`price_batch`/
`price_many`/`price_grid` (`price.hpp`) — coste alto para un problema que ya tiene precedente
resuelto en este mismo código.

### 3.2 Opción A (recomendada) — extender `MarketSnapshot` con campos FX opcionales

`MarketSnapshot` ya demuestra el patrón exacto que hace falta: `hazard_rate`/`recovery_rate` se
añadieron como campos opcionales (default `0.0`, "datos observables fuera del trade que solo
consume una medida", `market.hpp` líneas 63-66) sin tocar la firma de `IMeasure` ni de
`price()`. Se repite el mismo movimiento:

```cpp
class MarketSnapshot {
    // ... sin cambios ...
    const std::optional<Curve>& foreign_curve() const { return foreign_curve_; }
    const std::optional<Curve>& basis_curve() const { return basis_curve_; }
    std::optional<double> fx_spot() const { return fx_spot_; }
    const std::string& domestic_currency() const { return domestic_currency_; } // "" si no se dio
    const std::string& foreign_currency() const { return foreign_currency_; }   // "" si no se dio
private:
    // ... campos existentes sin cambios ...
    std::optional<Curve> foreign_curve_;
    std::optional<Curve> basis_curve_;
    std::optional<double> fx_spot_;
    std::string domestic_currency_;
    std::string foreign_currency_;
};
```

Constructor `MarketSnapshot(const Params&)` (el dinámico, el que ya usan Excel/dict de Python)
lee claves nuevas, todas opcionales: `"foreign_pillars"`/`"foreign_zero_rates"` (vector<double>,
juntas o ninguna), `"basis_pillars"`/`"basis_zero_rates"` (idem), `"fx_spot"` (double),
`"domestic_currency"`/`"foreign_currency"` (string). El constructor tipado (pillars/zero_rates/
hazard_rate/recovery_rate posicional) sigue existiendo tal cual para IRS — se añade un segundo
constructor tipado, o se amplía el existente con estos cinco parámetros opcionales al final
(default vacíos): a decidir en implementación, ver §4.4.

`IrSwapProduct`/sus medidas ignoran estos campos por completo (igual que `PresentValueMeasure`
ya ignora `hazard_rate` hoy) — cero riesgo de regresión sobre IRS.

**Ventajas**: cero cambios en `IMeasure`/`price.hpp`/el registry/la orquestación de lote
(`price_batch`/`price_many`/`price_grid` siguen aceptando un único `MarketSnapshot`/`IModel`
sin saber que ahora puede llevar datos FX). **Coste**: `MarketSnapshot` acumula un tercer
dominio de datos (tipos, crédito, FX) en una sola clase — aceptable porque sigue siendo "datos
planos observables", no lógica, mismo argumento que ya justificó añadir crédito.

### 3.3 Opción B (rechazada por ahora) — `FxMarketSnapshot` + `IFxMeasure` paralelos

Un tipo de mercado FX propio (domestic `Curve` + foreign `Curve` + spot + basis `Curve`, sin los
campos de IRS/crédito) y una interfaz `IFxMeasure` con su propia firma de `evaluate`, su propio
`Registry<IFxMeasure>`, y una función `price_fx()` paralela a `price()`. Más "limpio" en el
sentido de que `MarketSnapshot` no mezcla dominios, pero **multiplica la superficie**: un
segundo `price_fx`/`price_fx_batch`/... en C++/C ABI/Python/Excel, un segundo `Registries`
paralelo, y ningún trade podría mezclar productos IRS y FX en el mismo `price_many`/`price_grid`
sin unificar de nuevo las dos interfaces más adelante. Mismo criterio que ya usó este proyecto en
`PLAN.md` §7.18 ("no generalizar hasta el segundo calibrador real"): no hay todavía un segundo
producto cross-currency que justifique la interfaz paralela — si aparece uno, es el momento de
revisar esta decisión, no antes.

**Recomendación: Opción A.** Revisar si Opción B compensa el día que exista, por ejemplo, un
cross-currency swap o una opción FX que necesite lo mismo.

### 3.4 `BasisDelta` sin `basis_curve` configurada — por qué lanzar en vez de devolver 0

Se decide explícitamente lanzar `std::invalid_argument` (§2.3) en vez de tratar "sin basis
curve" como "basis plana en cero" para `BasisDelta` en concreto (mientras que **`PV` sí** trata
la ausencia de `basis_curve` como spread cero en todo `t`, porque un FX forward vainilla sin
basis conocido es un caso de uso legítimo y común). La asimetría es intencional: valorar sin
basis es una simplificación razonable por defecto; pedir la sensibilidad a algo que no se
modeló es, casi siempre, un error del llamante que conviene que falle alto y claro.

## 4. Diseño por capa

### 4.1 C++ (`cpp/engine`) — todo el cálculo nuevo vive aquí

- **`market.hpp`/`market.cpp`**: extensión de `MarketSnapshot` descrita en §3.2.
- **`product.hpp`/`product.cpp`**: `FxForwardProduct` — Params: `"notional_foreign"` (double,
  requerido), `"forward_rate"` (double, opcional → `use_par_rate()==true` si falta, mismo
  sentinel que `IrSwapProduct::fixed_rate`), `"maturity"` (double, requerido), `"buy_foreign"`
  (bool, default `true`), `"domestic_currency"`/`"foreign_currency"` (string, opcionales,
  cosméticos — si están presentes y `MarketSnapshot` también los trae, se valida que coincidan;
  si no, no se valida nada).
- **`measure.hpp`/`measure.cpp`**:
  - Función libre `compute_fx_forward_pv(market, fx, double valuation_offset = 0.0,
    std::optional<double> spot_override = std::nullopt)` — implementa §2.2 con madurez residual
    `fx.maturity() - valuation_offset` y spot `spot_override.value_or(market.fx_spot())`; único
    punto de la fórmula, reusado por las siete medidas.
  - `PresentValueMeasure::evaluate` gana una rama `dynamic_cast<const FxForwardProduct*>` junto
    a la de `IrSwapProduct` existente.
  - Siete clases nuevas (`FxDeltaMeasure`, `DomesticPv01Measure`, `ForeignPv01Measure`,
    `BasisDeltaMeasure`, `CarryMeasure`, `RollDownMeasure`, `PnLMeasure`), cada una
    `dynamic_cast` a `FxForwardProduct` (lanza si no coincide, mismo patrón que
    `Dv01Measure::evaluate` hoy) y delegando en `compute_fx_forward_pv` con el mercado/spot
    bumpeado o desplazado en fecha que corresponda.
- **`bootstrap.cpp`**: `registries.products.register_type<FxForwardProduct>("FXForward");` +
  siete `register_type` de medidas con los nombres de §0 (`"FXDelta"`, `"DomesticPV01"`,
  `"ForeignPV01"`, `"BasisDelta"`, `"Carry"`, `"RollDown"`, `"PnL"`).

### 4.2 Rust (`rust/crates/engine-core`) — sin cambios

Documentado explícitamente en el propio `bootstrap.cpp`/futuro `fx_forward.hpp` como comentario:
ninguna de las siete medidas necesita el core Rust (ni simulación ni AAD) — coherente con que
`PresentValueMeasure`/`Dv01Measure` de IRS tampoco lo necesitan desde `PLAN_REAPI.md` Fase 4.

### 4.3 C ABI (`engine/abi.h`/`abi.cpp`) — cambio de layout, versión sube a 4

`EngineMarketSnapshot` es un struct plano (`abi.h` líneas 131-137), no genérico — gana los
mismos cinco campos opcionales de §3.2 expresados como puntero+longitud/escalar (`foreign_
pillars`/`foreign_zero_rates`/`foreign_count`, `basis_pillars`/`basis_zero_rates`/`basis_count`,
`fx_spot` con un flag `has_fx_spot` o un valor sentinel documentado —a decidir en implementación,
`NAN` es una opción ya que `double` no tiene "ausente" nativo—, `domestic_currency`/
`foreign_currency` como `const char*` opcional). Es un **cambio de layout** de un struct público
existente → `engine_abi_version()` sube de 3 a 4, mismo criterio que ya subió a 2 (layout de
`EngineMarketSnapshot` cambió al añadir crédito) y a 3 (calibrador genérico). `engine_abi_
create_product`/`engine_abi_price` no cambian de firma — ya son genéricos (`EngineParam*`,
nombre de medida como string resuelto contra el registry).

### 4.4 Python / nanobind (`clients/python/src/engine_py_ext.cpp`)

`nb::class_<engine::MarketSnapshot>` tiene un constructor con `nb::arg` nombrados explícitos
(`pillars`, `zero_rates`, `hazard_rate=0.0`, `recovery_rate=0.0`, líneas 311-321) — no es
genérico como el dict de Excel. Gana los mismos cinco `nb::arg` opcionales de §3.2 con default
vacío/ausente, más `def_prop_ro` de solo lectura para cada uno (mismo patrón que `pillars`/
`hazard_rate` ya expuestos). `FxForwardProduct` no necesita binding dedicado — se crea vía
`eng.create_product("FXForward", {...})` genérico, igual que `IRSwap` hoy.

### 4.5 `engine_typed` (`clients/python/src/engine_typed/`)

Mismo patrón `BaseModel` + `.to_params()` que todo lo demás en el paquete:

- **`trade.py`**: `FxForward(TradeSpec)` — `product_type="FXForward"`, campos `notional_
  foreign: float`, `forward_rate: float | Literal["PAR"]` (requerido, mismo sentinel `PAR` que
  `IRSwap.fixed_rate`), `maturity: float`, `buy_foreign: bool = True`, `domestic_currency: str |
  None = None`, `foreign_currency: str | None = None`; classmethod `.par(...)` igual que
  `IRSwap.par`.
- **`market.py`**: extender `Market` con los mismos cinco campos opcionales (`foreign_pillars:
  list[float] | None`, `foreign_zero_rates`, `basis_pillars`, `basis_zero_rates`, `fx_spot:
  float | None`, `domestic_currency`/`foreign_currency: str | None`) más un `model_validator`
  que replica en Python la misma validación de invariantes que ya hace para `pillars`/
  `zero_rates` (mismo tamaño, estrictamente creciente) cuando `foreign_pillars`/`basis_pillars`
  están presentes. Se extiende la clase existente (no una `FxMarket` nueva) — coherente con la
  decisión de §3.2 de extender `MarketSnapshot`, no duplicarlo.
- **`measure.py`**: siete subclases de `Measure`, cada una con su `measure_name: ClassVar[str]`
  y los campos de §2.3/§2.4 como atributos `pydantic` con los defaults ya fijados ahí
  (`FXDelta(bump=0.01)`, `DomesticPV01(bump=0.0001, bucketed=False)`, `ForeignPV01(bump=0.0001,
  bucketed=False)`, `BasisDelta(bump=0.0001, bucketed=False)`, `Carry(horizon)`,
  `RollDown(horizon)`, `PnL(horizon, fx_spot_t1, domestic_zero_rates_t1=None, foreign_zero_
  rates_t1=None, basis_zero_rates_t1=None)` — `horizon` requerido sin default en las tres
  últimas, para no esconder una elección de mesa detrás de un valor mágico).
- **`__init__.py`**: exportar `FxForward` y las siete medidas nuevas.

### 4.6 Excel (`clients/excel`)

Sin cambios de código: `ENGINE.CREATE_PRODUCT("FXForward", params)` y `ENGINE.CREATE_MARKET
(params)` ya aceptan cualquier clave vía el rango clave/valor genérico (`engine_excel.cpp`
líneas 214-220). Único cambio: ampliar el texto de ayuda de `ENGINE.CREATE_MARKET` para
mencionar las claves FX opcionales nuevas (mismo lugar donde hoy documenta `hazard_rate`/
`recovery_rate` opcionales).

## 5. Por qué "Q" y "P" se resuelven aquí sin Monte Carlo, y qué queda fuera

Ninguna de las siete medidas pedidas (§0) está en la familia "requiere simular el futuro bajo
una dinámica estocástica" (esa familia, en este motor, es `ExposureProfile`/`UnilateralCVA`,
Monte Carlo sobre el tipo corto de Hull-White). Las cuatro "Q" son sensibilidades estáticas al
mercado observado *hoy* — exactamente la misma familia que `PV`/`DV01` de IRS desde
`PLAN_REAPI.md` Fase 4 (deterministas, curva observada, sin modelo). Las tres "P" son P&L
explicado por desplazamiento de fecha de valoración sobre la misma curva observada — tampoco
necesitan simular nada, solo releer la curva a una madurez residual distinta (`Carry`/
`RollDown`) o contra un mercado alternativo explícito (`PnL`). Es la práctica estándar de mesa
para carry/roll-down/P&L-explain de un forward: no hace falta un modelo de difusión de spot para
calcular "cuánto genero si nada me sorprende" ni "cuánto generé dado que el mercado se movió
así".

**Fuera de alcance de este documento, explícitamente**:

- **Perfil de exposición FX / CVA de un FXForward bajo Monte Carlo** — necesitaría un modelo de
  difusión de spot FX nuevo en Rust (el equivalente FX de `HullWhite1F`/`HullWhite2F`, con su
  propia calibración), y una medida `ExposureProfile`/`UnilateralCVA` específica de FX (la
  existente es específica de `IrSwapProduct`+tipo corto). Es un proyecto en sí mismo, no una
  extensión de este documento — si aparece la necesidad real, es material para un
  `PLAN_FXFORWARD_XVA.md` propio.
- **Cross-currency swaps** (intercambio periódico de intereses en dos divisas, no solo nocional
  a vencimiento) — comparte la infraestructura de `MarketSnapshot` extendida de §3.2 pero es un
  producto distinto, fuera de alcance aquí.
- **Múltiples pares FX / múltiples divisas extranjeras a la vez en un mismo `MarketSnapshot`** —
  el diseño de §3.2 asume exactamente una divisa extranjera por mercado (coherente con "un
  `FxForward` = un par de divisas"); una cartera con varios pares necesitará un `MarketSnapshot`
  por par, no un único mercado con N curvas extranjeras — sin cambios de diseño necesarios aquí,
  solo una limitación a documentar.

## 6. Fases de implementación

Cada fase verificable de forma independiente antes de empezar la siguiente (mismo criterio que
`PLAN.md`/`PLAN_REAPI.md`: build limpio + tests + ejemplo end-to-end).

### Fase 1 — `MarketSnapshot` extendido (C++ puro)

Campos opcionales de §3.2, constructor `Params` actualizado, constructor tipado con los cinco
parámetros opcionales añadidos. **Cero productos/medidas FX todavía** — solo la base. Tests:
`test_registry.cpp` nuevo caso que construye un `MarketSnapshot` con campos FX y verifica que
`IrSwapProduct`/sus medidas existentes siguen dando exactamente los mismos números que antes
(regresión cero sobre IRS). Riesgo: bajo, aditivo.

### Fase 2 — `FxForwardProduct` + `PV` extendido

Producto nuevo, `compute_fx_forward_pv`, rama nueva en `PresentValueMeasure::evaluate`, registro
en `bootstrap.cpp`. Test de sanity: un forward "a la par" (`forward_rate` ausente) da `PV≈0`,
mismo tipo de regresión barata que ya se exige para IRS a la par (`PLAN_REAPI.md` §6 Fase 4).
Verificación manual de la fórmula contra paridad de tipos cubierta con un caso de mano (curvas
planas, basis cero → `F_mkt = S·DF_for(T)/DF_dom(T)` verificable a mano). Riesgo: medio (primera
vez que se toca la fórmula de valoración FX).

### Fase 3 — Medidas Q (`FXDelta`, `DomesticPV01`, `ForeignPV01`, `BasisDelta`)

Las cuatro, con `bucketed` para las tres de curva. Test de consistencia: `sum(bucketed deltas)
≈ DomesticPV01(bucketed=False)` (mismo `bump`), igual patrón que ya exige `test_registry.cpp`
para `DV01` bucketed de IRS. Test explícito de que `DV01` sobre un `FxForwardProduct` lanza
(§2.3). Test de que `BasisDelta` sin `basis_curve` configurada lanza (§3.4). Riesgo: bajo, reusa
`compute_fx_forward_pv` de la Fase 2 sin fórmula nueva.

### Fase 4 — Medidas P (`Carry`, `RollDown`, `PnL`)

Las tres, con validación de `horizon` (`0 < horizon < maturity`). Test de consistencia: sobre un
mercado sintético con curvas conocidas, `Carry + RollDown ≈ PnL` cuando el `fx_spot_t1`/
`curvas_t1` de `PnL` se construyen exactamente como el forward implícito de hoy (mismo caso
"nada sorprendente" descrito en §2.4) — y **distinto** cuando `PnL` recibe un escenario que se
aparta de eso (el caso real de uso: cuantificar la sorpresa). Riesgo: medio (es la parte con más
superficie de parámetros nueva, `PnL` en particular).

### Fase 5 — C ABI (`EngineMarketSnapshot` extendido, versión 3→4)

Cambio de layout descrito en §4.3, `engine_abi_version()` → 4, sonda en C puro
(`examples/abi/abi_c_smoke.c` o equivalente) que crea un `FxForward` y pide las siete medidas.
Riesgo: medio — layout público, cualquier binario pre-compilado contra la versión 3 deja de ser
compatible (esperado y ya es el criterio documentado en `abi.h` para cambios de layout).

### Fase 6 — Python (nanobind) + `engine_typed`

`nb::arg` nuevos de §4.4, luego `FxForward`/`Market` extendido/siete medidas de §4.5. Ejemplo
nuevo `fx_forward_flow_typed.py` (mismo espíritu que `price_flow_typed.py`) que construye un
forward, un mercado con las cuatro curvas (doméstica, extranjera, basis) más spot, y pide las
siete medidas con `q.FXDelta()`, `q.Carry(horizon=...)`, etc. Riesgo: bajo, mecánico sobre las
Fases 1-4 ya verificadas.

### Fase 7 — Excel + consolidación

Texto de ayuda de `ENGINE.CREATE_MARKET`/`ENGINE.CREATE_PRODUCT` actualizado, ejemplo manual en
`clients/excel/README.md` (mismo criterio que IRS: verificación con Excel real queda manual, capa
4 de `PLAN.md` §5.6 solo automatiza C++↔Python). Migración de este documento a `PLAN.md` como
fase(s) nueva(s) una vez las Fases 1-6 estén verificadas end-to-end, actualización de
`README.md`.

## 7. Preguntas abiertas

- [ ] **`fx_spot` ausente en `MarketSnapshot` (Opción A, §3.2)**: ¿`std::optional<double>` en
  C++ (limpio, pero cambia el tipo de retorno de un accessor por primera vez en esta clase) o
  un sentinel (`NAN`, `-1.0` documentado) para mantener todos los accessors devolviendo `double`
  plano como hoy? Se recomienda `std::optional` — es C++17 y ya se usa en otras partes del
  motor (`ICalibrator`, revisar), pero confirmar antes de tocar la C ABI (§4.3), donde `double`
  plano con sentinel es casi obligatorio (structs de C no tienen `optional`).
- [ ] **Nombre de las claves `Params` para las curvas FX** — este documento usa `foreign_
  pillars`/`foreign_zero_rates`/`basis_pillars`/`basis_zero_rates`/`fx_spot`/`domestic_
  currency`/`foreign_currency`; confirmar que no choca con ninguna convención de nombres ya
  fijada en otro sitio del motor (no se ha encontrado ninguna al revisar `market.hpp`/
  `params.hpp` en este análisis).
- [ ] **`DomesticPV01`/`ForeignPV01` bucketed cuando las dos curvas tienen pillars distintos** —
  cada una se buquetiza por sus propios pillars (§2.3), lo cual es correcto pero significa que
  `MeasureResult.times` de `ForeignPV01(bucketed=True)` no coincide con los pillars de
  `market.pillars()` (la curva doméstica) — documentar explícitamente en el docstring de
  `engine_typed.measure.ForeignPV01` para que no se confunda con el DV01 bucketed de IRS, que sí
  usa `market.pillars()`.
- [ ] **`PnL` y pillars distintos entre t0 y t1** — confirmado como fuera de alcance en v1 (§2.4,
  §5); confirmar que es aceptable para el caso de uso real antes de implementar la Fase 4, ya
  que es la limitación más visible de las siete medidas de cara al usuario final.
