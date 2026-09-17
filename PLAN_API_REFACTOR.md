# PLAN_API_REFACTOR.md — API pythónica unificada para el cliente Python

> Documento de arquitectura y plan de implementación. No propone capacidades nuevas del motor
> (ningún modelo/producto/medida nuevo): reorganiza cómo un script Python llega a esas
> capacidades, sustituyendo el flujo actual de `engine` + `engine_typed` (dos imports, doble
> construcción típed → dict → objeto nativo, resultados indexados por string) por un único
> paquete `quantdesk` con una fachada de un solo nivel. Como todos los `PLAN_*.md` de este
> repositorio, ninguna fase se declara terminada aquí: se migra a `PLAN.md` solo después de
> quedar implementada y verificada de extremo a extremo.

## 0. Motivación y alcance

El README (sección "Quick start with Python") es hoy el primer contacto de cualquiera con el
motor, y el ejemplo que enseña tiene fricciones reales, no cosméticas:

- **Dos imports** (`engine`, `engine_typed`) para una sola tarea — y no es evidente por qué
  hacen falta los dos hasta leer `engine_typed/__init__.py`.
- **Doble construcción**: cada objeto de negocio (`trade`, `model`, `market`, `pricing`,
  `execution`) se construye primero como objeto tipado (`q.IRSwap(...)`) y luego se reconstruye
  a mano como objeto nativo (`eng.create_product(trade.product_type, trade.to_params())`,
  `engine.MarketSnapshot(**market.to_params())`, ...) — cinco líneas de traducción mecánica que
  no aportan nada al lector y que hay que repetir en cada script.
- **`PricingContext`/`ExecutionContext` viajan sueltos** en cada llamada a `price(...)`, cuando
  en la práctica son configuración del motor (cuántos paths, qué backend), no del trade que se
  está valorando — se fijan una vez por sesión de trabajo, no por instrumento.
- **Resultados indexados por string** (`results["PV"].scalar`) en vez de acceso por punto
  (`results.PV.scalar`), más detectable por autocompletado/typo-checking en un notebook.

Esto no es solo el README: el mismo patrón se repite literal en los 4 scripts de
`clients/python/examples/`, en los 10 notebooks de `clients/python/notebooks/`, y en la
documentación asociada. El objetivo de este plan es un único paquete pythónico,
`quantdesk`, que sustituye a `engine_typed` como la forma recomendada de usar el motor desde
Python, y actualizar **todo** lo que hoy enseña el patrón antiguo para que deje de existir un
segundo ejemplo canónico contradictorio.

**Fuera de alcance:** el motor en sí (C++/Rust), el ABI C, y el cliente Excel (XLL) — ver §8 para
la justificación explícita de por qué Excel no entra en este refactor. El módulo compilado
`engine` (nanobind) tampoco se toca: sigue siendo la "fachada dinámica" de bajo nivel que ya
documenta el README (§"Dynamic dict facade"), ahora consumida internamente por `quantdesk` en
vez de por el usuario final. Sí entra en alcance, en cambio, publicar el resultado en PyPI bajo
el nombre `quantdesk` (Fase 10): es la consecuencia natural de que ese pase a ser el nombre
público del paquete recomendado, y el workflow de release ya existente (`release.yml`) es lo que
hoy produce las wheels que habría que subir.

## 1. Antes / después

```python
# Antes -- README.md actual
import engine
import engine_typed as q

eng = engine.Engine()
model = q.HullWhite1F(a=0.10, b=0.03, sigma=0.01, r0=0.02)
trade = q.IRSwap(notional=1_000_000.0, fixed_rate=0.02,
                  payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0] * 5)
market = q.Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02],
                   hazard_rate=0.02, recovery_rate=0.40)
pricing = q.PricingContext(n_paths=5_000, n_steps=208, seed=7)
execution = q.ExecutionContext(backend="auto")

eng_model = eng.create_model(model.model_type, model.to_params())
eng_trade = eng.create_product(trade.product_type, trade.to_params())
eng_market = engine.MarketSnapshot(**market.to_params())
eng_pricing = engine.PricingContext(pricing.to_params())
eng_execution = engine.ExecutionContext(execution.to_params())

results = eng.price(eng_trade, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"],
                     eng_model, eng_market, eng_pricing, eng_execution)
print(results["PV"].scalar)
```

```python
# Después -- este plan
from quantdesk import Engine, HullWhite1F, IRSwap, Market

model = HullWhite1F(a=0.10, b=0.03, sigma=0.01, r0=0.02)
trade = IRSwap(notional=1_000_000.0, fixed_rate=0.02,
                payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0] * 5)
market = Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02],
                 hazard_rate=0.02, recovery_rate=0.40)

engine = Engine(backend="auto", n_paths=5_000, n_steps=208, seed=7)

metrics = ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"]
results = engine.price(trade, model, market, metrics)
print(results.PV.scalar)
```

Diez líneas de traducción mecánica desaparecen; nada de lo que hoy hace `engine_typed`
(validación pydantic, `fixed_rate` obligatorio, `IRSwap.par(...)`, el DSL de `Payoff`) cambia de
comportamiento — solo de sitio y de forma de invocarse.

## 2. Decisiones de diseño

- **`quantdesk` sustituye a `engine_typed`, no convive con él.** Es un `mv` + extensión, no un
  paquete nuevo por encima. Este es un prototipo de investigación pre-1.0 (README: "active
  research prototype") sin usuarios externos de `engine_typed` fuera de este propio repositorio
  (§5 confirma: 53 ficheros lo referencian y todos son de este repo) — no hay motivo para una
  capa de compatibilidad (`engine_typed` reexportando `quantdesk` con un aviso de deprecación).
  Ruptura limpia, todo el repositorio se actualiza en el mismo cambio.
- **El módulo nativo `engine` no se toca.** Renombrarlo (o su símbolo `NB_MODULE`) exigiría tocar
  `clients/python/src/engine_py_ext.cpp`, el build CMake, el paquete de la wheel, el instalador y
  los ejemplos de C ABI que también hablan de "engine" como familia de nombres — coste y riesgo
  muy por encima del problema real, que es puramente de ergonomía Python. `quantdesk` es un
  paquete Python puro (igual que `engine_typed` hoy) que internamente sigue llamando a
  `engine.Engine()`, `engine.MarketSnapshot`, etc.
- **`quantdesk.Engine` es una clase nueva, no un alias de `engine.Engine`.** Envuelve una
  instancia de `engine.Engine()` más un `PricingContext`/`ExecutionContext` ya resueltos en el
  constructor (§3.2) y hace la traducción típed → nativo por dentro. `engine.Engine` (nativo)
  sigue existiendo tal cual para quien use la fachada dinámica de bajo nivel.
- **Resultados con acceso por punto vía un wrapper fino, no un cambio en `engine_py_ext.cpp`.**
  `PriceResult` (§3.3) envuelve el `dict[str, engine.MeasureResult]` que ya devuelve
  `engine.Engine.price(...)`; sigue soportando `results["PV"]` (uso existente en tests/notebooks
  que no merece la pena forzar a reescribir donde el acceso por punto no aporta, p.ej. bucles
  sobre nombres de medida dinámicos) además de `results.PV`.
- **`PricingContext`/`ExecutionContext` pasan a vivir en `Engine.__init__`**, con la opción de
  sobreescribirlos por llamada (`engine.price(..., pricing=..., execution=...)`) para los casos
  ya existentes en el repo que sí varían `n_paths`/`bump`/backend entre llamadas dentro del mismo
  script (p.ej. comparar `DV01(bump=0.0001)` vs `DV01(bump=0.0002)` no necesita re-crear el
  motor, pero un notebook que compara backend `"cpu"` vs `"gpu"` sí necesita poder pasar una
  `ExecutionContext` distinta puntualmente).
- **`price(trade, model, market, metrics)` cambia el orden de argumentos** respecto a
  `engine.Engine.price(product, measures, model, market, pricing, execution)`: `trade` primero
  (foco de la llamada), `metrics` al final (spec variable de la llamada, no configuración fija),
  `model`/`market` en medio en el orden que ya usa el README propuesto por el usuario. Aplica
  igual a `price_batch`/`price_many`/`price_grid`.
- **Medidas tipadas (`q.PV()`, `q.DV01(bump=...)`, `greeks.delta(...)`) se aceptan mezcladas con
  strings** en la lista `metrics`, exactamente igual que hoy — `Engine.price` interno sigue
  aceptando `x.to_spec() if isinstance(x, Measure) else x` por elemento.

## 3. Diseño de `quantdesk`

### 3.1 Qué se mueve tal cual (sin cambio de comportamiento)

Todo el contenido actual de `clients/python/src/engine_typed/` (`trade.py`, `model.py`,
`market.py`, `context.py`, `measure.py`, `greeks.py`, `payoff.py`) pasa a
`clients/python/src/quantdesk/`, con los imports internos (`from engine_typed.measure import
Measure`, etc.) actualizados a `from quantdesk.measure import Measure`. Ninguna clase pydantic
cambia de campos ni de validación — `IRSwap`, `HullWhite1F`/`HullWhite2F`/`GbmBasket`, `Market`,
`Measure`/`PV`/`DV01`/`ExposureProfile`/`UnilateralCVA`, `Greek` y todo el DSL de `payoff.py`
siguen siendo exactamente los mismos objetos, solo con otro nombre de paquete contenedor.

### 3.2 `quantdesk.Engine` — la pieza nueva

```python
class Engine:
    def __init__(self, *, backend="auto", precision="FP64", n_paths, n_steps, seed, pricing_date=0.0):
        self._native = engine.Engine()          # engine.Engine() nativo, una sola vez
        self._pricing = PricingContext(pricing_date=pricing_date, n_paths=n_paths, n_steps=n_steps, seed=seed)
        self._execution = ExecutionContext(backend=backend, precision=precision)

    def price(self, trade, model, market, metrics, *, pricing=None, execution=None) -> PriceResult: ...
    def price_batch(self, trades, model, market, metrics, *, pricing=None, execution=None) -> list[BatchRow]: ...
    def price_many(self, trades, model, market, metrics, *, pricing=None, execution=None) -> list[BatchRow]: ...
    def price_grid(self, trades, models, markets, metrics, *, pricing=None, execution=None) -> list[GridRow]: ...
    def all_greeks(self, trade, metric_name, model, market, *, metric_params=None,
                    include_curve_buckets=False, include_second_order=False): ...
    def hessian(self, trade, metric_name, model, market, *, metric_params=None, risk_factors=None): ...
    def hvp(self, trade, metric_name, model, market, direction, *, metric_params=None): ...
    def simulate_paths(self, model, market): ...
    def calibrate(self, model_type, market, initial_guess): ...   # create_calibrator + .calibrate en un paso
    def list_models(self) -> list[str]: ...
    def list_products(self) -> list[str]: ...
    def list_measures(self) -> list[str]: ...
    def list_calibrators(self) -> list[str]: ...
```

Cada método que hoy exige un objeto nativo (`engine.IProduct`, `engine.IModel`,
`engine.MarketSnapshot`) acepta en su lugar el objeto tipado (`TradeSpec`, `ModelSpec`,
`Market`) y hace la traducción por dentro — el mismo `create_product`/`create_model`/
`MarketSnapshot(**...)` que hoy escribe cada script a mano, una sola vez, dentro de `Engine`.
`price_batch`/`price_many` reciben `trades: list[TradeSpec]`; `price_grid` recibe además
`models: list[ModelSpec]`/`markets: list[Market]`. `BatchRow`/`GridRow` son dataclasses finas
(`trade_index`, `[model_index, market_index]`, `measures: PriceResult`) que envuelven las
`BatchResult`/`GridResult` nativas igual que `PriceResult` envuelve el dict de `price(...)`.

`Portfolio` (ya "suficientemente pythónico" según su propio comentario en
`engine_py_ext.cpp`, cuatro métodos, sin dict de por medio) se reexporta directamente desde
`engine.Portfolio` sin envoltorio adicional — igual criterio que ya aplica el binding nativo hoy.

### 3.3 `PriceResult` — acceso por punto

```python
class PriceResult(Mapping[str, "engine.MeasureResult"]):
    """Envoltorio de solo lectura sobre {nombre_medida: MeasureResult}: results.PV.scalar
    además de results["PV"].scalar — mismo objeto MeasureResult nativo en ambos casos, esto
    no reimplementa scalar/times/primary/secondary."""
```

Se usa en el retorno de `price(...)` y en `.measures` de cada `BatchRow`/`GridRow`. No hace
falta envolver `GreekResult`/`GreeksReport`/`HessianReport`/`HvpReport`/`CalibrationResult`: esos
ya son objetos nativos con atributos con nombre (`.value`, `.rmse`, `.converged`, ...), no dicts
indexados por string — el problema que resuelve `PriceResult` no existe ahí.

### 3.4 Qué NO cambia

`cpp/`, `rust/`, el ABI C (`examples/abi/`), el XLL de Excel (`clients/excel/`), y el propio
módulo compilado `engine` (nanobind) — cero cambios de comportamiento o de superficie pública en
ninguno de ellos. `quantdesk` es, como `engine_typed` hoy, un paquete Python puro sin paso por
CMake (`pyproject.toml` ya lo empaqueta así, ver Fase 3).

## 4. Fases

### Fase 0 — Mover `engine_typed` → `quantdesk`

Renombrar el directorio (`git mv clients/python/src/engine_typed clients/python/src/quantdesk`),
actualizar los imports cruzados internos (`from engine_typed.X import Y` →
`from quantdesk.X import Y` en `payoff.py`, `greeks.py`, `__init__.py`), y el docstring de
`__init__.py`. Sin `Engine`/`PriceResult` todavía — es un rename puro, comportamiento idéntico.

**Criterio de aceptación.** `import quantdesk as q` expone exactamente lo mismo que hoy expone
`import engine_typed as q`; `engine_typed` deja de existir en el árbol (no queda como alias).

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- `git mv clients/python/src/engine_typed clients/python/src/quantdesk` preserva historia (8
  ficheros `.py` detectados como `R` — rename — por `git status`; los `__pycache__/*.pyc`
  presentes en el árbol de trabajo no estaban trackeados por git, así que no aparecen en el
  `mv` y desaparecen del directorio nuevo sin acción adicional).
- Búsqueda exhaustiva (`grep -rn engine_typed` sobre todo `clients/python/src/quantdesk/`, no
  solo los tres ficheros que nombraba el plan) encontró referencias en **5** ficheros, dos más
  de los explícitamente citados (`payoff.py`, `greeks.py`, `__init__.py`): también `market.py`
  (docstring, "consistente con el resto de `engine_typed`") y `model.py` (docstring, dos citas:
  "`TradeSpec` (`engine_typed.trade`)" y "Sin depender de `numpy` (`engine_typed` no lo importa
  hoy...)"). `trade.py`, `context.py` y `measure.py` no tenían ninguna referencia interna —
  confirmado por grep, no por inspección visual únicamente.
- Todas las referencias eran o bien imports (`from engine_typed.X import Y` → `from quantdesk.X
  import Y`, incluido `from engine_typed import greeks` → `from quantdesk import greeks` en
  `__init__.py` y `greeks.py`) o bien prosa de docstring/comentario citando el nombre del
  paquete (incluidos los ejemplos `import engine, engine_typed as q` dentro de docstrings de
  `__init__.py`, `payoff.py`, `greeks.py`) — no había nombres de logger, variables de entorno ni
  literales de string usados en lógica (p.ej. para introspección) que dependieran del nombre
  `engine_typed`, así que no hubo ninguna decisión ambigua real: todo lo encontrado por grep se
  cambió 1:1 a `quantdesk` sin excepción, sin necesidad de criterio adicional.
- No se tocó nada fuera de `clients/python/src/quantdesk/` en esta fase, tal y como pide el
  alcance. Confirmado por `grep -rn engine_typed` sobre el resto del árbol tras el rename: sigue
  habiendo referencias en (grupos, sin modificar ninguno, para que las fases correspondientes
  las recojan) — `pyproject.toml` (Fase 3); `.github/workflows/ci.yml` (Fase 3);
  `clients/python/tests/test_engine_typed_{model,payoff,greeks,trade,measure,context}.py`,
  `clients/python/tests/test_portfolio.py`, `clients/python/tests/test_market_products_realistic.py`
  (Fase 6, siguen fallando el import ahora mismo y es lo esperado); los 10 notebooks de
  `clients/python/notebooks/` + su `README.md` (Fase 5); `clients/python/examples/{price_flow,
  price_flow_typed,price_batch_flow,greeks_flow}.py` + `examples/README.md` (Fase 4);
  `README.md`, `clients/python/README_PYPI.md`, `docs/schema/engine.payoff/cookbook.md`,
  `clients/excel/README.md`, `clients/excel/tests/test_xloper.cpp`,
  `.claude/skills/execute-plan/SKILL.md` (Fase 7); `PLAN.md`, `PLAN_REAPI.md`, `PLAN_GREEKS.md` y
  demás `PLAN_*.md` históricos + citas cruzadas en `rust/crates/engine-core/src/payoff/{mod,
  compile,ir,basket_api}.rs`, `rust/crates/engine-core/src/models/gbm_basket.rs`,
  `rust/crates/engine-ffi/src/lib.rs`, `cpp/engine/tests/payoff/test_payoff_fixtures_cross_layer.cpp`
  (explícitamente fuera de alcance de todo el plan, Fase 7).
  - **Hallazgo no cubierto explícitamente por ninguna fase nombrada del plan:**
    `clients/python/src/engine_py_ext.cpp` (el binding nanobind del módulo `engine`, que el plan
    dice no tocar en su superficie pública) tiene **5** comentarios/docstrings C++ que citan
    `engine_typed` como referencia textual (líneas ~56, ~100, ~177, ~627, ~1011 en el momento de
    esta sesión — p.ej. `">>> from engine_typed import greeks\n"` dentro de un docstring
    embebido, y un comentario que cita un futuro `engine_typed/portfolio.py`). El plan (§5, fila
    "Documentación", y Fase 7) no lista este fichero entre los afectados. Se deja intacto en esta
    fase (fuera de `quantdesk/`, y ninguna fase posterior lo nombra tampoco) — el orquestador
    debería decidir si se cuela en Fase 7 (mismo criterio que el resto de comentarios de
    documentación) o si es una omisión del plan a corregir explícitamente antes de darlo por
    cerrado, porque si no se toca, el criterio de aceptación de Fase 7 ("`grep -rn engine_typed`
    ... no devuelve nada" salvo la lista explícita de exclusiones) fallaría por este fichero.
- Verificación de import: con el intérprete `S:\Projects\engine_quant\venv\Scripts\python.exe`,
  `sys.path.insert(0, 'clients/python/src'); import quantdesk as q; sorted(dir(q))` ejecuta sin
  error (no requiere el módulo nativo `engine` compilado: ningún fichero de `quantdesk/` hace
  `import engine` a nivel de módulo, solo lo mencionan en docstrings). `q.__all__` tras el rename
  es literalmente idéntico, elemento a elemento y en el mismo orden, al `__all__` de
  `git show HEAD:clients/python/src/engine_typed/__init__.py` (mismos 70 símbolos: `PAR`,
  `TradeSpec`, `IRSwap`, `ModelSpec`, `HullWhite1F`, `HullWhite2F`, `GbmBasket`, `Market`,
  `PricingContext`, `ExecutionContext`, `Measure`, `PV`, `DV01`, `ExposureProfile`,
  `UnilateralCVA`, `Greek`, `greeks`, `PayoffProduct`, `ScalarExpr`, `Predicate`, `Contract`, y
  todo el DSL de `payoff.py`). No se compararon los tests existentes (`test_engine_typed_*.py`)
  a propósito — siguen importando `engine_typed` y es esperado que fallen hasta la Fase 6.

### Fase 1 — `quantdesk.Engine` + `PriceResult`

Implementar la clase de §3.2 (constructor + `price`) y `PriceResult` de §3.3, en un módulo nuevo
dentro de `quantdesk/` (p.ej. `quantdesk/engine.py`, sin colisión: es `quantdesk.engine`, un
import absoluto de `engine` — el módulo nativo — sigue resolviendo al paquete top-level). Añadir
a `__init__.py`.

**Criterio de aceptación.** El bloque "Después" de §1 de este documento se ejecuta tal cual
contra un build local y produce los mismos números que el bloque "Antes".

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- Implementado `clients/python/src/quantdesk/engine.py` (`quantdesk.engine`, sin colisión con el
  paquete top-level `engine`: verificado explícitamente que `quantdesk.engine._native is
  (import engine as top_engine)` da `True` — el import absoluto `import engine as _native`
  dentro de `quantdesk/engine.py` resuelve al módulo nanobind compilado, nunca a sí mismo).
  Contiene `Engine` (constructor + `price`, exactamente §3.2) y `PriceResult` (§3.3). Ambos
  añadidos a `clients/python/src/quantdesk/__init__.py` y a su `__all__` (al principio de la
  lista, antes de `PAR`).
- `PriceResult` implementado como `Mapping[str, engine.MeasureResult]` (`__slots__ = ("_results",)`,
  `__getitem__`/`__iter__`/`__len__` delegan en el dict envuelto) más `__getattr__` para el
  acceso por punto — `__getattr__` solo se invoca cuando el atributo no existe ya por la vía
  normal (slots, métodos de `Mapping`), así que nunca compite con `_results`/`keys`/`values`/
  `items`/etc.; solo entra en juego para nombres de medida reales. `results.PV` y `results["PV"]`
  son literalmente el mismo objeto `engine.MeasureResult` (verificado con `is`, ver abajo) — no
  se reimplementa `scalar`/`times`/`primary`/`secondary`/`bump_used`/`has_scalar`.
- Traducción típed → nativo dentro de `Engine.price` replica EXACTAMENTE el patrón que ya usan
  `clients/python/examples/price_flow.py` y `price_flow_typed.py`: `eng.create_product(trade.
  product_type, trade.to_params())`, `eng.create_model(model.model_type, model.to_params())`,
  `engine.MarketSnapshot(**market.to_params())`, `engine.PricingContext(pricing.to_params())`
  (dict posicional, no kwargs — confirmado con `engine.PricingContext.__init__.__doc__` ==
  `"__init__(self, params: dict) -> None"`), `engine.ExecutionContext(execution.to_params())`
  (mismo patrón). `metrics` se traduce elemento a elemento con `x.to_spec() if isinstance(x,
  Measure) else x`, igual que ya documenta `quantdesk/measure.py`.
- **Firma real del binding nativo confirmada por introspección** (no asumida del plan a ciegas):
  `engine.Engine.price.__doc__` da `price(self, product: engine.Product, measure_names: list,
  model: engine.Model, market: engine.MarketSnapshot, pricing: engine.PricingContext, execution:
  engine.ExecutionContext) -> dict` — coincide exactamente con lo que el plan (§1 "Antes", §2)
  daba por hecho, sin discrepancia. `create_product`/`create_model` firman `(self, name: str,
  params: dict = {}) -> engine.Product/Model`, también sin discrepancia. `engine.MeasureResult`
  expone `bump_used`, `has_scalar`, `primary`, `scalar`, `secondary`, `times` — el plan (§3.3) solo
  nombra `scalar`/`times`/`primary`/`secondary` explícitamente; `bump_used`/`has_scalar` existen
  también y quedan accesibles igual (no hay nada que envolver: son atributos del objeto nativo).
  Ninguna discrepancia que resolver con criterio propio — el plan describía el binding con
  precisión.
- **Override puntual `pricing=`/`execution=` en `price(...)` no muta `Engine`** (verificación
  explícita pedida por el plan, no solo inspección de código): `Engine.price` calcula
  `active_pricing = pricing if pricing is not None else self._pricing` (mismo patrón para
  `execution`) en una variable local — nunca reasigna `self._pricing`/`self._execution`.
- **Resultado exacto de la verificación funcional** (venv `S:\Projects\engine_quant\venv`,
  módulo nativo en `venv\Lib\site-packages\engine.cp312-win_amd64.pyd`, script de verificación en
  el scratchpad de la sesión, trade/model/market idénticos al bloque "Después" de §1: `IRSwap`
  notional 1,000,000, `fixed_rate=0.02`, pagos anuales 1..5y; `HullWhite1F(a=0.10, b=0.03,
  sigma=0.01, r0=0.02)`; `Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02], hazard_rate=0.02,
  recovery_rate=0.40)`; `n_paths=5000, n_steps=208, seed=7`):
  - `quantdesk.Engine.price(...)` (bloque "Después"): `PV = 948.4537547220389`,
    `DV01 = 480.18800212936185`, `UnilateralCVA = 626.7254432766481`.
  - Flujo nativo manual equivalente (bloque "Antes" sin `engine_typed`, construido a mano con
    `eng = engine.Engine()`, `eng.create_model(...)`, `eng.create_product(...)`,
    `engine.MarketSnapshot(**...)`, `engine.PricingContext(...)`, `engine.ExecutionContext(...)`,
    usando las clases tipadas de `quantdesk` solo para los parámetros): `PV =
    948.4537547220389`, `DV01 = 480.18800212936185`, `UnilateralCVA = 626.7254432766481` —
    **idénticos bit a bit** a los de `quantdesk.Engine`, no una aproximación (comparados con
    `==` en Python, no con tolerancia). `ExpectedExposure.primary`/`PFE95.primary` (vectores de
    8 valores) también idénticos elemento a elemento (`list(...) == list(...)` → `True`).
  - `results.PV.scalar == results["PV"].scalar` → `True`, y además `results.PV is
    results["PV"]` → `True` (mismo objeto, no solo valores iguales) — comprobado para las 5
    medidas del ejemplo, no solo `PV`.
  - No-mutación con overrides: `Engine._pricing`/`Engine._execution` siguen siendo
    `PricingContext(n_paths=5000, n_steps=208, seed=7)`/`ExecutionContext(backend="auto")` tras
    llamar a `price(..., pricing=PricingContext(n_paths=1000, n_steps=50, seed=99))` y a
    `price(..., execution=ExecutionContext(backend="cpu"))` — confirmado leyendo los atributos
    directamente, no solo infiriéndolo del resultado. Verificación numérica más fuerte con una
    medida sensible a `n_paths`/`seed` (`ExpectedExposure`, que sí depende de Monte Carlo, a
    diferencia de `PV`/`DV01` de un IRS vainilla que son deterministas/bump-and-reval y no varían
    con `n_paths`): con `pricing=PricingContext(n_paths=200, n_steps=208, seed=123)` puntual, el
    perfil de exposición de esa llamada difiere del baseline (`[9625.35, 17355.03, 18352.55, ...]`
    vs `[9625.35, 17303.52, 16924.32, ...]` — el override sí tiene efecto real en esa llamada),
    pero una llamada posterior sin override reproduce el baseline exacto
    (`[9625.35, 17303.52, 16924.32, ...]`, `==` elemento a elemento) — confirma que el override
    no deja rastro en `Engine` para llamadas siguientes.
- **Nota para Fase 2/6:** ningún hallazgo que bloquee las fases siguientes. La firma nativa
  coincide con lo que el plan asume en todos los puntos tocados por esta fase
  (`create_product`/`create_model`/`price`/`MeasureResult`/`PricingContext`/`ExecutionContext`/
  `MarketSnapshot`); Fase 2 puede seguir el mismo patrón de introspección (`.__doc__` de cada
  método nativo antes de envolverlo) para `price_batch`/`price_many`/`price_grid`/`all_greeks`/
  `hessian`/`hvp`/`simulate_paths`/`calibrate`/`list_*`, que esta fase no tocó ni introspeccionó.
  `PV`/`DV01` de un `IRSwap` vainilla no dependen de `n_paths`/`seed` (deterministas/bump-and-
  reval) — quien escriba tests de Fase 6 para la no-mutación de overrides debería usar una medida
  Monte Carlo real (`ExpectedExposure`/`PFE95`) para que el test sea significativo, no `PV`/`DV01`
  (con esas dos el test pasaría igual aunque `Engine` sí mutara, por construcción del propio
  producto/medida).

### Fase 2 — Resto de métodos de `Engine`

`price_batch`/`price_many`/`price_grid`/`all_greeks`/`hessian`/`hvp`/`simulate_paths`/
`calibrate`/`list_*`, más `BatchRow`/`GridRow`, más el reexport directo de `Portfolio`.

**Criterio de aceptación.** Cada método nativo de `engine.Engine` tiene un equivalente pythónico
en `quantdesk.Engine` cubierto por al menos un test (Fase 6) — ninguna capacidad de la fachada
dinámica queda solo alcanzable importando `engine` a mano, salvo los casos ya documentados en
§3.4/§2 como fuera de alcance (Portfolio, ver arriba).

### Fase 3 — Empaquetado y CI

- `pyproject.toml`: `wheel.packages = ["clients/python/src/quantdesk"]` (y el comentario que lo
  explica, línea 29-32).
- `.github/workflows/ci.yml`: pasos "Python engine_typed test (...)" y las rutas
  `clients/python/tests/test_engine_typed_*.py` (nombres de paso y de fichero, ver Fase 6).

**Criterio de aceptación.** `python -m pip install .` produce una wheel con `quantdesk` (no
`engine_typed`); CI verde con los nombres nuevos.

### Fase 4 — Ejemplos (`clients/python/examples/`)

`price_flow.py`, `price_flow_typed.py`, `price_batch_flow.py`, `greeks_flow.py`: reescribir cada
uno sobre `quantdesk.Engine`. `price_flow.py` hoy contrasta deliberadamente la fachada dinámica
(`engine` crudo) con `engine_typed` en su docstring — pasa a contrastar la fachada dinámica con
`quantdesk` (mismo rol pedagógico, nuevo nombre). `clients/python/examples/README.md` se
actualiza en la misma fase (describe qué hace cada script).

**Criterio de aceptación.** Los cuatro scripts corren de punta a punta contra el build local con
el mismo output numérico que antes del refactor (se comparan antes/después, no solo "no
lanza excepción").

### Fase 5 — Notebooks

Los 10 notebooks de `clients/python/notebooks/` (`01`...`09` + `demo_registry.ipynb`) importan
`engine_typed as q` y repiten el patrón de doble construcción en la primera celda de setup de
cada uno. Se actualizan a `from quantdesk import Engine, ...` con la construcción directa de
`Engine(...)`; las celdas que usan `all_greeks`/`hessian`/`hvp`/`simulate_paths`/`calibrate`
pasan a los métodos equivalentes de `quantdesk.Engine` (Fase 2). `notebooks/README.md` se
actualiza en la misma fase.

**Criterio de aceptación.** Los 10 notebooks se re-ejecutan de punta a punta (`jupyter nbconvert
--execute` o equivalente) contra el build local sin error, con las mismas cifras que la versión
actual en el repositorio (memoria de este mismo proyecto en `.claude/.../memory/`: los notebooks
ya se verificaron "de verdad" contra el `.pyd` compilado — este refactor no puede bajar ese
listón).

### Fase 6 — Tests

Renombrar y adaptar `clients/python/tests/test_engine_typed_{model,payoff,greeks,trade,
measure,context}.py` → `test_quantdesk_{...}.py` (mismos casos, `import quantdesk as q`). Añadir
tests nuevos para `quantdesk.Engine`/`PriceResult`/`BatchRow`/`GridRow` (Fases 1-2) —
específicamente: `price(...)` da el mismo resultado que el flujo nativo equivalente,
`results.PV.scalar == results["PV"].scalar`, override puntual de `pricing=`/`execution=` en una
llamada no muta el `Engine` para llamadas siguientes. `test_portfolio.py`/
`test_market_products_realistic.py` (que importan `engine_typed` sin llevar ese sufijo en el
nombre de fichero) se actualizan a `quantdesk` in-place, sin renombrar el fichero.

**Criterio de aceptación.** `ctest`/suite Python en verde; ningún test importa `engine_typed`.

### Fase 7 — Documentación

- `README.md`: sección "Quick start with Python" reemplazada por el bloque "Después" de §1;
  "Dynamic dict facade" se reencuadra explícitamente como *"la fachada de bajo nivel que usa
  `quantdesk` por dentro, y que también usan Excel y el ABI C"* en vez de *"la alternativa a
  `engine_typed`"*; el resto de bloques de código del README que hoy usan `eng`/`q` (custom
  products, calibración, batch/grid) se migran al mismo patrón.
- `clients/python/README_PYPI.md`: mismo tratamiento que el README principal (es la página que
  ve quien instala desde PyPI).
- `docs/schema/engine.payoff/cookbook.md`: los fragmentos que muestran `engine_typed.payoff`
  actualizan el import a `quantdesk.payoff` (el DSL en sí no cambia, ver §3.1).
- `clients/excel/README.md` línea ~87 ("expuesto todavía desde Python (`engine_typed`, ver...")
  actualiza la referencia a `quantdesk` — es una nota de una frase, no un cambio de contenido.
- `clients/excel/tests/test_xloper.cpp` línea ~1155: comentario que cita
  `test_engine_typed_payoff.py` como fixture hermana — actualizar el nombre de fichero citado
  (Fase 6).
- `.claude/skills/execute-plan/SKILL.md` línea 46: lista de ejemplos de "sitios típicos que un
  plan toca" (`engine_typed/*.py`) — actualizar a `quantdesk/*.py`.
- **No se tocan:** `PLAN.md`, `PLAN_REAPI.md`, `PLAN_GREEKS.md`, `PLAN_BACKWARD.md`,
  `PLAN_PRODUCTS.md`, `PLAN_FXFORWARD.md`, `PLAN_IMPROVE_NOTEBOOK.md`,
  `PLAN_IMPROVE_NOTEBOOK2.md` — son actas históricas de fases ya cerradas (mismo criterio
  editorial que ya aplican entre sí, ver cabecera de cada uno); citan `engine_typed` como el
  nombre que tenía en el momento en que se escribieron, y reescribir historia no aporta nada.
  Comentarios de referencia cruzada en Rust/C++ (`rust/crates/engine-core/src/payoff/{mod,
  compile,ir,basket_api}.rs`, `rust/crates/engine-ffi/src/lib.rs`,
  `cpp/engine/tests/payoff/test_payoff_fixtures_cross_layer.cpp`) igual: son citas a estos mismos
  PLAN_*.md históricos, no documentación de `quantdesk` — no ameritan un cambio dedicado.

**Criterio de aceptación.** `grep -rn "engine_typed"` sobre el árbol, excluyendo los
`PLAN_*.md` históricos listados arriba y sus citas en Rust/C++, no devuelve nada.

### Fase 8 — Excel: decisión explícita de no tocar el XLL

El motivo por el que "clientes Python" tiene sentido aislar de Excel: la fricción que resuelve
este plan (doble construcción típed→nativo, imports duplicados, resultados por string) es
específica de tener **dos** capas Python (`engine` + `engine_typed`) para una tarea. Excel no
tiene ese problema — `ENGINE.CREATE_PRODUCT`/`ENGINE.CREATE_MODEL`/`ENGINE.CREATE_MARKET`/
`ENGINE.PRICE` ya son la única capa (no hay una "Excel tipada" por encima), y el encadenado de
handles entre celdas (`Trade`, `Model`, `Market`, `Pricing`, `Compute` como celdas separadas que
alimentan `ENGINE.PRICE`) no es una fricción de ergonomía sino una restricción del modelo de
cálculo de una hoja — cada paso *necesita* su propia celda para que el recálculo incremental
funcione. No hay una versión "más bonita" de eso que siga siendo una hoja de cálculo. Este plan
no toca `clients/excel/` (solo la mención de una frase en su README, Fase 7).

### Fase 9 — Verificación final

- `grep -rn "engine_typed"` limpio salvo lo listado en Fase 7.
- Build completo (`cmake --build build`) + suite Python completa + notebooks re-ejecutados.
- Repasar `clients/python/README_PYPI.md`/`README.md` renderizados (o al menos su Markdown) para
  confirmar que el flujo "Después" de §1 es lo primero que ve un lector nuevo.

### Fase 10 — Publicar `quantdesk` en PyPI

**Problema.** `.github/workflows/release.yml` ya existe y ya construye wheels de Python
(matriz `cp310`...`cp314`, `win_amd64`) más el `.xll`/instalador de Excel, y ya tiene un
`workflow_dispatch` con un textbox `version` (línea 23-26) que decide qué versión se publica —
hoy eso solo produce una Release de GitHub (`publish-release`, `softprops/action-gh-release`)
con los `.whl`/`.zip`/`.exe` como adjuntos. No sube nada a PyPI: hoy `pip install engine-quant`
no funciona salvo instalando desde una wheel descargada a mano. Además, el nombre de
distribución en `pyproject.toml` sigue siendo `engine-quant` (línea 6), no `quantdesk` — hay que
decidir explícitamente que el nombre del paquete PyPI pase a coincidir con el nombre del paquete
Python recomendado (Fases 0-2), no que queden desalineados (`pip install engine-quant` instalando
un `import quantdesk`).

**Tareas.**
- `pyproject.toml`: `[project].name` de `"engine-quant"` a `"quantdesk"` (línea 6) — actualizar
  también el comentario de la línea 13 (referencia a "engine_typed") para que cite `quantdesk`.
  Esto cambia el nombre de fichero de las wheels que ya produce `build-wheels`:
  `engine_quant-X.Y.Z-cpNN-cpNN-win_amd64.whl` → `quantdesk-X.Y.Z-cpNN-cpNN-win_amd64.whl`.
- Ficheros que hoy asumen el nombre de distribución `engine-quant`/`engine_quant` y hay que
  actualizar a `quantdesk` (búsqueda por nombre de distribución, no de import — no confundir con
  `engine_typed`, ya cubierto en Fase 0-9):
  - `clients/python/install/Install-EngineWheels.ps1`: `importlib.metadata.version('engine-quant')`
    (línea ~129) y los comentarios que citan `engine-quant`.
  - `clients/python/install/Uninstall-EngineWheels.ps1`: `pip uninstall -y engine-quant`
    (línea ~38).
  - `installer/EngineQuantSetup.iss` línea ~48: comentario/patrón de borrado que asume el nombre
    de fichero `engine_quant-*.whl` — pasa a `quantdesk-*.whl`.
  - **No se renombra** el branding del instalador completo (`AppName`, `AppPublisher`,
    `DefaultDirName`, `OutputBaseFilename` — "Motor XVA (engine-quant)", carpeta `engine_quant`,
    `engine_quant_setup.exe`): es el instalador Windows todo-en-uno (Python + Excel + XLL), no
    el paquete PyPI — cambiarlo es una decisión de marca aparte, fuera de alcance de un plan
    sobre clientes Python.
- **Input nuevo y propio, desactivado por defecto — no reutiliza el gate de `publish`.** Todavía
  no existe el token/trusted-publisher de PyPI (prerrequisito manual, ver abajo), así que subir a
  PyPI tiene que poder quedar apagado *sin* dejar de poder publicar Releases de GitHub como hoy.
  `workflow_dispatch.inputs.publish_pypi` (`type: boolean`, `default: false`) nuevo, independiente
  de `publish` (que sigue controlando solo la Release de GitHub, sin cambios). Un disparo por tag
  (`push: tags: v*`) tampoco sube a PyPI por sí solo — ese camino no pasa por los inputs de
  `workflow_dispatch`, así que `publish_pypi` en ese caso se trata como `false` salvo que se
  decida lo contrario explícitamente más adelante (fuera de alcance mientras no haya token).
- Nuevo job `publish-pypi` en `.github/workflows/release.yml`, después de `build-wheels`:
  - `needs: [determine-version, build-wheels]`, `if: needs.determine-version.outputs.publish ==
    'true' && inputs.publish_pypi == 'true'` — requiere AMBAS cosas: que se esté publicando de
    verdad (mismo gate que `publish-release`) Y que `publish_pypi` esté marcado a mano en ese
    disparo concreto. Con el default `false`, ningún `workflow_dispatch` ni tag existente hoy
    empieza a subir nada a PyPI por sorpresa — hay que marcarlo explícitamente cada vez, hasta que
    se decida lo contrario.
  - Descarga los artefactos `wheel-cp*` (mismo `actions/download-artifact` con
    `merge-multiple: true` que ya usa `build-installer`, línea ~228-238).
  - Publica con **Trusted Publishing** de PyPI (`pypa/gh-action-pypi-publish@release/v1`, OIDC,
    sin secret de token almacenado) *una vez exista* el "trusted publisher" registrado en
    pypi.org — prerrequisito manual, fuera de lo que el workflow puede hacer por sí mismo:
    registrar `quantdesk` como trusted publisher apuntando a este repositorio/workflow/environment,
    una sola vez. El job necesita `permissions: id-token: write` y, recomendado, un
    `environment: pypi` (permite exigir aprobación manual en GitHub antes de publicar, dado que
    PyPI **no permite volver a subir la misma versión** si algo sale mal — a diferencia de la
    Release de GitHub, que se puede editar o borrar).
  - **Camino inicial real, mientras no exista el trusted publisher:** token clásico en el secret
    `PYPI_API_TOKEN` (se crea la primera vez que se registra el proyecto `quantdesk` en PyPI a
    mano, ahí se genera el token), misma action con `password: ${{ secrets.PYPI_API_TOKEN }}` en
    vez de OIDC. El job se implementa ya con este camino (es lo que hay disponible hoy); migrar a
    Trusted Publishing es un cambio de configuración posterior sin tocar el resto del job, cuando
    convenga.
  - El paso de publicación falla con un mensaje claro si `PYPI_API_TOKEN` no está configurado
    todavía (comportamiento por defecto de la action, no hace falta lógica extra) — aceptable
    porque, con `publish_pypi` en `false` por defecto, ese paso solo se ejecuta cuando alguien lo
    pide explícitamente sabiendo que hace falta el secret.
- Actualizar el comentario de cabecera de `release.yml` (líneas 3-12) para mencionar el nuevo job
  `publish-pypi` y el nuevo input `publish_pypi` (desactivado por defecto) junto a `publish`.

**Criterio de aceptación.** Con `publish_pypi` en su valor por defecto (`false`), ningún
`workflow_dispatch` ni disparo por tag sube nada a PyPI — el comportamiento actual (solo Release
de GitHub) queda intacto. Un `workflow_dispatch` con `publish=true`, `version=X.Y.Z` y
`publish_pypi=true` (y `PYPI_API_TOKEN` configurado) produce, además de la Release de GitHub, un
paquete `quantdesk` publicado en PyPI con esa versión; `pip install quantdesk==X.Y.Z` en un
entorno limpio (Windows, `cp310`-`cp314`) instala y `import quantdesk` funciona.

## 5. Inventario de ficheros afectados

| Grupo | Ficheros | Fase |
| --- | --- | --- |
| Paquete Python | `clients/python/src/engine_typed/*.py` → `clients/python/src/quantdesk/*.py` (+ `engine.py` nuevo) | 0, 1, 2 |
| Empaquetado/CI | `pyproject.toml`, `.github/workflows/ci.yml` | 3 |
| Ejemplos | `clients/python/examples/{price_flow,price_flow_typed,price_batch_flow,greeks_flow}.py`, `clients/python/examples/README.md` | 4 |
| Notebooks | `clients/python/notebooks/{01..09,demo_registry}.ipynb`, `clients/python/notebooks/README.md` | 5 |
| Tests | `clients/python/tests/test_engine_typed_*.py` (renombrados), `test_portfolio.py`, `test_market_products_realistic.py` | 6 |
| Documentación | `README.md`, `clients/python/README_PYPI.md`, `docs/schema/engine.payoff/cookbook.md`, `clients/excel/README.md` (1 línea), `clients/excel/tests/test_xloper.cpp` (1 comentario), `.claude/skills/execute-plan/SKILL.md` (1 línea) | 7 |
| Publicación PyPI | `pyproject.toml` (`[project].name`), `.github/workflows/release.yml` (job `publish-pypi`), `clients/python/install/{Install,Uninstall}-EngineWheels.ps1`, `installer/EngineQuantSetup.iss` (patrón de nombre de wheel) | 10 |
| No se tocan | `clients/excel/` (resto), `cpp/`, `rust/`, `examples/abi/`, `PLAN*.md` históricos y sus citas en Rust/C++, branding del instalador (`AppName`/`DefaultDirName`/`OutputBaseFilename`) | 8, 10, n/a |

## 6. Riesgos y mitigaciones

- **Notebooks silenciosamente desincronizados de su output guardado.** Mitigación: Fase 5 exige
  re-ejecución real, no solo `s/engine_typed/quantdesk/`.
- **Un método nativo de `engine.Engine` sin equivalente pythónico deja una capacidad solo
  accesible con la fachada de bajo nivel.** Mitigación: el criterio de aceptación de la Fase 2
  es explícito (cobertura 1:1 salvo Portfolio, ya justificado).
- **Cambio de orden de argumentos (`price(trade, model, market, metrics)` vs el orden nativo
  `(product, measures, model, market, pricing, execution)`) puede confundir a quien ya conoce la
  API nativa.** Mitigación: documentado explícitamente en §2 y en el docstring de `Engine.price`.
- **Una publicación a PyPI es irreversible** (a diferencia de la Release de GitHub, que se puede
  editar/borrar, PyPI no permite volver a subir la misma versión). Mitigación: `publish=false`
  deja probar el build completo sin tocar PyPI (Fase 10); usar un `environment: pypi` con
  aprobación manual en GitHub como puerta adicional antes de publicar de verdad.
- **Aún no existe el token/trusted-publisher de PyPI** — si el job `publish-pypi` pudiera correr
  igual, fallaría en cualquier intento real. Mitigación: `publish_pypi` nace en `false` por
  defecto (input propio, independiente de `publish`) — el job de PyPI no se ejecuta en ningún
  disparo existente hasta que alguien lo marque a mano sabiendo que `PYPI_API_TOKEN` ya está
  configurado (Fase 10); el comportamiento actual del workflow (solo Release de GitHub) no
  cambia mientras tanto.

## 7. Criterio de aceptación global

`from quantdesk import Engine, HullWhite1F, IRSwap, Market` reproduce el bloque "Después" de §1
tal cual contra un build local; `engine_typed` no existe en el árbol salvo citas históricas
explícitamente excluidas (Fase 7); toda la documentación viva (README, README_PYPI, notebooks,
ejemplos) enseña `quantdesk` como único camino recomendado, con la fachada dinámica (`engine`
crudo) documentada como alternativa de bajo nivel igual que hoy; `pip install quantdesk` instala
el paquete publicado por `release.yml` (Fase 10) y `import quantdesk` funciona igual que el build
local.
