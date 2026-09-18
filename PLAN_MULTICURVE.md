# PLAN_MULTICURVE.md — Mercado multicurva: registro de curvas nombradas, bootstrapping, calibración

> Documento de análisis + plan de implementación, mismo espíritu que `PLAN_REAPI.md`/
> `PLAN_FXFORWARD.md`: no es código, es la referencia para decidir diseño antes de tocar
> `cpp/engine`/`rust/crates/engine-core`. Se migra a `PLAN.md` como fase(s) `§7.25+` solo cuando
> quede implementado y verificado end-to-end. Estado del motor en el momento de este análisis:
> `PLAN.md` §7.24 cerrada (fachada `quantdesk`), `PLAN_FXFORWARD.md` sin ejecutar (solo análisis).

## 0. Origen y encuadre

Hoy `qd.Market` es una única curva de descuento (`pillars`/`zero_rates`) más dos escalares de
crédito planos:

```python
swap_market = qd.Market(
    pillars=[0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0],
    zero_rates=[0.031, 0.032, 0.034, 0.0355, 0.038, 0.0395, 0.0405],
    hazard_rate=0.015, recovery_rate=0.40,
)
resultado = eng.price(cp, hw_model, swap_market, ["PV", "ExpectedExposure", "PFE95", "UnilateralCVA"])
```

Esto es literalmente `engine::MarketSnapshot` (`cpp/engine/include/engine/market.hpp`): compone
un único `engine::Curve` (pillars/zero_rates, interpolación lineal en tipo cero, extrapolación
plana) + `hazard_rate`/`recovery_rate` escalares. No hay ninguna noción de "más de una curva".

**Por qué esto no es solo un problema de forma de los datos.** `rust/crates/engine-core/src/
products/irs.rs` (línea 3-4) documenta explícitamente el supuesto actual: *"Valoración por
réplica en bonos cero-cupón bajo curva única (misma curva de descuento y de proyección de la
pata flotante)"*. La pata flotante de `IrSwap::npv` se calcula como `P(t,T0) - P(t,Tn)` leído
del propio modelo de tipo corto (Hull-White) calibrado a esa única curva — no hay ninguna curva
de proyección observada de mercado distinta de la de descuento. Soportar multicurva de verdad
implica, además de rediseñar `Market`, cambiar esa fórmula de valoración.

**Relación con dos análisis previos que tocan el mismo problema desde ángulos distintos, sin
haberlo resuelto de forma general:**

- `PLAN_FXFORWARD.md` §3 (sin ejecutar) ya identificó la misma tensión — un producto que
  necesita más de una curva (`FXForward`: doméstica + extranjera + basis) — y recomendó una
  solución puntual: añadir `foreign_curve`/`basis_curve` como campos opcionales de
  `MarketSnapshot`. Válido para dos curvas fijas y bien nombradas, pero no escala a "una curva
  de proyección por índice/tenor" (EURIBOR3M, EURIBOR6M, €STR, SOFR...), que es lo que pide
  multicurva de swaps IRS.
- `PLAN_PRODUCTS.md` §3 ya diseñó `ObservableId`/`CurveId` namespaced (`"IR.DF.EUR.OIS"`,
  `"IR.FWD.EUR.EURIBOR3M"`) y una interfaz `MarketDataView::discount_factor(CurveId, from, to)`
  para el motor de payoff nuevo (`engine::payoff`) — pero vive solo ahí, desconectada de
  `MarketSnapshot`; `market_snapshot_bridge.cpp` hoy solo expone la curva única.
- `ROADMAP.md` ya anticipa el API de cara al usuario que este problema necesita:
  `q.PV01(curve="USD.OIS")`, `q.BasisDelta(curve="EURUSD.BASIS")`.

**Decisión de este documento: generalizar, no bifurcar.** En vez de una tercera solución ad hoc,
este plan propone un único mecanismo — un **registro de curvas nombradas** — que subsume el caso
de `PLAN_FXFORWARD.md` (dos curvas fijas pasan a ser dos entradas con nombre) y le da un
consumidor real al `CurveId` de `PLAN_PRODUCTS.md`. Si `FXForward` se implementa después de este
plan, debe reusar el registro de aquí en vez de campos `foreign_curve`/`basis_curve` sueltos.

## 1. Decisión de diseño central

`Market` pasa de "una `Curve` + dos escalares" a "un registro de `Curve` nombradas + crédito +
un nombre de curva de descuento por defecto":

```cpp
// cpp/engine/include/engine/market.hpp
class Market {
public:
    const Curve& curve(const std::string& curve_id) const;   // lanza si no existe
    const Curve& discount_curve() const;                     // curve(discount_curve_id_)
    bool has_curve(const std::string& curve_id) const;
    std::vector<std::string> curve_ids() const;

    double hazard_rate() const;
    double recovery_rate() const;
private:
    std::map<std::string, Curve> curves_;
    std::string discount_curve_id_;   // p.ej. "EUR.OIS" — cuál de curves_ descuenta
    double hazard_rate_ = 0.0;
    double recovery_rate_ = 0.0;
};
```

**Compatibilidad retroactiva no negociable** (mismo criterio que ya aplicó `PLAN.md` §7.20 al
extraer `Curve` de dentro de `MarketSnapshot`): el constructor de una sola curva sigue
funcionando sin cambios, traducido internamente a `curves_ = {{"DEFAULT", Curve(pillars,
zero_rates)}}, discount_curve_id_ = "DEFAULT"`. `MarketSnapshot` se renombra a `Market` con un
alias/typedef que mantiene el nombre viejo compilando (mismo patrón que ya usa el código para
`Curve`/`MarketSnapshot` hoy — ver comentario de `market.hpp` líneas 58-73). Ningún trade/medida
que solo use `discount_curve()` nota el cambio.

`CurveId` reusa el `engine::payoff::ObservableId`/`CurveId` que `PLAN_PRODUCTS.md` §3 ya definió
(`cpp/engine/include/engine/payoff/ids.hpp`) — no un tipo nuevo paralelo. Convención de nombres:
namespaced con puntos, `"{CCY}.{ÍNDICE}"` (`"EUR.OIS"`, `"EUR.EURIBOR6M"`, `"USD.SOFR"`), igual
que ya usa `ROADMAP.md` (`curve="USD.OIS"`) y `PLAN_PRODUCTS.md` (`"IR.DF.EUR.OIS"`).

**API Python de destino** (tras Fase 1+9, ver detalle por fase):

```python
swap_market = qd.Market(
    curves={
        "EUR.OIS":        qd.Curve(pillars=[0.5, 1, 2, 3, 5, 7, 10], zero_rates=[...]),  # descuento
        "EUR.EURIBOR6M":  qd.Curve(pillars=[0.5, 1, 2, 3, 5, 7, 10], zero_rates=[...]),  # proyección
    },
    discount_curve="EUR.OIS",
    hazard_rate=0.015, recovery_rate=0.40,
)
cp = qd.IRSwap(..., projection_curve="EUR.EURIBOR6M")  # default = discount_curve, cero cambio de comportamiento si se omite

resultado = eng.price(cp, hw_model, swap_market, ["PV", "ExpectedExposure", "PFE95", "UnilateralCVA"])
```

El constructor legado (`qd.Market(pillars=..., zero_rates=..., hazard_rate=..., recovery_rate=...)`)
sigue aceptado indefinidamente vía shim (Fase 9) — no hay fecha de retirada en este plan.

## 2. Desacoplamiento: curvas como módulo interno, no acoplado a modelos/productos/Monte Carlo

Pregunta explícita: ¿debería `Curve`/`CurveSet` vivir como si fuera una librería aparte, sin
depender de cómo se construyen modelos/productos/Monte Carlo? **Respuesta corta: el acoplamiento
que ya existe es el correcto y no hace falta un crate nuevo — solo hay que no romperlo al añadir
multicurva.** Análisis del estado real (no supuesto):

- **Hoy, `rust/crates/engine-core/src/curve.rs` ya es de facto una hoja del grafo de
  dependencias.** No importa nada de `models`/`products`/`exposure`. La única grieta es que sus
  dos constructores de conveniencia, `synthetic_from_hull_white`/`synthetic_from_hull_white_2f`,
  llaman a `crate::smoke::hull_white_zero_coupon_bond` (fórmula cerrada, no al tipo `Model`) para
  fabricar curvas de prueba/demo — acoplamiento cosmético, no estructural, y ya aislado dentro de
  esos dos métodos.
- **La discretización actual ya es unidireccional y limpia**: `calibration.rs` depende de
  `curve::Curve` (para leer la curva a reproducir) y de `models::hull_white{,_2f}` (para
  construir el modelo que calibra) — `Curve` nunca depende de `calibration`. `products::irs`
  hoy ni siquiera depende de `Curve`: descuenta enteramente vía `ShortRateModel::zero_coupon_bond`
  (fórmula del modelo, no lookup en curva) — ver §0. Es decir: el acoplamiento entre "curvas" y
  "modelos/Monte Carlo" hoy es cero en el camino de valoración, y de un solo sentido
  (`calibration -> curve`) en el camino de calibración.
- **Este plan introduce, a propósito, una dependencia nueva y real**: bajo la Opción A de §3,
  `products::irs::IrSwap::npv` pasa a leer directamente de `CurveSet` (el forward determinista de
  la curva de proyección + el spread congelado en `t=0`) ADEMÁS de seguir usando
  `ShortRateModel::zero_coupon_bond` para descontar. Es coupling genuino, no accidental: la
  fórmula de un swap multicurva necesita las dos cosas a la vez. No es "gran acoplamiento" en el
  sentido problemático (ciclos, dependencia circular, un cambio en `Curve` obligando a tocar
  `Model`) — sigue siendo una dependencia en un solo sentido, `products -> {curve, models}`,
  `curve` nunca sabe que `products`/`models` existen.

**Recomendación: NO extraer `curve`/bootstrap a un crate de Cargo separado.** El workspace ya
tiene crates separados donde hay una razón real de límite (`engine-core` motor puro,
`engine-ffi` frontera `cxx` hacia C++, `engine-abi` frontera C) — ninguna de esas fronteras
existe hoy para "curvas": nadie fuera de `engine-core` necesita `Curve` de forma aislada, y crear
un crate solo para expresar una intención de diseño añade fricción real (visibilidad `pub`,
compilación separada, versión propia) sin un segundo consumidor que la justifique. El nivel de
ceremonia correcto es el que ya hay: un módulo (`crate::curve`) con una regla de dependencia
explícita y vigilada en review, no en el compilador:

- `curve`/`bootstrap` (nuevo, Fase 7-8): **nunca** importa `models::*` ni `products::*` en su
  camino de construcción/interpolación/bootstrap (los dos `synthetic_from_hull_white*` existentes
  son la única excepción tolerada, documentada, y ya aislada).
- `calibration`, `products`, `measures`/`exposure`: pueden depender de `curve` (lectura, nunca
  mutación — `Curve`/`CurveSet` son inmutables, mismo criterio que `MarketSnapshot` en C++, ver
  `market.hpp` línea 58).
- Si en el futuro aparece un segundo consumidor real de "curvas" fuera de `engine-core` (p.ej. un
  servicio de bootstrap standalone), **entonces** se reevalúa extraerlo a crate — no antes
  (YAGNI, mismo criterio que ya aplica el resto del motor a evitar abstracción prematura).

## 3. Decisión pendiente — cómo trata el modelo la curva de proyección bajo Monte Carlo

Esto **no** se decide en este documento; se marca explícitamente para decidirse al llegar a la
Fase 4, con una recomendación de partida:

- **Opción A (recomendada como default) — spread determinista sobre el driver estocástico
  único.** El Hull-White sigue simulando un único tipo corto, calibrado a la curva de descuento.
  El fixing de la pata flotante en `[T_{i-1}, T_i]` se calcula como el forward determinista
  implícito en la curva de proyección en `t=0` (`F_proj(T_{i-1}, T_i)`), más el mismo *spread*
  entre curva de proyección y curva de descuento congelado en `t=0`, aplicado sobre el forward
  que el modelo ya genera en cada trayectoria: `L(t; T_{i-1},T_i) = F_HW(t; T_{i-1},T_i) +
  (F_proj(0;T_{i-1},T_i) - F_disc(0;T_{i-1},T_i))`. Un solo factor estocástico, sin recalibrar
  nada nuevo; el basis es exacto en `t=0` y se transporta sin sesgo de primer orden. Es el
  enfoque pragmático estándar en motores XVA que no necesitan basis estocástico de por sí.
- **Opción B — dinámica estocástica separada** (un segundo factor para el spread proyección-
  descuento, o un HJM/LMM multicurva completo). Correcto pero es un modelo nuevo, no una
  extensión de `Market` — se deja fuera de alcance salvo que la Fase 4 decida que la Opción A no
  basta (p.ej. si se necesita Vega de basis).

Las Fases 0-3 y 5-9 son válidas bajo cualquiera de las dos opciones (son de datos/API, no de
dinámica); solo la Fase 4 depende de esta decisión.

## 4. Fases

### Fase 0 — ADR y nombres

- Documentar en `docs/adr/` la decisión de la Fase 1 (registro de curvas nombradas vs. campos
  ad hoc de `PLAN_FXFORWARD.md`) y la convención de `CurveId` (`"{CCY}.{ÍNDICE}"`).
- Confirmar con `PLAN_FXFORWARD.md` (si se retoma) que reusará este registro en vez de
  `foreign_curve`/`basis_curve`.
- **Criterio de aceptación**: ADR mergeado, sin código.

### Fase 1 — `engine::Market` (C++): registro de curvas nombradas

- `market.hpp`/`.cpp`: `MarketSnapshot` → `Market` (alias `MarketSnapshot = Market` para no
  romper compilación existente), `curves_: std::map<std::string, Curve>` +
  `discount_curve_id_`. Constructor legado (`pillars`, `zero_rates`, `hazard_rate`,
  `recovery_rate`) delega en el nuevo (`curves={"DEFAULT": Curve(...)}`, `discount_curve="DEFAULT"`).
  Constructor nuevo: `Market(std::map<std::string, Curve> curves, std::string discount_curve_id,
  double hazard_rate = 0.0, double recovery_rate = 0.0)`.
- `Market(const Params&)`: acepta tanto la forma plana legada (`pillars`/`zero_rates`) como una
  forma nueva serializada (`curve_ids: vector<string>`, y por cada id `"curve.{id}.pillars"`/
  `"curve.{id}.zero_rates"` dentro del mismo `Params` plano — `Params` no admite mapas anidados,
  ver `engine/params.hpp`) + `discount_curve_id: string`.
- `bump_market_parallel`/`bump_market_pillar`/`bump_market_credit`: firma gana un parámetro
  `curve_id` (default = `discount_curve_id()`, preserva comportamiento actual de todos los
  llamadores existentes sin tocarlos).
- **Criterio de aceptación**: `test_registry.cpp`/tests de `market.hpp` existentes pasan sin
  modificar; nuevos tests: `Market` con 2+ curvas, lookup por `CurveId`, error explícito
  (`std::invalid_argument`) al pedir una curva que no existe, roundtrip del constructor legado.

### Fase 2 — Rust `engine-core`: `CurveSet` + IRS multicurva determinista

- `rust/crates/engine-core/src/curve.rs`: nuevo `CurveSet { curves: HashMap<String, Curve>,
  discount_id: String }`, con `discount(&self) -> &Curve` y `get(&self, id: &str) -> &Curve`.
  `Curve` en sí no cambia (sigue siendo pillars/zero_rates puros).
- `products/irs.rs`: `IrSwap::npv` gana un `projection_curve_id: Option<String>` (`None` =
  comportamiento actual, pata flotante 100% réplica del modelo). Cuando hay proyección
  explícita, la pata flotante se recalcula fixing a fixing con el forward de la curva de
  proyección (fórmula de la Opción A de §3 — aplicable ya en Fase 2 aunque la decisión formal de
  §3 se cierre en Fase 4, porque el caso determinista, sin ruido de modelo, es idéntico bajo
  A o B cuando solo hay un factor estocástico).
- Reescribir el docstring de cabecera de `irs.rs` (líneas 1-16), que hoy afirma "curva única"
  como limitación permanente — ya no lo es.
- **Criterio de aceptación**: test nuevo — swap con curva de proyección distinta de la de
  descuento reproduce exactamente el PV de réplica estática (`sum tau_i (F_proj_i - K) DF_disc_i`)
  fuera de Monte Carlo; con curva de proyección = curva de descuento, resultado idéntico byte a
  byte al `IrSwap::npv` actual (test de no-regresión).

### Fase 3 — Wiring de medidas + bridge al motor de payoff

- `cpp/engine/src/measure.cpp`: `ExposureProfileMeasure`/`UnilateralCvaMeasure`/`Dv01Measure`
  pasan `market.discount_curve()` para descuento como hoy, y (si el producto la pide)
  `market.curve(product.projection_curve_id())` para la proyección — sin romper productos que no
  la piden.
- `IrSwapProduct` (`product.hpp`): nuevo campo opcional `projection_curve_id` en `Params`
  (default = ausente → comportamiento actual).
- `cpp/engine/src/payoff/market_snapshot_bridge.cpp`: el `MarketDataView` que ya diseñó
  `PLAN_PRODUCTS.md` §3 (`discount_factor(CurveId, from, to)`) pasa a leer del registro completo
  de `Market` en vez de una sola curva — primer consumidor real de ese diseño.
- **Criterio de aceptación**: `test_price_market_discounting.py`, `test_irs_templates.cpp`,
  `test_market_snapshot_bridge.cpp` pasan sin modificar (curva única sigue siendo el camino por
  defecto); nuevo test end-to-end Python: `PV`/`DV01` de un IRS con `projection_curve` distinta
  de `discount_curve` vía `eng.price(...)`.

### Fase 4 — Cierre de la decisión pendiente (§3): dinámica de la proyección bajo MC

- Ejecutar solo tras decidir explícitamente Opción A vs B con el usuario (no se asume aquí).
- Si A: extender la Fase 2 (ya determinista para PV) al perfil de exposición Monte Carlo —
  `ExposureProfileMeasure` aplica el mismo spread congelado en cada nodo de simulación.
- Si B: nuevo modelo (`HullWhiteBasis`/segundo factor), calibrador nuevo, fuera del alcance de
  este plan salvo que se re-analice como su propio `PLAN_*.md`.
- **Criterio de aceptación**: perfil de `ExpectedExposure`/`UnilateralCVA` de un swap multicurva
  converge, en el límite curva de proyección = curva de descuento, al resultado actual sin
  proyección (test de no-regresión obligatorio, igual que Fase 2).

### Fase 5 — Greeks multicurva

- `engine/greeks.hpp`/`.cpp`: `RiskFactorKind::CurveParallel`/bucketed ganan un `curve_id`
  (default = curva de descuento del producto que se esté arriesgando, igual criterio que Fase 1).
- Nuevo `RiskFactorKind::CrossCurveBasis` opcional (sensibilidad al spread entre dos curvas
  nombradas) — solo si Fase 4 resolvió Opción A (el spread es un parámetro de mercado explícito
  bajo esa opción).
- `all_greeks`: al enumerar candidatos automáticamente, itera `market.curve_ids()` en vez de
  asumir una sola curva.
- **Criterio de aceptación**: `DV01`/`BucketedDV01` de la curva de proyección y de la curva de
  descuento se piden y se obtienen por separado sobre el mismo trade multicurva; `all_greeks`
  sobre un `Market` de 2 curvas devuelve el doble de entradas de tipo curva que sobre uno de 1.

### Fase 6 — Calibración multicurva

- `ICalibrator::calibrate`: firma gana qué `curve_id` de `Market` calibrar (default = curva de
  descuento, comportamiento actual sin cambio). Las curvas de proyección **no se calibran** en
  este plan — son datos observados de mercado, no parámetros de un modelo de tipo corto; solo la
  curva de descuento alimenta `HullWhite1FCalibrator`/`HullWhite2FCalibrator`.
- **Generalizar `levenberg_marquardt_2p` (`calibration.rs`) a N parámetros libres**, aunque
  ningún calibrador de esta fase lo necesite todavía (ambos siguen calibrando exactamente 2:
  `a`/`b`). Motivo: es la misma pieza que reaprovecha la Fase 7b (bootstrap global opcional) y
  cualquier calibrador futuro con más de 2 parámetros libres (p.ej. si algún día se calibra
  `sigma` contra instrumentos de volatilidad) — sin esta generalización esos casos duplicarían el
  bucle de amortiguación en vez de reusarlo, que es justo lo que el propio módulo dice evitar
  (comentario de cabecera de `calibration.rs`, línea 43-49). Cambio mecánico: `[f64; 2]` → `Vec
  <f64>`/`nalgebra` (o Gauss-Seidel/eliminación gaussiana con pivote a mano, sin dependencia
  nueva, consistente con `solve_2x2` actual "no hace falta una dependencia de álgebra lineal para
  esto") en vez de la regla de Cramer 2x2; mismo criterio de amortiguación/aceptación de paso,
  sin tocar el resultado numérico de `calibrate_hull_white`/`calibrate_hull_white_2f` (test de
  no-regresión obligatorio: mismo `a`/`b`/`rmse`/`iterations` antes y después, bit a bit si es
  posible).
- **Criterio de aceptación**: `test_calibration.cpp`/`test_calibration.py` pasan sin modificar;
  nuevo test — calibrar contra `market.curve("EUR.OIS")` de un `Market` de 3 curvas da el mismo
  resultado que calibrar contra un `Market` de una sola curva con esos mismos pillars/zero_rates;
  `levenberg_marquardt_np` (generalizado) reproduce exactamente los resultados existentes de
  `levenberg_marquardt_2p` sobre los mismos dos calibradores.

### Fase 7 — Bootstrapping de una curva desde instrumentos

Trabajo nuevo (no existía ni de forma parcial): construir un `Curve` a partir de cotizaciones de
mercado en vez de tipos cero ya dados.

**Por qué el algoritmo central NO es Levenberg-Marquardt.** Bootstrapping y calibración son dos
problemas distintos aunque ambos "ajustan algo a datos de mercado": calibración (Fase 6) tiene
pocas incógnitas (2 parámetros de modelo) y muchas observaciones (un pillar por dato de curva) —
sobredeterminado, sin solución exacta, ahí LM es la herramienta correcta y ya existe en el motor
(`levenberg_marquardt_2p`). Bootstrapping tiene **exactamente una incógnita nueva por
instrumento** (el zero rate de cada pillar nuevo) — determinado, con solución exacta, y
resoluble instrumento a instrumento en orden de madurez creciente porque cada instrumento nuevo
solo depende de pillars ya resueltos (más cortos) y de la propia curva que se está construyendo.
Ese es exactamente el método estándar de bootstrap (QuantLib `PiecewiseYieldCurve` y cualquier
motor de curvas real): un **root-find 1D** (Newton-Raphson, con Brent como fallback si Newton no
converge) por pillar — resolver `NPV_instrumento(zero_rate_del_pillar) = 0` reusando la réplica
de bonos cero-cupón que ya existe (`IrSwap::par_rate`/fórmula de PV) — no un sistema de N
incógnitas simultáneas. Usar LM aquí sería resolver con un martillo genérico un problema que ya
viene descompuesto en N problemas triviales de 1 incógnita; más lento y sin ninguna ventaja
(la solución exacta ya existe, no hay mínimos cuadrados que amortiguar).

- `engine::Bootstrapper`/`Curve::bootstrap(instruments)` (C++, con equivalente en
  `engine-core::curve` si el bootstrap se hace en Rust por reuso de `Curve`): instrumentos
  soportados en un primer corte — depósito (tipo simple, cierra su propio pillar sin root-find,
  fórmula cerrada), swap par (tipo fijo anual, auto-descontado, Newton 1D por pillar sobre "NPV
  del swap par en ese pillar = 0", con Brent como fallback si Newton diverge). Vive en el mismo
  módulo `curve`/`bootstrap` que §2 aísla de `models`/`products` — el root-find 1D no necesita
  ni `ShortRateModel` ni Monte Carlo, solo la réplica determinista de bonos cero-cupón.
- API Python de destino: `qd.Curve.bootstrap(instruments=[qd.Deposit(...), qd.SwapRate(...)])`.
- **Criterio de aceptación**: bootstrap de una curva desde tipos swap par sintéticos (generados a
  partir de un `Curve` conocido, mismo patrón de round-trip que `synthetic_from_hull_white`)
  recupera esa curva dentro de tolerancia numérica.

### Fase 7b — Bootstrap global (opcional, solo si la Fase 7 no basta)

**No forma parte del alcance por defecto** — se activa solo si, al implementar Fase 8
(multicurva), aparece un caso real que el bootstrap secuencial no resuelve bien: instrumentos que
acoplan dos curvas a la vez (p.ej. un basis swap EURIBOR3M-vs-EURIBOR6M que no aporta "una
incógnita nueva por instrumento" a ninguna curva por separado) o un conjunto sobredeterminado de
cotizaciones (más quotes que pillars, se quiere un ajuste suavizado en vez de repreciado exacto).
En ese caso, y solo en ese caso, la pieza correcta **sí es** `levenberg_marquardt_np` (la versión
generalizada de la Fase 6): minimizar en mínimos cuadrados el residuo de reprecio de todos los
instrumentos a la vez sobre el vector completo de zero rates (o sobre los parámetros de una forma
paramétrica tipo Nelson-Siegel-Svensson, si se prefiere una curva suave a una lineal a trozos).
Mismo optimizador genérico reusado, no uno nuevo — motivo adicional para generalizarlo en la
Fase 6 aunque no se use aún.

- **Criterio de aceptación (si se ejecuta)**: sobre el mismo caso sintético de la Fase 8, el
  bootstrap global reproduce el resultado del secuencial dentro de tolerancia cuando los
  instrumentos SÍ se descomponen limpiamente (caso sin basis cruzado); documentar explícitamente
  en qué caso concreto (con datos) el secuencial falla y el global no.

### Fase 8 — Bootstrapping multicurva

- Orden fijo (estándar de mercado): primero la curva de descuento (OIS, auto-descontada con sus
  propios instrumentos), después cada curva de proyección (usa la curva de descuento ya
  bootstrapeada del paso anterior para descontar sus propios swaps/FRAs de proyección — bootstrap
  secuencial dual-curve, no simultáneo, para no acoplar la resolución numérica de N curvas en un
  único sistema). Sigue siendo Newton/Brent 1D por pillar (Fase 7), aplicado dos veces en
  secuencia — ver Fase 7b si este orden resulta insuficiente para algún instrumento real.
- **Criterio de aceptación**: bootstrap de `{"EUR.OIS": ..., "EUR.EURIBOR6M": ...}` desde
  instrumentos sintéticos de ambas curvas recupera ambas dentro de tolerancia; error explícito si
  se pide bootstrapear una curva de proyección antes de que exista su curva de descuento.

### Fase 9 — Bindings (Python / Excel / C ABI)

- **Python** (`quantdesk/market.py`): `Market` (pydantic) gana `curves: Dict[str, Curve]` +
  `discount_curve: str`, con un `model_validator` que acepta la forma legada (`pillars`/
  `zero_rates` sueltos) y la traduce a `curves={"DEFAULT": ...}, discount_curve="DEFAULT"` —
  mismo criterio de compatibilidad que la Fase 1 en C++. `to_params()` serializa al formato plano
  de la Fase 1 (`curve_ids`, `curve.{id}.pillars`, ...).
- **Excel**: ninguna UDF nueva — `ENGINE.CREATE_MARKET` ya es genérica sobre pares clave/valor
  (mismo argumento que ya usó `PLAN_FXFORWARD.md` §1 para llegar a la misma conclusión); solo se
  actualiza el texto de ayuda para documentar las claves `curve.*`.
- **C ABI** (`abi.h`): `EngineMarketSnapshot` (struct plano de una curva) se mantiene intacto por
  compatibilidad binaria; se añade `EngineMarketV2` con un array de `{const char* curve_id; const
  double* pillars; size_t n; const double* zero_rates; ...}` + `discount_curve_id`, y funciones
  `*_v2` paralelas a las que hoy toman `EngineMarketSnapshot` (mismo patrón de versionado que ya
  usa `abi.h` línea 60, "`EngineMarketSnapshot` (gana hazard_rate/recovery_rate). Sube a 3 en
  PLAN.md §7.18").
- **Criterio de aceptación**: `test_quantdesk_context.py`/`test_xloper.cpp`/`test_abi.cpp`
  existentes pasan sin modificar; nuevo test Python de round-trip con `curves={...}` de 2+
  entradas a través de `Engine.price(...)`.

### Fase 10 — Documentación, notebooks, migración

- Actualizar `06_exposure_cva_portfolio.ipynb`/`09_option_strategies_and_greeks.ipynb` (o el que
  aplique) con un ejemplo multicurva real (EUR.OIS descuento + EUR.EURIBOR6M proyección).
- Migrar este documento a `PLAN.md` §7.25+ (mismo criterio de cierre que el resto de
  `PLAN_*.md`) solo cuando las fases anteriores estén verificadas end-to-end.
- Actualizar `ROADMAP.md`: las líneas `q.PV01(curve="USD.OIS")`/`q.BasisDelta(curve=...)` dejan
  de ser aspiracionales.

## 5. Fuera de alcance (explícito)

- Cross-currency (FX spot/forward, `CrossCurrencyModel` de `ROADMAP.md`) — depende de este plan
  (reusa el mismo registro de curvas) pero es su propio `PLAN_*.md`.
- Estructura temporal de `hazard_rate` (curva de crédito, no escalar) — mismo mecanismo de
  registro de curvas nombradas sería reutilizable, pero no se incluye aquí para no mezclar tipo
  de curva de tasa con curva de supervivencia en la primera iteración.
- Bootstrapping con instrumentos de futuros/convexity adjustment — se deja para cuando haya
  demanda real, la Fase 7 cubre depósitos + swaps par (suficiente para reproducir el caso de la
  pregunta original).
