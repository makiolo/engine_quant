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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- Implementado todo en `clients/python/src/quantdesk/engine.py` (mismo fichero de la Fase 1,
  extiende `Engine`): `price_batch`, `price_many`, `price_grid`, `all_greeks`, `hessian`,
  `hvp`, `simulate_paths`, `calibrate`, `list_models`, `list_products`, `list_measures`,
  `list_calibrators`, más `BatchRow`/`GridRow` (dataclasses `frozen=True, slots=True` —
  `requires-python = ">=3.10"` en `pyproject.toml` línea 12, `slots=True` en `dataclass`
  disponible desde 3.10, verificado sin discrepancia). `Portfolio` reexportado en
  `clients/python/src/quantdesk/__init__.py` como `Portfolio = _native.Portfolio` (import
  `import engine as _native` añadido al principio del fichero), sin wrapper — igual criterio
  que ya aplica el binding nativo hoy (comentario en `engine_py_ext.cpp`).
- **Traducción típed → nativo factorizada** (pedido explícito de la tarea, "no dupliques la
  traducción típed→nativo 8 veces"): siete helpers privados nuevos en `Engine`
  (`_to_native_product`, `_to_native_model`, `_to_native_market`, `_resolve_pricing`,
  `_resolve_execution`, `_to_native_pricing`, `_to_native_execution`, `_to_native_metrics`),
  extraídos del cuerpo de `price` de la Fase 1 sin cambiar su comportamiento (`price` se
  reescribió para usarlos, verificado numéricamente idéntico — ver más abajo). Todos los
  métodos nuevos los reutilizan; ninguno repite `create_product`/`create_model`/
  `MarketSnapshot(**...)`/`PricingContext(...)`/`ExecutionContext(...)` a mano.
- **Introspección real del binding nativo ANTES de escribir código** (`.__doc__` de cada
  método sobre `venv\Scripts\python.exe`, mismo patrón que la Fase 1) — resultados exactos:
  - `price_batch(self, products: Sequence[engine.Product], measure_names: list, model, market, pricing, execution) -> list[engine.BatchResult]`.
  - `price_many`: misma firma que `price_batch`, semántica heterogénea (agrupa internamente,
    devuelve en orden de entrada).
  - `price_grid(self, products, measure_names, models: Sequence[engine.Model], markets: Sequence[engine.MarketSnapshot], pricing, execution) -> list[engine.GridResult]`.
  - `all_greeks(self, product, metric_name: str, model, market, pricing, execution, metric_params: dict = {}, include_curve_buckets: bool = False, include_second_order: bool = False) -> engine.GreeksReport`.
  - `hessian(self, product, metric_name, model, market, pricing, execution, metric_params: dict = {}, risk_factors: Sequence[str] | None = None) -> engine.HessianReport`.
  - `hvp(self, product, metric_name, model, market, pricing, execution, direction: dict, metric_params: dict = {}) -> engine.HvpReport` — nótese `direction` posicional ANTES de
    `metric_params`, y ambos DESPUÉS de `pricing`/`execution` (a diferencia de la firma típed
    de §3.2, que pone `direction` justo después de `market` y `metric_params` como kwarg —
    mismo reordenamiento de argumentos ya aceptado por el plan en `price`, §2).
  - `simulate_paths(self, model, market, pricing: engine.PricingContext) -> tuple` — **no
    recibe `execution`** (a diferencia de todos los demás métodos de cómputo).
  - No existe `Engine.calibrate` nativo: el patrón real es `Engine.create_calibrator(name: str) -> engine.Calibrator` + `Calibrator.calibrate(self, market: engine.MarketSnapshot, initial_guess: dict) -> engine.CalibrationResult` — coincide exactamente con el comentario del plan
    (`# create_calibrator + .calibrate en un paso`), sin discrepancia.
  - `list_models`/`list_products`/`list_measures`/`list_calibrators`: `(self) -> list[str]`,
    sin parámetros, tal cual el plan.
  - `engine.BatchResult`: campos `trade_index`, `measures` (confirmado por
    `dir(engine.BatchResult)`, sin más). `engine.GridResult`: `trade_index`, `model_index`,
    `market_index`, `measures` — **nombres de campo idénticos, uno a uno, a los que ya
    proponía el plan en §3.2** ("`trade_index`, `[model_index, market_index]`,
    `measures: PriceResult`"), ninguna discrepancia que resolver con criterio propio.
    `measures` en ambos es un `dict[str, engine.MeasureResult]` igual que el que devuelve
    `Engine.price` — se envuelve con `PriceResult(row.measures)` exactamente igual que en la
    Fase 1, así que `BatchRow.measures.PV.scalar` y `BatchRow.measures["PV"].scalar`
    funcionan igual.
  - `engine.Portfolio`: cuatro métodos (`add`, `size`, `trades`, `price`, `hessian`, `hvp` —
    seis en realidad, el plan decía "cuatro" citando el comentario de
    `engine_py_ext.cpp`, que probablemente contaba solo los cuatro "core"; no se investigó
    más a fondo por estar fuera de alcance de esta fase — Portfolio se reexporta tal cual, sin
    envoltorio, así que el conteo exacto de métodos no cambia nada de la implementación).
    `Portfolio.price(measures, model, market, pricing, execution) -> list[engine.BatchResult]`
    (mismo tipo de retorno que `Engine.price_many`, verificado).
- **Discrepancias resueltas con criterio técnico frente a la firma literal de §3.2** (ninguna
  se inventó en silencio, todas están documentadas aquí y en los docstrings de cada método en
  `quantdesk/engine.py`):
  - `all_greeks`/`hessian`/`hvp` en §3.2 NO listan `pricing=`/`execution=` en su firma, pero
    el binding nativo los exige como argumentos posicionales sin default, y
    `clients/python/notebooks/04_greeks_and_risk_surfaces.ipynb` ya varía `PricingContext`
    entre llamadas dentro del MISMO script sobre el MISMO `engine.Engine()` nativo (`pricing`
    de 200k paths para la call GBM vs `hw_pricing` de 1k paths para el IRS Hull-White,
    celda de `hw_hessian`). Sin un override puntual, `quantdesk.Engine` habría obligado a
    instanciar un segundo `Engine` solo para reproducir ese notebook — contradice el espíritu
    del propio plan (§2: "los casos ya existentes en el repo que sí varían n_paths/bump/
    backend entre llamadas dentro del mismo script"). Se añadió `*, pricing=None,
    execution=None` a los tres métodos, mismo patrón exacto que `price`/`price_batch`/
    `price_many`/`price_grid` (override no muta `self._pricing`/`self._execution`).
  - `simulate_paths` en §3.2 tiene firma literal `(self, model, market)`, sin `pricing`. El
    binding nativo exige `pricing` como tercer argumento posicional (no tiene `execution` en
    absoluto, a diferencia del resto). `clients/python/notebooks/07_montecarlo_paths_q_vs_p.ipynb`
    llama a `simulate_paths` dos veces sobre el MISMO `engine.Engine()` nativo con
    `PricingContext` distintos (`pricing_q`/`pricing_p`, mismo `n_paths`/`n_steps`, semillas
    distintas) para comparar medida Q vs P. Se añadió `*, pricing=None` (default:
    `self._pricing` del constructor) — mismo razonamiento que el punto anterior. Sin este
    parámetro, el criterio de aceptación de la fase ("cubierto por al menos un test... sin
    tener que importar `engine` a mano") no se podría cumplir para este caso de uso real ya
    existente en el repo.
  - Ambas decisiones están documentadas también inline en el docstring de cada método
    afectado en `quantdesk/engine.py`, citando esta misma sección, para que Fase 4/5/6 no
    tengan que redescubrir el razonamiento.
- **Resultado exacto de la verificación funcional** (venv `S:\Projects\engine_quant\venv`,
  `.pyd` nativo ya compilado, sin recompilar C++/Rust — no hizo falta, esta fase es
  puramente Python; script ad-hoc en el scratchpad de la sesión, datos de prueba tomados
  literalmente de `clients/python/examples/price_batch_flow.py`, `greeks_flow.py`,
  `clients/python/notebooks/04_greeks_and_risk_surfaces.ipynb`,
  `07_montecarlo_paths_q_vs_p.ipynb` y `clients/python/tests/test_calibration.py`, 49
  aserciones en total, **todas en verde**):
  - `price_batch` (3 `IRSwap` 5y, `["PV", "UnilateralCVA"]`, `HullWhite1F`): `trade_index`,
    `PV` y `UnilateralCVA` de cada `BatchRow` idénticos bit a bit (`==`) a
    `engine.Engine.price_batch(...)` nativo manual (`PV`: `948.4537547220389`,
    `61254.96451762912`, `-11302.539148803793`); además cada fila coincide con la llamada
    individual `qeng.price(trade, ...)` equivalente.
  - `price_many` (5y, 3y, 5y intercalados, `["PV"]`): 3 filas, `trade_index`/`PV` idénticos
    al nativo manual y a `price()` individual del trade a 3y (`PV = 12691.837573942801`).
  - `price_grid` (2 trades × `[HullWhite1F, HullWhite2F]` × `[market, market_stressed]`,
    `["PV", "UnilateralCVA"]`): **8** celdas (no 4 — el plan no fija el número, es
    `len(trades) × len(models) × len(markets)`; primer intento del script de verificación
    tenía una aserción de conteo equivocada, `4` en vez de `8`, corregida — no era un bug de
    la implementación, era un error del script de prueba), `trade_index`/`model_index`/
    `market_index`/`PV`/`UnilateralCVA` idénticos al nativo manual celda a celda.
  - `all_greeks` (call GBM ATM sobre `AAPL_CALL_100`, `PayoffPriceQ`, `n_paths=200_000`,
    `seed=7`, `backend="cpu"`, override puntual de `pricing=`/`execution=`): 8 greeks de
    primer orden, valores idénticos bit a bit al nativo manual (`model.spot =
    637.5071993587057`, `model.volatility = 37563.46781002449`, etc.), `skipped` idéntico
    (vacío). `include_second_order=True`: 4 gammas puras idénticas bit a bit
    (`model.spot = 18.485654371037285`, ...).
  - `hessian` (mismo caso GBM): 3 entradas (`spot-spot`, `volatility-volatility`,
    `spot-volatility`) idénticas bit a bit al nativo manual. `hessian` sobre
    `HullWhiteModelNpv` (IRS 5y, `HullWhite1F`, `n_paths=1000`, caso de la celda `hw_hessian`
    del notebook 04): **10** entradas (4 diagonales + 6 cruzadas, mismo número que documenta
    el propio notebook), idénticas bit a bit.
  - `hvp` (misma call GBM, `direction={"model.spot": 1.0, "model.volatility": 0.0}`):
    componentes idénticos bit a bit al nativo manual, y además coincide con la fila
    `spot`/`spot` de la Hessiana de arriba (`18.485654371037285 == 18.485654371037285`,
    tolerancia `1e-9` solo por robustez del test, en la práctica exactos) — mismo criterio
    de verificación cruzada que usa el propio notebook 04.
  - `simulate_paths` (GBM, `s0=100, sigma=0.22`, `n_paths=1000, n_steps=52, seed=1234`, caso
    del notebook 07): `times`/`paths` idénticos elemento a elemento (`np.array_equal`) al
    nativo manual, tanto con `pricing=` explícito como usando el `PricingContext` del
    constructor por defecto (sin pasar `pricing=`).
  - `calibrate("HullWhite1F", market, initial_guess)` (caso exacto de
    `test_calibration.py::test_calibrator_recovers_known_parameters_and_feeds_create_model`,
    `true_a=0.15, true_b=0.025`, estimación inicial deliberadamente lejos): `converged=True`
    en ambos caminos, `optimal_params` idénticos bit a bit al nativo manual
    (`create_calibrator("HullWhite1F").calibrate(...)`), recupera `a`/`b` con error
    `< 1e-4` frente a los valores verdaderos.
  - `list_models`/`list_products`/`list_measures`/`list_calibrators`: las cuatro devuelven
    listas no vacías e idénticas elemento a elemento (`==`, mismo orden) a las del
    `engine.Engine()` nativo.
  - `Portfolio`: `quantdesk.Portfolio is engine.Portfolio` → `True` (reexport directo, no una
    copia). `Portfolio().add(...)` × 2 + `.size()` → `2`; `Portfolio.price(["PV"], ...)`
    devuelve `list[BatchResult]` con `PV` idéntico al de `price_batch`/`price()` individual
    del mismo trade.
- **Nota para Fase 4/5/6:** firmas finales exactas de `quantdesk.Engine` (todas en
  `clients/python/src/quantdesk/engine.py`, con docstring propio cada una):
  `price_batch(trades, model, market, metrics, *, pricing=None, execution=None) -> list[BatchRow]`;
  `price_many(trades, model, market, metrics, *, pricing=None, execution=None) -> list[BatchRow]`;
  `price_grid(trades, models, markets, metrics, *, pricing=None, execution=None) -> list[GridRow]`;
  `all_greeks(trade, metric_name, model, market, *, metric_params=None, include_curve_buckets=False, include_second_order=False, pricing=None, execution=None) -> engine.GreeksReport`;
  `hessian(trade, metric_name, model, market, *, metric_params=None, risk_factors=None, pricing=None, execution=None) -> engine.HessianReport`;
  `hvp(trade, metric_name, model, market, direction, *, metric_params=None, pricing=None, execution=None) -> engine.HvpReport`;
  `simulate_paths(model, market, *, pricing=None) -> tuple[np.ndarray, np.ndarray]`;
  `calibrate(model_type, market, initial_guess) -> engine.CalibrationResult`;
  `list_models()/list_products()/list_measures()/list_calibrators() -> list[str]`.
  `all_greeks`/`hessian`/`hvp`/`simulate_paths` tienen `pricing=`/`execution=` que §3.2 no
  lista literalmente — ver "Discrepancias resueltas" arriba antes de escribir tests/ejemplos/
  notebooks que asuman la firma literal del documento de diseño. `BatchRow`/`GridRow` son
  `@dataclass(frozen=True, slots=True)` con exactamente los campos que expone el nativo
  (`trade_index`/`measures`; `GridRow` añade `model_index`/`market_index`), `measures` ya
  envuelto en `PriceResult`. `Portfolio` es literalmente `engine.Portfolio` (mismo objeto,
  `is`, no una subclase ni wrapper) — usarlo con objetos nativos (`engine.Product` de
  `create_product`, no `TradeSpec` tipado directamente: `Portfolio.add` espera
  `engine.Product`, sin traducción típed→nativo automática, fuera de alcance de esta fase
  según el plan).

### Fase 3 — Empaquetado y CI

- `pyproject.toml`: `wheel.packages = ["clients/python/src/quantdesk"]` (y el comentario que lo
  explica, línea 29-32).
- `.github/workflows/ci.yml`: pasos "Python engine_typed test (...)" y las rutas
  `clients/python/tests/test_engine_typed_*.py` (nombres de paso y de fichero, ver Fase 6).

**Criterio de aceptación.** `python -m pip install .` produce una wheel con `quantdesk` (no
`engine_typed`); CI verde con los nombres nuevos.

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- `pyproject.toml` usa `scikit-build-core` (`[tool.scikit-build]`), no hatch/setuptools, así
  que la sintaxis real coincidía con la asumida por el plan: `wheel.packages` es efectivamente
  una lista de rutas (no una tabla hatch). Cambiado `wheel.packages = ["clients/python/src/
  engine_typed"]` -> `wheel.packages = ["clients/python/src/quantdesk"]`, y reescrito el
  comentario explicativo (líneas 28-32, sin desplazamiento -- mismo número de líneas de
  comentario tras el cambio) para que cite
  `quantdesk` como nombre actual y `engine_typed` solo como origen histórico ("renombrado desde
  `engine_typed` en PLAN_API_REFACTOR.md Fase 0"). `[project].name` se deja intacto
  (`"engine-quant"`, Fase 10 según el propio plan) -- no tocado.
- `.github/workflows/ci.yml`: renombrados únicamente los `name:` de paso y las rutas `run:` que
  referenciaban `engine_typed` -- 4 pasos: "Install engine_typed runtime dependency (pydantic)"
  -> "Install quantdesk runtime dependency (pydantic)"; "Python engine_typed test
  (TradeSpec/IRSwap tipados)" -> "Python quantdesk test (...)" con
  `test_engine_typed_trade.py` -> `test_quantdesk_trade.py`; ídem para el par
  context (`test_engine_typed_context.py` -> `test_quantdesk_context.py`), measure
  (`test_engine_typed_measure.py` -> `test_quantdesk_measure.py`) y payoff
  (`test_engine_typed_payoff.py` -> `test_quantdesk_payoff.py`). Cada `name:` y su `run:`
  correspondiente quedaron consistentes entre sí (ninguno mezcla `quantdesk` con una ruta
  `engine_typed` o viceversa). Nótese que `ci.yml` no invocaba en ningún paso
  `test_engine_typed_model.py` ni `test_engine_typed_greeks.py` (esos dos ficheros existen en
  `clients/python/tests/` pero no estaban -- y siguen sin estar -- cableados en el workflow); no
  se ha añadido ningún paso nuevo para ellos, fuera de alcance de esta fase.
  **Deliberadamente NO tocados** (alcance limitado a "nombres de paso y de fichero" según el
  propio texto de la Fase 3, y siguiendo el mismo criterio editorial que la Fase 7 aplica a citas
  históricas de otros PLAN_*.md): el comentario de la línea 148 ("PLAN_REAPI.md §6 Fase 1:
  engine_typed es un paquete Python puro...") y el de la línea 173 ("PLAN_PRODUCTS.md SS12 Fase 3
  (adelanta engine_typed.payoff de Fase 10)...") -- ambos citan el nombre que tenía el paquete en
  el momento en que se escribió esa nota histórica de otro PLAN_*.md, no son "nombres de paso ni
  rutas". `ci.yml` NO queda 100% libre de la cadena `engine_typed` tras esta fase (solo esos dos
  comentarios prosa); eso es intencional y coherente con el criterio de aceptación de Fase 7
  (`grep -rn engine_typed` limpio salvo PLAN_*.md históricos y sus citas cruzadas) -- si Fase 7 no
  cubre explícitamente estos dos comentarios de `ci.yml` en su lista, quedaría un resto marginal
  a decidir en esa fase (no bloquea el criterio de aceptación de esta Fase 3, que es sobre wheel
  y sobre los pasos/rutas, no sobre comentarios).
- Verificación de build/wheel (real, no simulada). El entorno de shell (Git Bash) no trae el
  entorno MSVC activo por defecto; se usó el script ya presente `build/vcenv.sh` (`source
  build/vcenv.sh`) más `export CMAKE_GENERATOR=Ninja` (sin esto CMake elegía el generador
  "Visual Studio 17 2022" en vez de Ninja, con el que el `build/` existente había sido
  configurado, y fallaba con "Does not match the generator used previously"). El árbol `build/`
  raíz ya existía de una sesión previa (con `engine.cp312-win_amd64.pyd` ya compilado), así que
  se reutilizó vía `-C build-dir=build` sobre `pip wheel . --no-deps -w <scratchpad>` (equivalente
  incremental a `pip install .`, sin tocar nada del módulo nativo C++/Rust -- esta fase es
  puramente de empaquetado Python). Primeros 3 intentos fallaron por corrupción/obsolescencia de
  cachés `.ninja_deps`/`.ninja_log` en subbuilds `FetchContent` preexistentes y no relacionados
  con este cambio (`build/_deps/corrosion-subbuild`, `build/_deps/simdjson-subbuild`,
  `build/_deps/nanobind-subbuild`, cada uno con un `.ninja_deps.recompact` residual de una
  recompactación interrumpida en una sesión anterior -- error `ninja: error: failed recompaction:
  No such file or directory`); se resolvió borrando esos 3 ficheros `.ninja_deps.recompact`
  sueltos (sin tocar ningún artefacto compilado, sin invalidar el `build/` raíz) y reintentando.
  El 4º intento completó con éxito sin recompilar Rust/C++/nanobind desde cero (reutilizó los
  artefactos ya presentes; solo la etapa de empaquetado Python puro se ejecutó de nuevo) y generó
  `engine_quant-0.0.0-cp312-cp312-win_amd64.whl` (1 542 632 bytes). Inspección completa con
  `python -m zipfile -l` del `.whl` resultante: contiene `engine.cp312-win_amd64.pyd` +
  `quantdesk/{__init__,context,engine,greeks,market,measure,model,payoff,trade}.py` +
  `engine_quant-0.0.0.dist-info/*` -- **cero** ocurrencias de `engine_typed` en el listado
  completo del wheel. Instalación real de ese wheel en `S:\Projects\engine_quant\venv`
  (`pip install --force-reinstall --no-deps`, sustituyendo la instalación previa obsoleta
  `engine-quant 0.10.0` que aún traía el paquete `engine_typed` de antes de la Fase 0) y
  verificación en caliente: `import quantdesk` funciona (`from quantdesk.engine import Engine,
  PriceResult` también), `import engine` (extensión nativa) funciona, `import engine_typed`
  lanza `ModuleNotFoundError` como se espera.
- Validación de sintaxis YAML: `python -c "import yaml; yaml.safe_load(open('.github/workflows/
  ci.yml'))"` -> `YAML OK`, sin excepciones.
- **Advertencia explícita para el orquestador.** Tras esta fase, `.github/workflows/ci.yml`
  referencia `clients/python/tests/test_quantdesk_{trade,context,measure,payoff}.py`, ficheros
  que **todavía no existen** en el árbol -- los ficheros reales siguen llamándose
  `test_engine_typed_{trade,context,measure,payoff}.py` (más `test_engine_typed_{model,greeks}.py`,
  no referenciados por CI) hasta que corra la Fase 6, que es quien los renombra. Esto es la
  secuencia que el propio plan ordena (Fase 3 antes que Fase 6) y es esperado -- pero significa
  que un run real de CI disparado entre esta fase y la Fase 6 fallaría (los `run:` de esos 4
  pasos apuntarían a rutas inexistentes). No se ha disparado ningún run de CI real en esta sesión.

### Fase 4 — Ejemplos (`clients/python/examples/`)

`price_flow.py`, `price_flow_typed.py`, `price_batch_flow.py`, `greeks_flow.py`: reescribir cada
uno sobre `quantdesk.Engine`. `price_flow.py` hoy contrasta deliberadamente la fachada dinámica
(`engine` crudo) con `engine_typed` en su docstring — pasa a contrastar la fachada dinámica con
`quantdesk` (mismo rol pedagógico, nuevo nombre). `clients/python/examples/README.md` se
actualiza en la misma fase (describe qué hace cada script).

**Criterio de aceptación.** Los cuatro scripts corren de punta a punta contra el build local con
el mismo output numérico que antes del refactor (se comparan antes/después, no solo "no
lanza excepción").

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- **Captura del "antes".** `engine_typed` ya no existe en el árbol (Fase 0), así que no se pudo
  ejecutar literalmente el script tal cual estaba en el commit padre de la Fase 0 (`930dda5~1`)
  sin antes reconstruir el paquete borrado. Se optó por el segundo camino que ofrecía la tarea
  ("reconstruir manualmente el mismo cálculo con `engine` + clases de `quantdesk`... comportamiento
  idéntico verificado en Fase 0"): para cada uno de los 4 scripts se copió el contenido EXACTO
  leído del árbol de trabajo actual (idéntico línea a línea al que había en `930dda5~1`, confirmado
  por lectura directa de ambos con `Read`/`git show` antes de tocar nada) a un script temporal en
  el scratchpad de la sesión, cambiando únicamente `import engine_typed as q` →
  `import quantdesk as q` (y `from engine_typed import greeks` → `from quantdesk import greeks` en
  `greeks_flow.py`) — ningún otro carácter tocado, ni siquiera comentarios. Los 4 scripts "antes"
  se ejecutaron con `venv\Scripts\python.exe` contra el `.pyd` nativo ya compilado (mismo venv que
  Fase 3 dejó con `quantdesk` instalado) y su stdout exacto se guardó en el scratchpad
  (`before_price_flow.out`, `before_price_flow_typed.out`, `before_price_batch_flow.out`,
  `before_greeks_flow.out`).
- **Reescritura de los 4 scripts** (todos en `clients/python/examples/`), sobre `quantdesk.Engine`
  tal como quedó definido en Fase 1/2 (constructor con `n_paths`/`n_steps`/`seed`/`backend` fijados
  una vez; `.price`/`.price_batch`/`.price_many`/`.price_grid`/`.all_greeks` reciben
  `trade`/`model`/`market` tipados directamente, sin `create_product`/`create_model`/
  `MarketSnapshot`/`PricingContext`/`ExecutionContext` manuales):
  - `price_flow.py`: `q.Engine(backend="auto", n_paths=5000, n_steps=208, seed=7)` sustituye las
    ~10 líneas de traducción manual; `qeng.price(trade, model, market, [...])` sustituye
    `eng.price(product, [...], eng_model, eng_market, eng_pricing, eng_execution)`. El rol
    pedagógico del docstring (contraste fachada dinámica vs fachada tipada) se mantiene, ahora
    citando `quantdesk` en vez de `engine_typed`; el propio código conserva un contraste real
    puntual (no solo de prosa): la única pieza de información que el flujo original imprimía y que
    `quantdesk.Engine` no expone (el backend `"auto"` ya resuelto a `"cpu"`/`"gpu"`) se obtiene
    con una llamada explícita a la fachada dinámica cruda (`engine.ExecutionContext({"backend":
    "auto", "precision": "FP64"}).backend`), documentada inline. 72 → 68 líneas.
  - `price_flow_typed.py`: mismo patrón; medidas tipadas (`q.PV()`, `q.DV01(bump=...)`,
    `q.DV01(bucketed=True)`) pasadas directamente a `qeng.price(...)` sin `.to_spec()` explícito
    (`quantdesk.Engine._to_native_metrics` ya hace `x.to_spec() if isinstance(x, Measure) else x`
    por elemento). Igual que `price_flow.py`, conserva la consulta puntual del backend resuelto vía
    `engine.ExecutionContext(...)` cruda (se había omitido en un primer borrador de esta fase y el
    diff antes/después lo detectó — ver "Discrepancias" más abajo). 82 → 72 líneas.
  - `price_batch_flow.py`: `qeng.price_batch(trades, model, market, metrics)`/`qeng.price_many(...)`/
    `qeng.price_grid(trades, models, markets, metrics)` sustituyen las versiones nativas con
    `eng_model`/`eng_market`/`eng_pricing`/`eng_execution` repetidos en cada llamada — el mismo
    `Engine` construido una vez cubre los tres niveles. 82 → 61 líneas.
  - `greeks_flow.py`: `qeng.price(...)`/`qeng.all_greeks(...)` sustituyen las llamadas nativas.
    **Hallazgo real, no cubierto por ninguna fase anterior** (buscado explícitamente por la
    instrucción de esta fase, "verifica si algún script usaba algo sin equivalente directo"):
    `quantdesk.model` solo tipa `HullWhite1F`/`HullWhite2F`/`GbmBasket` — no existe un `ModelSpec`
    para el modelo univariante `"GBM"` que usa este script (confirmado por `grep -n GBM` sobre
    `clients/python/src/quantdesk/model.py`: cero coincidencias de clase, solo una mención en el
    docstring de `GbmBasket`). `GbmBasket` NO es un sustituto válido (es multi-activo
    correlacionado, invocaría un modelo nativo distinto y cambiaría el resultado). Confirmado
    también que `quantdesk.Engine.price`/`.all_greeks` no aceptan un `engine.Product`/`engine.Model`
    nativo ya construido en su lugar (`_to_native_product`/`_to_native_model` llaman
    incondicionalmente a `trade.product_type`/`trade.to_params()`, que un objeto nativo no tiene —
    verificado con `hasattr(prod, 'product_type') == False` sobre un `engine.Product` real).
    Resuelto sin tocar `quantdesk/model.py` (fuera de alcance de esta fase, que es solo
    `examples/`): se define un `ModelSpec` mínimo local en el propio script (`class
    Gbm(q.ModelSpec)`, mismo patrón de 2 métodos — `model_type`/`to_params()` — que ya usan
    `HullWhite1F`/`HullWhite2F` en `quantdesk/model.py`), documentado con una nota explícita en el
    docstring del módulo para que Fase 5/6 no lo redescubran a ciegas. `trade` (`PayoffProduct`,
    `product_type = "Payoff"`) y `market` (`q.Market(pillars=[1.0], zero_rates=[0.05])`, con
    `hazard_rate`/`recovery_rate` por defecto a `0.0` — mismos valores que el `MarketSnapshot`
    nativo original, que tampoco los pasaba) sí tenían equivalente tipado directo, sin problema.
    71 → 90 líneas (más largo que el original, no más corto — la única excepción a "cada script
    queda considerablemente más corto": el coste de tipar `Gbm` localmente más el docstring que
    documenta el hallazgo pesa más que las líneas de traducción manual que desaparecen en un
    script ya corto).
  - `clients/python/examples/README.md`: descripción de los 4 scripts actualizada
    (`engine_typed` → `quantdesk`, `Engine.price`/`price_batch`/`price_many`/`price_grid`/
    `all_greeks` ahora citados como métodos de `q.Engine`), con una nota nueva en el párrafo
    introductorio sobre los dos puntos donde estos ejemplos siguen tocando la fachada dinámica
    cruda (backend resuelto en `price_flow.py`/`price_flow_typed.py`; modelo `GBM` sin tipar en
    `greeks_flow.py`) para que quien lea el README no los confunda con un patrón general.
- **Resultado exacto de la comparación antes/después** (`diff` byte a byte entre el stdout
  capturado del flujo nativo-manual reconstruido y el stdout real de cada script reescrito,
  ejecutados contra el mismo venv/`.pyd`, sin fijar ninguna tolerancia numérica — comparación de
  texto exacta):
  - `price_flow.py`: `diff` vacío — **idéntico byte a byte** (incluye `PV`, `DV01`,
    `UnilateralCVA`, el perfil `ExpectedExposure`/`PFE95` por fecha de reseteo, `PV (swap par) =
    0.000000`, y la línea `Backend resuelto: cpu`).
  - `price_flow_typed.py`: **idéntico byte a byte** tras corregir la omisión detectada (ver
    "Discrepancias" abajo) — incluye `PV`, `DV01 (1bp)`/`DV01 (2bp)`, el desglose `DV01 bucketed
    (por pillar)` con su suma, `UnilateralCVA`, `ExpectedExposure`/`PFE95`, y `Backend resuelto:
    cpu`.
  - `price_batch_flow.py`: **idéntico byte a byte** (3 filas de `price_batch`, 3 filas de
    `price_many`, 8 celdas de `price_grid`, incluidos los acentos de "homogéneo"/"heterogéneo" —
    ver nota sobre el primer intento de comparación más abajo).
  - `greeks_flow.py`: **idéntico byte a byte** (`PayoffPriceQ = 10,463.80`, `Delta (spot) =
    637.5072`, las 8 Greeks de primer orden con su `método`/`medida`, `skipped` vacío, y las 4
    Gammas puras con `include_second_order=True`).
- **Discrepancias detectadas por el propio proceso de comparación (no silenciadas, corregidas):**
  - Un primer borrador de `price_flow_typed.py` omitió por completo la línea `Backend resuelto:
    ...` que sí imprimía el script original (y que `price_flow.py` sí conservaba desde el primer
    borrador) — detectado por el `diff` antes/después (`1d0 < Backend resuelto: cpu`), no por
    inspección visual. Corregido añadiendo la misma consulta puntual a la fachada dinámica cruda
    que usa `price_flow.py`.
  - Un primer `diff` de `price_batch_flow.py` marcó como distintas las líneas de cabecera
    (`"lote homogéneo"`/`"lote heterogéneo"`) — investigado y confirmado que la causa era un
    defecto del propio script de reconstrucción "antes" (tecleado sin tilde por error, `"lote
    homogeneo"`), no del script reescrito (que sí conservaba la tilde original, confirmado leyendo
    el fichero reescrito con `grep`). Corregida la reconstrucción "antes" y repetida la
    comparación — `diff` vacío tras la corrección. Documentado aquí para que quede explícito que
    no era una regresión real, solo un defecto de la propia verificación.
  - El hallazgo de `greeks_flow.py`/modelo `GBM` sin `ModelSpec` tipado (arriba) es una
    discrepancia de alcance de `quantdesk`, no de comportamiento numérico — no afectó al resultado
    del `diff` (que dio idéntico), solo a cómo se tuvo que escribir el script.
- **Verificación de entorno.** `S:\Projects\engine_quant\venv` y `S:\Projects\engine_quant\build`
  seguían íntegros de la Fase 3 (no hizo falta recompilar nada — Fase 4 es Python puro):
  `venv\Scripts\python.exe -c "import quantdesk; import engine"` resuelve ambos módulos desde
  `site-packages` sin necesidad de `sys.path.insert` manual adicional (el wheel de Fase 3 ya
  instaló `quantdesk` como paquete top-level).
- **Nota para Fase 5/6.** Los notebooks y los tests nuevos de `quantdesk.Engine` probablemente
  repiten patrones de estos 4 scripts — dos cosas a tener en cuenta si aparecen casos similares:
  (1) si algún notebook consulta el backend `"auto"` ya resuelto, no hay atajo en
  `quantdesk.Engine` — hay que usar `engine.ExecutionContext({...}).backend` puntualmente, como
  aquí; (2) si algún notebook/test usa el modelo `GBM` univariante (no `GbmBasket`) con
  `quantdesk.Engine`, no existe `q.GBM`/`q.Gbm` en el paquete — hay que definir un `ModelSpec`
  local (2 campos: `model_type`/`to_params()`) igual que hace `greeks_flow.py`, o bien valorar si
  merece la pena añadir un `Gbm(ModelSpec)` real a `quantdesk/model.py` en una fase futura (fuera
  de alcance de Fase 4, que solo toca `examples/`) — el `notebook 07_montecarlo_paths_q_vs_p.ipynb`
  (citado en Fase 2 como usuario de `simulate_paths` con modelo `GBM`) es candidato directo a
  toparse con este mismo hueco.

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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- **Contexto de la sesión.** Esta fase se retomó tras un corte de sesión previo (límite de rate
  limit, sin commit ni reporte): al empezar, `01`...`09` ya estaban migrados a `import quantdesk
  as q` y ya re-ejecutados (`execution_count` secuencial 1..N, cero celdas `error`), y
  `quantdesk/model.py`/`__init__.py`/`engine.py` ya tenían `Gbm`/`GbmP`/`evaluate_scenario`
  añadidos — pero sin ningún reporte que confirmara que las CIFRAS numéricas (no solo "no lanza
  excepción") coincidían con el comportamiento pre-refactor. Esta sesión no confió en eso a
  ciegas: repitió la verificación numérica completa de `01`...`09` desde cero (comparación
  antes/después contra `dff8735`, el commit previo a esta fase) antes de dar nada por bueno, y
  completó lo que faltaba (`demo_registry.ipynb`, `notebooks/README.md`, esta misma sección).
- **Verificación numérica de `01`...`09` (script ad-hoc, no solo inspección visual).** Para cada
  notebook: `git show dff8735:clients/python/notebooks/<nombre>.ipynb` extraído a un fichero
  aparte, comparado celda a celda contra el árbol de trabajo actual con un extractor Python
  propio (`outputs` de cada celda de código: `stream`/`execute_result`/`display_data` con
  `text/plain`, más hash SHA-256 de cada `image/png` para comparar las figuras `matplotlib` byte
  a byte, no solo su presencia) — sin margen de tolerancia numérica, comparación de texto exacta:

  | Notebook | Cifras (`text/plain`) | Imágenes (`image/png`, hash) | Nota |
  | --- | --- | --- | --- |
  | `01_vanilla_options_black_scholes` | idénticas (`diff` vacío) | 4/4 idénticas bit a bit | — |
  | `02_exotic_and_path_dependent_options` | idénticas (`diff` vacío) | 8/8 idénticas bit a bit | — |
  | `03_bermudan_exercise` | idénticas (`diff` vacío) | 4/4 idénticas bit a bit | — |
  | `04_greeks_and_risk_surfaces` | idénticas | 5/5 idénticas bit a bit | único `diff`: un chunk de `stream stdout` vacío insertado en distinto punto (ver abajo) |
  | `05_physical_measure_forecasting` | idénticas (`diff` vacío) | 4/4 idénticas bit a bit | — |
  | `06_exposure_cva_portfolio` | idénticas (`diff` vacío) | 3/3 idénticas bit a bit | — |
  | `07_montecarlo_paths_q_vs_p` | idénticas | 2/2 idénticas bit a bit | mismo tipo de `diff` cosmético que `04` |
  | `08_multi_asset_options` | idénticas (`diff` vacío) | 6/6 idénticas bit a bit | — |
  | `09_option_strategies_and_greeks` | idénticas | 28/28 idénticas bit a bit | mismo tipo de `diff` cosmético que `04` |
  | `demo_registry` | idénticas (ver detalle abajo) | 1/1 idéntica bit a bit | migrado en esta sesión, ver más abajo |

  Los únicos tres `diff` no vacíos (`04`, `07`, `09`) se investigaron uno a uno, no se
  descartaron por presunción: en los tres casos el contenido impreso es exactamente el mismo
  (mismos números, mismas cifras, mismo orden) — lo único que cambia es dónde Jupyter corta un
  `print()` en un objeto `stream stdout` independiente frente a fusionarlo con el chunk
  siguiente/anterior (p.ej. en `04`, `[STREAM stdout] volatility ...` seguido de un
  `[STREAM stdout]` vacío antes de `rate ... dividend_yield ...`, frente a la versión "antes" que
  fusiona esa misma línea vacía dentro del chunk de `rho`/`dividend_yield`). Es una diferencia de
  cómo `ipykernel` trocea/empaqueta `stdout` entre ejecuciones (timing de flush), no una
  regresión de la migración — no hay ninguna cifra distinta en ningún punto de los tres notebooks
  afectados, confirmado leyendo el contexto completo de cada `diff`, no solo el recuento de
  líneas.
- **Migración de `demo_registry.ipynb` (única pieza que faltaba de verdad).** Este notebook NO
  es "solo calibración Hull-White" (una caracterización que quedó flotando de la sesión cortada,
  sin reporte que la respaldara, y que esta sesión no dio por buena sin comprobarlo): es la demo
  completa del registry (PLAN.md §5.4/§7.6) — 9 secciones que cubren `list_models`/
  `list_products`/`list_measures`, construcción de un `Model`/`Product` desde su nombre
  registrado, `Market`/`PricingContext`/`ExecutionContext`, perfil de exposición EE/PFE, CVA
  unilateral, calibración de **dos** modelos (`HullWhite1F` y `HullWhite2F`, contra mercados
  sintéticos `MarketSnapshot.synthetic_from_hull_white[_2f]`) y los tres niveles de lote
  (`price_batch`/`price_many`/`price_grid`) — y termina volviendo a listar el registry para
  demostrar que un modelo/medida nuevo aparecería sin tocar el notebook (§9, el propio punto
  central de PLAN.md §5.4).
  - **Decisión de diseño explícita, no cubierta literalmente por "mismo patrón que los otros
    9".** Las secciones 1-3 (listar el registry, construir un modelo/producto individual desde
    su nombre) se dejan deliberadamente sobre la fachada dinámica cruda (`eng =
    engine.Engine()`, `eng.create_model(...)`, `eng.create_product(...)`) en vez de
    `quantdesk.Engine`: es literalmente el objeto de esta demo concreta (a diferencia de los
    otros 9 notebooks, que usan la API para *valorar*, no para enseñar cómo se construye un
    `Model`/`Product` desde el registry) — documentado inline en una nota nueva en la celda
    markdown de la sección 2. A partir de la sección 4 (`Market`/pricing), el notebook migra a
    `quantdesk.Engine` como los otros 9: un único `qeng = q.Engine(backend="auto",
    n_paths=5_000, n_steps=208, seed=7)` sustituye la traducción manual de `Market`/
    `PricingContext`/`ExecutionContext` que antes se repetía; `qeng.price(...)` (perfil de
    exposición, CVA), `qeng.price_batch`/`price_many`/`price_grid` (sección 8) sustituyen las
    llamadas nativas con `pricing`/`execution` repetidos a mano en cada una.
  - **Hueco real encontrado, análogo al de `GBM` en `greeks_flow.py` (Fase 4).** La calibración
    (sección 7, celdas 18/19) sigue sobre la fachada nativa (`eng.create_calibrator(...)`,
    `.calibrate(market_nativo, ...)`) porque `market_1f`/`market_2f` se fabrican con
    `engine.MarketSnapshot.synthetic_from_hull_white[_2f](...)` -- un `@staticmethod` nativo sin
    equivalente tipado en `quantdesk.market.Market` (que solo modela pillars/zero_rates/hazard/
    recovery explícitos, no "deriva una curva desde HullWhite1F cerrado"). `Engine.calibrate`
    (§3.2) exige `market: Market` tipado y llama a `_to_native_market` (`market.to_params()`)
    por dentro -- pasarle un `engine.MarketSnapshot` ya nativo fallaría (no tiene
    `.to_params()`). No se ha tocado `quantdesk/market.py` para añadir este hueco (fuera de
    alcance de Fase 5, que es solo notebooks) -- documentado aquí y en el docstring de la celda
    para que quien revise Fase 6/7 no lo redescubra a ciegas.
  - **Verificación numérica exacta** (mismo método que `01`...`09`, comparado contra
    `git show dff8735:clients/python/notebooks/demo_registry.ipynb`): EE=`[0.0, 12862.617942...,
    13673.529752..., 11957.816098..., 7124.106240...]`, PFE(95%) y CVA=`503.64194077997536`
    idénticos bit a bit; `optimal_params` de ambas calibraciones (`HullWhite1F`: `a≈0.15`,
    `b≈0.025`; `HullWhite2F`: `a≈0.15`, `b≈0.25`, `eta=0.01`, `rho=-0.6`) idénticos bit a bit;
    las 8 celdas de `price_grid` y las 3 filas de `price_batch`/`price_many` (PV/CVA) idénticas
    bit a bit -- incluyendo `PV=948.4537...`/`CVA=626.7254...` del `trade_index=0`, mismos
    valores ya verificados de forma independiente en la Fase 1/2 de este plan para el mismo
    `IRSwap`/`HullWhite1F`/`Market`. La única figura (`matplotlib`, perfil EE/PFE) es idéntica
    bit a bit por hash SHA-256.
  - **Diferencias esperadas, no regresiones (documentadas, no silenciadas).** (1) La versión
    "antes" en `dff8735` tenía `execution_count` fuera de secuencia (`[12, 3, 4, 5, 6, 7, 8, 13,
    14, ..., 20]`) -- este notebook llevaba tiempo sin re-ejecutarse de punta a punta antes de
    esta fase (a diferencia de `01`...`09`, mantenidos al día por `PLAN_IMPROVE_NOTEBOOK2.md`);
    tras esta fase, `execution_count` es secuencial `1..15` sin huecos. (2) `list_models()`/
    `list_measures()` muestran un modelo (`GbmBasket`) y una medida (`PayoffUnilateralCvaQ`) que
    NO aparecían en la captura "antes" -- confirmado que esto es 100% independiente de
    `quantdesk` (`engine.Engine()` nativo puro, sin pasar por `quantdesk`, ya devuelve
    `GbmBasket`/`PayoffUnilateralCvaQ` hoy): son características añadidas al motor en fases de
    OTROS planes (`PLAN_IMPROVE_NOTEBOOK.md` Fase 3, etc.) posteriores a la última vez que
    `demo_registry.ipynb` se había ejecutado de verdad, no un efecto de esta migración -- de
    hecho es la propia sección 9 del notebook demostrándose a sí misma ("si alguien añade un
    modelo/medida nuevo, aparece aquí sin tocar el notebook"). (3) La celda de `!pip install
    pydantic matplotlib` imprime "already satisfied" en vez del log de descarga/instalación real
    -- differencia esperada de tener ya el entorno preparado de sesiones anteriores, no
    relacionada con el código del notebook. (4) La celda que antes mostraba el `repr` de
    `(MarketSnapshot, PricingContext, ExecutionContext)` nativos ahora muestra `(Market(...),
    <quantdesk.engine.Engine at 0x...>)` -- cambio de contenido esperado y deliberado (ya no se
    construyen esos tres objetos nativos a mano en esa celda), no una regresión.
- **`clients/python/notebooks/README.md`.** Tenía 2 referencias a `engine_typed` (línea 7,
  descripción general de la batería; línea 11, `engine_typed.greeks` en la entrada de `01`).
  Ambas actualizadas a `quantdesk`/`quantdesk.greeks` -- mismo criterio editorial que ya aplicó
  Fase 4 a `clients/python/examples/README.md`. La línea 7 conserva una mención explícita a
  `engine_typed` como el nombre que `quantdesk` sustituye (cita histórica de una frase, no
  documentación activa del patrón antiguo) -- coherente con el criterio de exclusión que fijará
  Fase 7 para citas históricas.
- **Revisión de coherencia de `01`...`09` (sin repetir la migración, ya hecha antes del corte de
  sesión -- solo auditoría).** Verificado con `grep`/introspección del código fuente de cada
  notebook, no solo relectura:
  - **Orden de argumentos.** Los 60 usos de `.price(` en los 9 notebooks respetan
    `price(trade, model, market, metrics)` (nunca el orden nativo) sin excepción -- confirmado
    listando cada llamada con su primer argumento.
  - **`pricing=`/`execution=`.** Ninguno de los 9 notebooks usaba `ExecutionContext` más de una
    vez en su versión pre-refactor (`grep` sobre `dff8735`: `ExecutionContext(` aparece
    exactamente 1 vez en cada uno) -- ningún notebook comparaba backends dentro de sí mismo, así
    que no hace falta `execution=` en ninguno tras la migración (confirmado: 0 usos), sin pérdida
    de capacidad. `pricing=` sí se usa donde el notebook pre-refactor variaba `PricingContext`
    más de una vez dentro del mismo script (`01`, `04`, `06`, `07`, `08`, `09` -- construyen un
    único `Engine`/`q.Engine` y pasan `PricingContext` puntuales por llamada en vez de
    reinstanciar el motor), verificado comparando el recuento de `PricingContext(` "antes" contra
    el recuento de `pricing=`/`q.PricingContext(` "después" notebook a notebook: ninguno perdió
    la capacidad de comparar dentro del mismo notebook (p.ej. `07` sigue comparando Q vs P con
    `pricing_p`/`pricing_hi`/`pricing_hit` puntuales sobre el mismo `eng`).
  - **`Gbm`/`GbmP`/`evaluate_scenario`.** Mismos nombres de campo en los 8 notebooks que usan
    `Gbm` (`s0`/`r`/`q`/`sigma`/`observable`, confirmado por introspección de cada llamada
    `q.Gbm(...)`) y en los 2 que usan `GbmP` (`s0`/`mu`/`sigma`/`observable`) -- coincide con los
    parámetros nativos de `"GBM"`/`"GBM_P"` (confirmado contra `create_model(...).to_params()`
    real, no solo contra el propio docstring de `quantdesk/model.py`). `evaluate_scenario` solo
    lo usa `09` (único notebook con pagos intrínsecos a graficar), patrón único, sin
    inconsistencia que comparar entre notebooks.
  - **Sin restos de construcción manual.** `grep` de `engine\.(MarketSnapshot|PricingContext|
    ExecutionContext)\(` sobre los 9 notebooks migrados: cero coincidencias -- ninguno quedó a
    medio migrar con una traducción típed→nativo residual.
  - No se encontró ninguna discrepancia que corregir en `01`...`09`: la migración hecha antes
    del corte de sesión era correcta en los nueve, tanto numérica (Paso 1) como
    estructuralmente (este paso).
- **Nota para Fase 6/7.** `Gbm`, `GbmP` (`clients/python/src/quantdesk/model.py`) y
  `Engine.evaluate_scenario` (`clients/python/src/quantdesk/engine.py`) son piezas nuevas de
  `quantdesk` sin cobertura de test dedicada todavía (no estaban contempladas en el diseño
  original de §3.2/§3.1 de este documento -- `Gbm`/`GbmP` llenan un hueco que Fase 4 ya había
  detectado y resuelto solo localmente en `greeks_flow.py`; `evaluate_scenario` es un hueco de
  cobertura real de Fase 2, ver el docstring del método). Fase 6 debería añadirles al menos un
  test cada uno (mismo criterio que el resto de `Engine`: comparación bit a bit contra el flujo
  nativo equivalente). El hueco de calibración con `MarketSnapshot.synthetic_from_hull_white[_2f]`
  sin equivalente tipado (`demo_registry.ipynb`, sección 7, arriba) queda documentado pero sin
  resolver -- decisión para una fase futura si se considera que merece un `Market` "sintético"
  tipado, fuera de alcance de Fase 5/6 de este plan.

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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- **Renombrados con `git mv`** (historia preservada, 6 `R` en `git status`):
  `test_engine_typed_{model,payoff,greeks,trade,measure,context}.py` ->
  `test_quantdesk_{model,payoff,greeks,trade,measure,context}.py` — coinciden EXACTAMENTE con
  los 4 nombres que `.github/workflows/ci.yml` ya esperaba desde la Fase 3
  (`test_quantdesk_{trade,context,measure,payoff}.py`), cerrando el desalineamiento que esa
  fase dejó documentado a propósito. En cada fichero: `import engine_typed as q` ->
  `import quantdesk as q` (y `from engine_typed.model import GbmBasket` ->
  `from quantdesk.model import GbmBasket`, `from engine_typed import greeks` ->
  `from quantdesk import greeks`), más docstring de cabecera, comentarios inline y el `print("OK:
  ...")` final de cada bloque `if __name__ == "__main__":` — mismos CASOS de test, ningún
  `assert`/fixture/dato numérico tocado.
- **`test_portfolio.py`/`test_market_products_realistic.py` editados in-place** (sin `git mv`,
  igual que pide el plan): `test_portfolio.py` en realidad NO importaba `engine_typed` (solo
  `import engine`, confirmado leyendo el fichero completo antes de tocarlo) — su única cita era
  de prosa en el docstring de cabecera ("Mismo criterio que `test_engine_typed_greeks.py`"),
  actualizada a `test_quantdesk_greeks.py`. `test_market_products_realistic.py` sí importaba
  `engine_typed as q` de verdad (`import quantdesk as q`), más una cita de comentario
  ("`engine_typed.payoff`" -> "`quantdesk.payoff`").
- **Búsqueda exhaustiva más allá de la lista literal del plan** (mismo patrón que ya repitieron
  Fase 0/3, señalado explícitamente en el encargo de esta fase): `grep -rn engine_typed
  clients/python/tests/` antes de tocar nada encontró **9** ficheros, dos más de los 8
  nombrados explícitamente por el plan (6 renombrados + `test_portfolio.py` +
  `test_market_products_realistic.py`) — el noveno, no listado en ningún inventario de ninguna
  fase anterior, es `clients/python/tests/test_engine_evaluate_scenario.py`
  (`import engine_typed.payoff as q`, línea 26). Editado in-place (mismo criterio que
  `test_market_products_realistic.py`: importa un submódulo del paquete, no el paquete raíz, así
  que la única línea a tocar era exactamente ese import) -> `import quantdesk.payoff as q`. Este
  fichero prueba `engine.Engine.evaluate_scenario` (el método NATIVO, no `quantdesk.Engine`) para
  las 14 estrategias de `09_option_strategies_and_greeks.ipynb` — se mantiene intacto en su
  alcance (sigue siendo la cobertura exhaustiva del método nativo), solo cambia de dónde importa
  `call_leg`/`put_leg`/`custom_strategy`. `test_engine_simulate_paths.py` (nombre gemelo, mismo
  patrón de test de un método nativo sin sufijo `test_engine_typed_`) NO citaba `engine_typed` en
  ningún punto (confirmado por grep antes de descartarlo) — no requería ningún cambio.
- **Test nuevo:** `clients/python/tests/test_quantdesk_engine.py` (22 tests), cubriendo lo que
  Fase 1/2/5 dejaron señalado como hueco de cobertura formal:
  - `test_price_matches_the_native_flow_bit_for_bit`: mismo caso del bloque "Después" de §1
    (`IRSwap` 5y, `HullWhite1F`, `Market` con crédito, 5 medidas incluyendo
    `ExpectedExposure`/`PFE95` vectoriales) — comparado dinámicamente contra un flujo nativo
    construido a mano en el propio test (no valores hardcodeados como golden data, mismo
    criterio que el resto del repositorio), bit a bit vía un helper `_assert_measure_result_equal`
    que compara `has_scalar`/`scalar`/`times`/`primary`/`secondary`/`bump_used`.
  - `test_price_result_dot_access_matches_bracket_access_and_is_the_same_object`: confirma
    `results.PV.scalar == results["PV"].scalar` Y `results.PV is results["PV"]` (mismo objeto,
    no solo valores iguales) para 3 medidas. `test_price_result_dot_access_raises_attribute_error
    _for_a_measure_not_requested`: cubre la rama de error de `PriceResult.__getattr__` (sin test
    previo).
  - `test_price_override_pricing_has_a_real_effect_but_does_not_mutate_the_engine`: usa
    `ExpectedExposure` (Monte Carlo real, depende de `n_paths`/`seed`) tal como pide
    explícitamente el encargo de esta fase y la propia nota de Fase 1 — NO `PV`/`DV01` de un IRS
    vainilla (deterministas: el test pasaría igual con una mutación real, por construcción). Tres
    aserciones encadenadas: (a) el override puntual SÍ cambia el resultado de esa llamada
    (perfil `[9625.35, ...]` con `n_paths=5000/seed=7` frente a un perfil distinto con
    `n_paths=200/seed=123`, confirmando que el override no es un no-op), (b) `qeng._pricing`
    sigue siendo literalmente `PricingContext(n_paths=5_000, n_steps=208, seed=7)` tras la
    llamada con override (lectura directa del atributo, no solo inferencia del resultado), (c)
    una llamada posterior SIN override reproduce el perfil baseline EXACTO (no un valor nuevo
    ligeramente distinto). `test_price_override_execution_does_not_mutate_the_engine_for_
    subsequent_calls`: mismo patrón (b) aplicado a `execution=`/`self._execution.backend`.
  - `test_price_batch_matches_the_native_price_batch` / `test_price_many_matches_the_native_
    price_many_on_heterogeneous_trades` / `test_price_grid_matches_the_native_price_grid`:
    comparación fila a fila / celda a celda contra el nativo. **Hallazgo real durante la
    escritura del test** (no un bug — comportamiento correcto y ya documentado, pero mi primer
    borrador lo pasó por alto): un primer intento de `price_many` heterogéneo usó
    `IRSwap.par(...)` para uno de los tres trades y `Engine.price_many` lo rechazó con
    `ValueError: los trades del lote deben traer fixed_rate explícito (use_par_rate no soportado
    en lote)` — comportamiento correcto del binding nativo (ya documentado en el propio
    docstring de `Engine.price_batch` en `quantdesk/engine.py`, "cada IRSwap debe traer
    fixed_rate explícito... el lote nativo no lo soporta"), no una regresión de `quantdesk`.
    Corregido el fixture del test (fixed_rate explícito en los tres trades, heterogeneidad real
    vía calendario distinto en vez de vía `IRSwap.par`), sin tocar ninguna implementación.
  - `test_all_greeks_matches_the_native_all_greeks` / `test_hessian_matches_the_native_hessian` /
    `test_hvp_matches_the_native_hvp`: call GBM ATM (mismo caso que
    `test_quantdesk_greeks.py::_gbm_call_fixture`, ahora construido también con el `Gbm` tipado
    nuevo de Fase 5 en el lado `quantdesk.Engine`), comparación bit a bit de
    `risk_factor`/`value`/`measure`/`order` (greeks), `(factor_i, factor_j)` -> `value`
    (hessian), `factor` -> `value` (hvp), y `skipped` en los tres.
  - `test_simulate_paths_matches_the_native_simulate_paths`: `times`/`paths` vía
    `np.array_equal`. `test_simulate_paths_pointwise_pricing_override_does_not_mutate_the_engine`:
    mismo patrón de no-mutación que `price(...)` pero aplicado a `simulate_paths` (que según
    Fase 2 también soporta `pricing=` puntual pese a no estar en la firma literal de §3.2) —
    override con `seed` distinto cambia las rutas simuladas, `self._pricing` no muta, llamada
    posterior sin override reproduce las rutas baseline exactas (`np.array_equal`).
  - `test_calibrate_matches_the_native_create_calibrator_and_calibrate`: mismo mercado/estimación
    inicial por los dos caminos (`quantdesk.Engine.calibrate` vs
    `eng.create_calibrator(...).calibrate(...)` nativo manual), `converged`/`optimal_params`
    idénticos. `test_list_methods_match_the_native_engine_registry`: las 4 listas
    (`list_models`/`list_products`/`list_measures`/`list_calibrators`) idénticas elemento a
    elemento Y no vacías (evita el falso positivo de "dos listas vacías son iguales").
    `test_portfolio_is_the_native_portfolio_reexported_directly`: `q.Portfolio is
    engine.Portfolio` (identidad, no solo mismo comportamiento) — cobertura que faltaba desde
    `quantdesk` (ya existía indirectamente vía `engine.Portfolio` en `test_portfolio.py`, pero
    nunca se había confirmado el reexport en sí desde el paquete tipado).
  - `test_gbm_to_params_matches_the_native_gbm_params_and_feeds_the_real_engine` /
    `test_gbm_p_to_params_matches_the_native_gbm_p_params_and_feeds_the_real_engine`: `to_params()`
    exacto más `eng.create_model(model.model_type, model.to_params()).type_name` correcto (`"GBM"`
    / `"GBM_P"`) — hueco de Fase 5 señalado explícitamente ("`Gbm`/`GbmP`... sin cobertura de test
    dedicada todavía"). `test_engine_price_with_gbm_matches_the_native_flow_bit_for_bit` /
    `test_engine_price_with_gbm_p_matches_the_native_flow_bit_for_bit`: además del `to_params()`
    aislado, confirma que `quantdesk.Engine.price(...)` con un `Gbm`/`GbmP` como `model` produce
    el mismo resultado que el flujo nativo (`PayoffPriceQ`/`PayoffForecastP` respectivamente),
    cerrando el ciclo completo trade+modelo+mercado tipados -> `Engine.price`.
  - `test_engine_evaluate_scenario_matches_the_native_evaluate_scenario` /
    `test_engine_evaluate_scenario_rejects_a_missing_observable_like_the_native_method`: hueco de
    Fase 2/5 señalado explícitamente ("`evaluate_scenario` es un hueco de cobertura real de
    Fase 2"). No repite la verificación exhaustiva de las 14 estrategias del notebook 09 (eso ya
    lo cubre `test_engine_evaluate_scenario.py` contra el método NATIVO) — aquí solo se confirma
    que el envoltorio tipado `quantdesk.Engine.evaluate_scenario` traduce `trade -> producto` y
    delega idénticamente al nativo (una estrategia butterfly, 5 escenarios de spot, ledger
    idéntico elemento a elemento) y que propaga la misma excepción (`"fixing ausente"`) cuando
    falta un observable requerido.
- **Ningún hallazgo de esta fase reveló un bug real** en la implementación de Fase 1/2/5 — el
  único comportamiento inesperado durante la escritura de tests (`price_many` rechazando
  `IRSwap.par(...)`) es una restricción ya documentada del binding nativo, no un defecto de
  `quantdesk`; se corrigió el test, no la implementación.
- **`abi_dll_path`: dos tests preexistentes rotos, NO causados por esta fase, fuera de alcance.**
  `test_greeks_fixtures_cross_layer.py::test_all_greeks_matches_between_nanobind_and_c_abi_for_
  the_call_fixture` y `test_payoff_fixtures_cross_layer.py::test_all_fixtures_match_between_
  nanobind_and_c_abi` fallan en el paso de `setup` con `fixture 'abi_dll_path' not found` —
  confirmado que ningún `conftest.py` en el repo (no existe ninguno bajo `clients/python/tests/`)
  ni ningún `pytest_addoption` en esos dos ficheros provee esa fixture; tampoco hay ningún
  `--abi-dll-path` cableado en `.github/workflows/ci.yml` (que invoca cada fichero de test como
  script `python archivo.py`, nunca como colección `pytest` de todo el directorio) ni en ningún
  `CTestTestfile.cmake`. Confirmado con `git status`/`git log` que ninguno de los dos ficheros
  fue tocado en esta sesión ni en las dos anteriores (`a90e9256`, 2026-09-15, dos días antes de
  esta sesión) — son ficheros huérfanos preexistentes, no una regresión de Fase 6. No se han
  "arreglado" (añadir la fixture/opción que falta es un cambio de infraestructura de test fuera
  del alcance de "renombrar tests y añadir cobertura de `quantdesk.Engine`") — se documentan aquí
  para que el orquestador decida si merece un hallazgo/fase propia.
- **Build C++ (`cmake --build build`): no se pudo re-ejecutar, hallazgo de entorno preexistente,
  no causado por esta fase.** `build/CMakeCache.txt` referencia un `cmake.exe` dentro de un
  directorio temporal de aislamiento de build de `pip` de una sesión anterior
  (`C:\Users\...\Temp\pip-build-env-2o8galto\...\cmake.exe`, usado por la Fase 3 al construir la
  wheel con `pip wheel . --no-deps`) que ya no existe en el sistema -- cualquier
  `cmake --build build` que necesite regenerar `build.ninja` falla con `CreateProcess failed: The
  system cannot find the file specified`. Confirmado que esto es un artefacto de entorno, no de
  código: `build/CMakeCache.txt` no fue tocado por ninguna fase de este plan, y esta fase no toca
  `cpp/`/`rust/`/CMake en absoluto. No se ha intentado una reconfiguración completa desde cero
  (fuera de alcance de una fase puramente Python -- coste/riesgo de una recompilación completa de
  C++/Rust no está justificado solo para "confirmar" algo que la Fase 3/4/5 ya construyeron y que
  el propio `.pyd` instalado en `venv` demuestra que sigue íntegro, ver abajo). También se
  encontró, al intentar este build, que el `ninja` de `/e/dev/sandbox/bin` (primero en `PATH` en
  este entorno de sesión) es la versión 1.4.0, incompatible con `ninja_required_version` (>= 1.5)
  del `build.ninja` ya generado por una sesión anterior con un ninja más nuevo -- resuelto
  anteponiendo al `PATH` el ninja de Visual Studio Build Tools (`.../Common7/IDE/
  CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`, versión 1.12.1), pero esto no fue suficiente
  para superar el problema de `CMakeCache.txt` de arriba. Ninguno de los dos hallazgos (ninja
  desactualizado en `PATH`, `CMakeCache.txt` apuntando a un `cmake.exe` efímero) es una
  regresión de esta fase -- ambos preexistían a esta sesión.
- **Confirmación indirecta de que el `.pyd` nativo sigue íntegro**, sin necesidad de recompilar:
  `venv\Scripts\python.exe -c "import quantdesk, engine"` resuelve ambos módulos sin error
  (`engine.cp312-win_amd64.pyd` ya compilado en `venv\Lib\site-packages`), y los 212 tests que sí
  corrieron (ver abajo) ejercitan ese mismo `.pyd` de forma extensiva (`Engine.price`/
  `price_batch`/`all_greeks`/`hessian`/`hvp`/`simulate_paths`/`calibrate`/Portfolio, todos contra
  el binding nanobind real) sin ningún fallo atribuible al binding nativo.
- **`ctest`: confirmado por inspección, no asumido, que los tests Python NO están cableados en
  ctest en absoluto** -- `build/clients/python/CTestTestfile.cmake` solo referencia el
  subdirectorio `nanobind-build` (tests internos de la librería `nanobind`, no de este
  repositorio), sin ningún `add_test` para `pytest`/los scripts de `clients/python/tests/`. Esto
  confirma la sospecha que el propio encargo de esta fase ya adelantaba ("probablemente `ctest`
  aquí se refiere a que la suite completa... siga en verde, no que haya tests C++ que citen
  `engine_typed`") -- no hay ningún `ctest` de Python que ejecutar; el equivalente funcional real
  de "ctest en verde" para esta fase es la suite `pytest` completa (ver abajo). No se ha
  ejecutado `cargo test -p engine-core --release` (ningún fichero Rust tocado por esta fase, ni
  motivo para pensar que esté afectado -- confirmado por `git status` que ningún fichero bajo
  `rust/` cambió en esta sesión).
- **Resultado EXACTO de `venv\Scripts\python.exe -m pytest clients/python/tests/` (suite
  completa, tras limpiar `__pycache__`):** `212 passed, 2 errors in 16.01s` -- los 2 errores son
  los dos tests huérfanos de `abi_dll_path` documentados arriba, preexistentes y fuera de
  alcance. **0 fallos** (`FAILED`) en toda la suite. Antes de esta fase (baseline, mismo comando,
  sobre el árbol tal como lo dejó la Fase 5): `Interrupted: 8 errors during collection` (los 8
  ficheros que citaban `engine_typed`, confirmado con la traza completa antes de tocar nada).
- **`grep -rn "engine_typed" clients/python/tests/ --include="*.py"` final: VACÍO** (`exit=1`,
  sin coincidencias) -- confirmado explícitamente tras todos los cambios de esta fase, incluido
  el noveno fichero no listado por el plan (`test_engine_evaluate_scenario.py`). No quedó ninguna
  cita histórica ambigua en `tests/` que decidir caso por caso (a diferencia de `PLAN_*.md`): todo
  lo encontrado por el grep exhaustivo se cambió 1:1 a `quantdesk`.
- **Nota para Fase 7 (documentación).** Un grep repo-wide (`grep -rln engine_typed`, excluyendo
  `PLAN_*.md`) tras esta fase sigue devolviendo, además de los ya conocidos y documentados en
  Fase 0/3 (`clients/python/src/engine_py_ext.cpp`, 5 citas C++; `.github/workflows/ci.yml`, 2
  comentarios de prosa citando otros `PLAN_*.md`), exactamente los ficheros que la propia Fase 7
  ya lista como su alcance (`README.md`, `clients/python/README_PYPI.md`,
  `docs/schema/engine.payoff/cookbook.md`, `clients/excel/README.md`,
  `clients/excel/tests/test_xloper.cpp`, `.claude/skills/execute-plan/SKILL.md`,
  `clients/python/notebooks/README.md`) más UNA cita adicional no nombrada explícitamente por
  ningún inventario anterior: `clients/python/examples/price_flow.py` línea 7 ("...que es lo que
  hacía el flujo anterior sobre `engine_typed`"), una nota histórica de una frase con el mismo
  criterio editorial que ya aplican `notebooks/README.md` línea 7 y los comentarios de `ci.yml`
  (cita el nombre que tenía el paquete ANTES del refactor, no documentación activa del patrón
  antiguo) -- no se ha tocado en esta fase (fuera de alcance, `examples/` es Fase 4, ya cerrada) y
  se deja como dato para que Fase 7 decida si lo incluye explícitamente en su propio inventario o
  lo trata igual que las demás citas históricas ya aceptadas.

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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- **Ficheros actualizados (inventario original de la fase, 6 ficheros):**
  - `README.md`: sección "Quick start with Python" (líneas 83-118 tras el cambio) sustituida
    por el bloque "Después" de §1 literal (mismos nombres de variable: `model`, `trade`,
    `market`, `engine = Engine(...)`, `metrics`, `results.PV.scalar`). "Dynamic dict facade"
    reencuadrada con la frase exacta del plan, traducida al inglés del resto del README (todo el
    documento está en inglés, la frase del plan está en español): *"`engine` (the compiled
    extension) is the low-level facade that `quantdesk` uses internally, and that Excel and the
    C ABI also use"*. Los demás bloques de código migrados a `quantdesk` (ver detalle por
    sección más abajo, incluye "Custom products from Python", "Calibration" y "Batch and grid
    calculation") — más 2 referencias de prosa fuera de bloques de código que el propio grep
    encontró y que no estaban en el inventario original de la fase pero son del mismo tipo
    (documentación viva, no cita histórica): la fila "Payoff authoring" de la tabla de
    capacidades (`engine_typed.payoff` → `quantdesk.payoff`) y la última viñeta de "Scope and
    known limitations" (`engine_typed` → `quantdesk`, "Per-measure configuration... only
    reachable from Python").
  - `clients/python/README_PYPI.md`: mismo tratamiento (documento en español) — bloque
    "Después" traducido al patrón de variables ya usado en el resto del fichero
    (`model`/`trade`/`market`/`engine`/`metrics`/`results`), reencuadre de "Fachada dinámica
    (dict crudo)" con la misma frase del plan (esta vez sin traducir, el documento ya está en
    español), y 2 citas de prosa fuera de código (`engine_typed` también cubre `Measure`...";
    "`engine_typed.IRSwap` la elimina...") actualizadas a `quantdesk`.
  - `docs/schema/engine.payoff/cookbook.md`: única ocurrencia, en la frase de cabecera del
    documento (línea 3-4), `engine_typed.payoff`/`import engine_typed as q` →
    `quantdesk.payoff`/`import quantdesk as q`. El resto del cookbook (todos los fragmentos de
    código) ya usaba `q.` sin `import` explícito por bloque — no había más ocurrencias que
    cambiar, confirmado por grep exhaustivo del fichero completo, no solo de la cabecera.
  - `clients/excel/README.md`: línea 87, cambio de una frase, sin más contenido alrededor
    tocado.
  - `clients/excel/tests/test_xloper.cpp`: línea 1155, comentario actualizado a
    `test_quantdesk_payoff.py` — confirmado que ese fichero existe de verdad en
    `clients/python/tests/` (renombrado en Fase 6).
  - `.claude/skills/execute-plan/SKILL.md`: línea 46, `engine_typed/*.py` → `quantdesk/*.py` en
    la lista de "archivos centrales del motor" que justifican ejecución secuencial de fases.
    Nota: este fichero está bajo `.claude/`, que en este repositorio está **sin trackear por
    git** (`git status` al inicio de la sesión ya mostraba `?? .claude/`) — el cambio se hizo
    igualmente (el propio encargo de la fase lo pide explícitamente, con nota de que el fichero
    en cuestión es el skill que orquesta esta misma ejecución) pero el commit de esta fase
    añade ese fichero individualmente (`git add .claude/skills/execute-plan/SKILL.md`), no todo
    `.claude/` (que también contiene `.claude/scheduled_tasks.lock`, ajeno a esta fase y a este
    plan, no tocado ni commiteado).
- **Migración de los bloques de código no listados explícitamente por el texto de la fase, pero
  cubiertos por "el resto de bloques de código del README que hoy usan `eng`/`q`... se migran al
  mismo patrón"** — cada uno verificado ejecutándolo literalmente contra
  `venv\Scripts\python.exe` (build local ya compilado, sin recompilar nada — Fase 7 es
  puramente documental):
  - **"Custom products from Python", bloque 1 (call europea)**: el original creaba el producto
    nativo solo para imprimir `product.type_name()` (`eng.create_product(...)` +
    `.type_name()`). `quantdesk.Engine` no expone `create_product` como método público (no está
    en su superficie, §3.2 — deliberadamente, la traducción típed→nativo es un detalle interno
    de `price`/`price_batch`/etc.), así que replicar el patrón "mismo `eng.create_product` pero
    con `q` = `quantdesk`" no era posible sin reintroducir la doble construcción que todo este
    plan elimina. Decisión tomada: usar `trade.product_type` directamente (atributo plano del
    objeto `PayoffProduct` ya construido, sin crear nada nativo) — comunica el mismo hecho
    ("todo contrato custom se registra como el tipo genérico `Payoff`") sin la construcción
    manual. **Hallazgo colateral real, no una invención de esta fase**: verificado por
    introspección (`venv\Scripts\python.exe`) que `product.type_name()` en el binding nativo
    actual **no es invocable como método** — `type_name` es un atributo string ya resuelto
    (`TypeError: 'str' object is not callable` al intentar `product.type_name()`); el README
    original ya tenía este bug de forma independiente al rename `engine_typed`→`quantdesk` (no
    se investigó cuándo se introdujo, fuera de alcance). Al sustituir ese bloque por
    `trade.product_type` el bug deja de estar presente en el README (no se dejó pasar un bloque
    nuevo con el mismo error).
  - **"Custom products from Python", bloque 2 (barrera)**: el original terminaba con
    `barrier_product = eng.create_product(barrier_trade.product_type, barrier_trade.to_params())`
    sin imprimir ni usar el resultado — solo demostraba que se podía crear. Se eliminó esa
    llamada (ya no hace falta demostrarlo por separado: la frase introductoria de la sección ya
    deja explícito que cualquier `PayoffProduct` se puede pasar directo a `Engine.price(...)`,
    y el bloque 1 ya lo hace con `trade.product_type`); el bloque ahora termina en la
    construcción del objeto típed, que es el punto real de la sección.
  - **"Custom products from Python", bloque 3 (envelope JSON)**: sin cambios de fondo
    (nunca citó `engine_typed`) salvo que, al haber eliminado `eng = engine.Engine()` de los
    bloques 1-2 de la misma sección, este bloque necesitaba su propia importación/instancia de
    `engine` para seguir siendo autocontenido — añadido `import engine` + `eng =
    engine.Engine()` al principio. Verificado que sigue ejecutando y creando el producto sin
    error.
  - **"Calibration"**: el original usaba `engine.MarketSnapshot.synthetic_from_hull_white(...)`
    (helper nativo, sin equivalente en `quantdesk` — no existe una función típed que genere una
    curva sintética a partir de parámetros Hull-White) seguido de `eng.create_calibrator(...)`
    + `calibrator.calibrate(...)` + `eng.create_model(...)`. `quantdesk.Engine.calibrate(
    model_type, market, initial_guess)` exige un `market` **típed** (`quantdesk.market.Market`,
    traducido internamente vía `market.to_params()`), no el `engine.MarketSnapshot` nativo que
    devuelve `synthetic_from_hull_white` (confirmado por introspección:
    `engine.MarketSnapshot` no tiene `to_params()` — `hasattr(ms, 'to_params')` → `False`).
    Decisión tomada: mantener la llamada nativa `synthetic_from_hull_white` (sigue siendo la
    única forma de generar esa curva sintética) y reconstruir un `quantdesk.Market` típed a
    partir de sus atributos ya resueltos (`synthetic.pillars`/`synthetic.zero_rates`, listas
    Python planas, confirmado por introspección — no son métodos, son propiedades ya
    materializadas) para poder seguir usando `quantdesk.Engine.calibrate(...)` el resto del
    flujo. Verificado end-to-end contra el venv: `fit.rmse ≈ 1.07e-14`, `fit.iterations = 9`,
    `fit.converged = True`, `calibrated_model` reconstruido con `q.HullWhite1F(**
    fit.optimal_params)` recupera `a=0.15`/`b=0.025` con error `< 1e-4` frente a los valores
    verdaderos usados para generar la curva sintética (mismo resultado cualitativo que ya
    verificó la Fase 2 para este mismo caso vía `test_calibration.py`).
  - **"Batch and grid calculation"**: el original construía cada trade como producto nativo
    antes de pasarlo a `eng.price_batch(...)` (`eng.create_product("IRSwap", q.IRSwap(...)
    .to_params())`). `quantdesk.Engine.price_batch(trades, model, market, metrics, ...)` acepta
    `trades: Sequence[TradeSpec]` **típed** directamente (§3.2/Fase 2) — la traducción a
    `engine.Product` ocurre dentro del método. Migrado a pasar `q.IRSwap(...)` sin
    `eng.create_product`, reutilizando `engine`/`model`/`market` definidos en la sección "Quick
    start with Python" anterior del mismo README (mismo patrón narrativo que ya usaba el
    original, que reutilizaba `eng_model`/`eng_market`/`eng_pricing`/`eng_execution` de esa
    misma sección temprana sin redefinirlos). Verificado contra el venv: `PV`/`UnilateralCVA`
    de las dos filas idénticos a los valores ya reportados por la Fase 2
    (`948.4537547220389`/`626.7254432766481` para el primer trade — el mismo trade/model/market
    que el bloque "Después" de §1, coincide bit a bit).
- **Bloque "Después" de §1 (el que se insertó literal en `README.md`/`README_PYPI.md`)
  re-ejecutado en esta sesión, no asumido de la Fase 1**: `venv\Scripts\python.exe`, mismo
  `trade`/`model`/`market`/`n_paths=5000`/`n_steps=208`/`seed=7` que la Fase 1 — resultado
  **`results.PV.scalar == 948.4537547220389`**, `results.DV01.scalar ==
  480.18800212936185`, `results.UnilateralCVA.scalar == 626.7254432766481` — idénticos bit a
  bit a los ya reportados en la Fase 1. También verificado `engine.list_models()`/
  `list_products()`/`list_measures()`/`list_calibrators()` (usados en la frase que sigue al
  bloque) devuelven listas no vacías sin error.
- **Los 4 hallazgos ya conocidos, resueltos:**
  1. `clients/python/src/engine_py_ext.cpp`: las 5 citas C++ (comentarios/docstrings, líneas
     56, 100, 177, 627, 1011 en el momento de esta sesión — coinciden con los números que
     estimaba la Fase 0) actualizadas a `quantdesk` 1:1, incluida
     `">>> from engine_typed import greeks\n"` → `">>> from quantdesk import greeks\n"` y el
     comentario que cita un futuro `engine_typed/portfolio.py` → `quantdesk/portfolio.py`.
     Ninguna toca la superficie pública del binding (`NB_MODULE(engine, m)`, nombres de
     método/clase expuestos) — solo texto de comentarios/docstrings, confirmado leyendo cada
     una de las 5 ubicaciones antes de editar.
  2. `.github/workflows/ci.yml`: **NO tocado**, tal y como razonaba ya la propia Fase 3 — los 2
     comentarios (línea 148, cita de `PLAN_REAPI.md §6 Fase 1`; línea 173, cita de
     `PLAN_PRODUCTS.md §12 Fase 3`) citan el nombre que tenía el paquete en el momento en que se
     escribió esa nota de otro `PLAN_*.md` histórico — mismo criterio editorial que el resto de
     citas cruzadas a `PLAN_*.md` ya excluidas explícitamente por esta fase. Confirmado que
     siguen siendo exactamente esos 2 (ninguno nuevo apareció).
  3. `clients/python/examples/price_flow.py` línea 7: **NO tocado**. La frase completa es
     "...que es lo que hacía el flujo anterior sobre `engine_typed`" — describe explícitamente
     el comportamiento del flujo ANTES del refactor (la traducción manual típed→nativo que
     `quantdesk.Engine` ahora hace por dentro), no una afirmación sobre el comportamiento
     actual con el nombre antiguo. Mismo criterio que las citas de `ci.yml` y de
     `clients/python/notebooks/README.md` línea 7 (ya decidido en Fase 5: "cita histórica de
     una frase, no documentación activa del patrón antiguo").
  4. `clients/python/notebooks/08_multi_asset_options.ipynb`: **2** ocurrencias encontradas por
     grep exhaustivo del fichero (no solo la ya señalada por Fase 5) — la ya conocida
     (celda de código, comentario `# clients/python/tests/test_engine_typed_greeks.py::
     test_delta_per_asset_of_a_basket_call_matches_manual_bump_and_reval` → cambiado a
     `test_quantdesk_greeks.py`, confirmado que esa función sigue existiendo con ese nombre
     exacto en `test_quantdesk_greeks.py` tras el rename de Fase 6) y una segunda, no señalada
     por ningún hallazgo anterior: una celda markdown que describe la arquitectura del AST/
     compilador de payoff ("`expression.hpp`/`docs/schema/engine.payoff/v1.schema.json`/
     `engine_typed.payoff` en C++/Python") — esta es documentación VIVA de la arquitectura
     actual (enumera los tres sitios donde vive el AST hoy: Rust, JSON schema, Python), no una
     cita histórica del "antes", así que se actualizó a `quantdesk.payoff` sin ambigüedad.
     Edición hecha con un script Python (reemplazo de texto sobre el JSON crudo del notebook,
     no con el editor de celda a celda) para preservar bit a bit el resto del fichero
     (saltos de línea CRLF dentro de las cadenas JSON, whitespace); verificado post-edición que
     el fichero sigue siendo JSON válido (`json.load` sin excepción) y que el `git diff` es
     mínimo (2 líneas, sin reformateo colateral).
- **Hallazgo nuevo del punto 8 (búsqueda final exhaustiva), no cubierto por ningún hallazgo
  anterior**: `pyproject.toml` tiene **2** ocurrencias (líneas 13 y 30) que no estaban en el
  inventario de ningún hallazgo previo bajo ese nombre exacto, pero SÍ estaban ya documentadas
  y decididas por la propia Fase 3 ("el comentario explicativo... para que cite `quantdesk` como
  nombre actual y `engine_typed` solo como origen histórico") — confirmado releyendo el estado
  de Fase 3: línea 13 cita literalmente `PLAN_REAPI.md §6 Fase 1` (mismo patrón que los 2
  comentarios de `ci.yml`, cita de otro `PLAN_*.md` histórico) y línea 30 dice explícitamente
  "renombrado desde `engine_typed` en PLAN_API_REFACTOR.md Fase 0" (cita histórica del propio
  rename, redactada así a propósito). **No tocado** — mismo criterio editorial que el resto de
  citas históricas ya excluidas, decisión ya tomada en Fase 3 y solo confirmada/ratificada aquí.
  Segundo hallazgo del punto 8: el propio `PLAN_API_REFACTOR.md` (este documento) contiene
  decenas de ocurrencias de `engine_typed` en la prosa retrospectiva de sus propias Fases 0-6 ya
  cerradas (y ahora también en esta misma sección de Fase 7, inevitablemente, al describir qué
  se hizo). Ninguna fase lo lista explícitamente en su "No se tocan" (esa lista nombra los OTROS
  ocho `PLAN_*.md`, no a sí mismo), pero por el mismo principio editorial que el propio
  documento aplica a esos ocho ("son actas históricas de fases ya cerradas... reescribir
  historia no aporta nada"), sus propias secciones "Estado verificado / decisiones tomadas" de
  fases ya cerradas son historia igual de legítima y no se tocan — decisión tomada aquí de forma
  explícita porque el propio documento no lo decía por sí mismo. El criterio de aceptación de
  esta fase, interpretado con este criterio, se cumple: ninguna ocurrencia de `engine_typed`
  fuera de (a) los 8 `PLAN_*.md` históricos explícitamente listados, (b) este mismo documento
  (`PLAN_API_REFACTOR.md`) en su propia prosa retrospectiva, (c) las citas cruzadas Rust/C++ a
  esos mismos `PLAN_*.md`, y (d) los 2 comentarios de `ci.yml` + 2 de `pyproject.toml` ya
  decididos en Fase 3 como citas históricas.
- **Resultado EXACTO del grep final** (`grep -rn "engine_typed"` sobre el árbol completo,
  excluyendo `.git/`; herramienta de búsqueda por ficheros, no por líneas, usada para el barrido
  — 21 ficheros con al menos una ocurrencia):
  - **Excluidos por nombre explícito de la fase (8):** `PLAN.md`, `PLAN_REAPI.md`,
    `PLAN_GREEKS.md`, `PLAN_BACKWARD.md`, `PLAN_PRODUCTS.md`, `PLAN_FXFORWARD.md`,
    `PLAN_IMPROVE_NOTEBOOK.md`, `PLAN_IMPROVE_NOTEBOOK2.md`.
  - **Excluidos por ser citas cruzadas Rust/C++ a esos mismos históricos (6):**
    `rust/crates/engine-core/src/payoff/mod.rs`, `.../compile.rs`, `.../ir.rs`,
    `.../basket_api.rs`, `rust/crates/engine-core/src/models/gbm_basket.rs`,
    `rust/crates/engine-ffi/src/lib.rs`, `cpp/engine/tests/payoff/
    test_payoff_fixtures_cross_layer.cpp` (7 ficheros en realidad, contando
    `gbm_basket.rs` que la Fase 0 ya había clasificado en este mismo grupo aunque el texto de
    Fase 7 no lo repita explícitamente).
  - **Excluido por ser este mismo documento en su prosa retrospectiva (1):**
    `PLAN_API_REFACTOR.md` (decisión tomada explícitamente en esta sesión, ver arriba).
  - **Excluidos por decisión ya tomada en Fase 3, ratificada aquí (2):** `.github/workflows/
    ci.yml` (2 comentarios), `pyproject.toml` (2 comentarios).
  - **Excluidos por decisión ya tomada en Fase 5/6, ratificada aquí (3):**
    `clients/python/notebooks/README.md` (línea 7, "`quantdesk` sustituye a `engine_typed`"),
    `clients/python/examples/README.md` (líneas 5-6, mismo patrón: "`quantdesk`... sustituye a
    `engine_typed`"), `clients/python/examples/price_flow.py` (línea 7, hallazgo conocido #3
    de esta fase, ver arriba).
  - **Total: 8 + 7 + 1 + 2 + 3 = 21 de los 21 ficheros del grep quedan justificados por alguna
    de las categorías anteriores.** No quedó ningún fichero sin clasificar tras el barrido
    completo.
  - `grep -rn "engine_typed" clients/python/src/quantdesk/__pycache__/*.pyc` encontró 3
    ficheros `.pyc` con la cadena embebida (bytecode compilado de una versión anterior del
    árbol, antes de esta fase) — **no forman parte del árbol git** (confirmado con
    `git check-ignore -v`, matchean la regla `__pycache__/` de `.gitignore` línea 12) y no
    afectan al criterio de aceptación, que es sobre el árbol versionado.
- **`ctest`/suite Python**: fase puramente documental, sin cambios de código Python/C++/Rust más
  allá de comentarios (`engine_py_ext.cpp`) y prosa (`.md`/`.ipynb`) — no se ha ejecutado
  `pytest`/`ctest` completos en esta sesión (no hay superficie ejecutable nueva que verificar
  más allá de los bloques de código del README, ya verificados uno a uno arriba contra el venv).

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

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).** Confirmado
con `git log --name-only 930dda5..HEAD -- clients/excel/` (rango que cubre todas las Fases 0-7 ya
commiteadas) que los únicos dos ficheros tocados en `clients/excel/` en todo el plan son
`clients/excel/README.md` (la frase de una línea, Fase 7) y `clients/excel/tests/test_xloper.cpp`
(el comentario que cita el nombre de fichero de test renombrado, Fase 7) — ningún fichero del XLL
en sí (código fuente, proyecto de build) fue modificado. La decisión de no tocar Excel, ya
justificada arriba, se mantiene sin ninguna excepción adicional descubierta durante la
implementación del resto de fases. No hay código que verificar en esta fase (es una decisión de
alcance, no una implementación) — la verificación consiste en confirmar que el resto del plan
respetó el límite que esta fase declara.

### Fase 9 — Verificación final

- `grep -rn "engine_typed"` limpio salvo lo listado en Fase 7.
- Build completo (`cmake --build build`) + suite Python completa + notebooks re-ejecutados.
- Repasar `clients/python/README_PYPI.md`/`README.md` renderizados (o al menos su Markdown) para
  confirmar que el flujo "Después" de §1 es lo primero que ve un lector nuevo.

**Estado verificado / decisiones tomadas (sesión de implementación de esta fase).**

- **Punto 1 — `grep -rn "engine_typed"`.** `git grep -l engine_typed` sobre el árbol completo
  (equivalente a un `grep -rn` restringido a ficheros versionados, mismo criterio que ya usó
  Fase 7) devuelve **exactamente los mismos 21 ficheros** que clasificó Fase 7, sin ningún
  fichero nuevo ni ninguno que haya dejado de aparecer: los 8 `PLAN_*.md` históricos, los 7
  ficheros de citas cruzadas Rust/C++ (`rust/crates/engine-core/src/payoff/{mod,compile,ir,
  basket_api}.rs`, `rust/crates/engine-core/src/models/gbm_basket.rs`,
  `rust/crates/engine-ffi/src/lib.rs`, `cpp/engine/tests/payoff/
  test_payoff_fixtures_cross_layer.cpp`), el propio `PLAN_API_REFACTOR.md` en su prosa
  retrospectiva, los 2 comentarios ya decididos en Fase 3 (`.github/workflows/ci.yml`,
  `pyproject.toml`) y los 3 ficheros de cita histórica de una frase decididos en Fase 5/6/7
  (`clients/python/notebooks/README.md`, `clients/python/examples/README.md`,
  `clients/python/examples/price_flow.py`). Confirmado además, por separado (`grep` normal, no
  `git grep`, porque no está trackeado): `.claude/skills/execute-plan/SKILL.md` sigue limpio
  (0 coincidencias, el cambio de Fase 7 se mantiene). Un `grep -rln` sin restringir a ficheros
  versionados sobre el árbol completo añade únicamente rutas bajo `build/`, `venv/`,
  `rust/target/`, `.pytest_cache/` y `clients/python/notebooks/.ipynb_checkpoints/` — las cinco
  están cubiertas por `.gitignore` (confirmado con `git check-ignore -v` sobre una muestra de cada
  una), no forman parte del árbol versionado y no son responsabilidad de este plan (artefactos de
  build/entorno, algunos con el símbolo `engine_typed` embebido en binarios de una compilación
  anterior a la Fase 0 que nunca se limpiaron). Ninguna sorpresa: el criterio de aceptación de
  esta fase se cumple sin cambios adicionales de código o documentación.
- **Punto 2 — Build completo (`cmake --build build`).** El aviso pendiente de Fase 6 **se
  resolvió, no se dejó como verificación alternativa.** Causa raíz confirmada:
  `build/CMakeCache.txt` (`CMAKE_COMMAND:INTERNAL`) apuntaba a
  `C:/Users/.../Temp/pip-build-env-2o8galto/normal/Lib/site-packages/cmake/data/bin/cmake.exe`,
  un `cmake.exe` efímero del entorno de aislamiento de build que `pip wheel` creó y borró durante
  la Fase 3 — coincide exactamente con lo que Fase 6 ya había diagnosticado sin arreglar. Además
  se confirmó que el `cmake`/`ninja` reales del sistema existen y son compatibles:
  `D:/CMake/bin/cmake.exe` (3.28.0-rc5) y el `ninja.exe` de VS Build Tools
  (`.../Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`, 1.12.1 — el mismo que ya
  había identificado Fase 6 como compatible, frente al 1.4.0 de `/e/dev/sandbox/bin` que es el
  que gana por defecto en el `PATH` de esta sesión). **Arreglo aplicado:** `source
  build/vcenv.sh` (entorno MSVC) + `PATH` con el ninja de VS Build Tools y el cmake de `D:/CMake`
  antepuestos + `CMAKE_GENERATOR=Ninja` + `cmake -S . -B build` — reconfiguración **incremental**
  sobre el `build/` ya existente (no se borró ni se recreó el directorio), completada en 22.6s sin
  invalidar ningún artefacto ya compilado ("Configuring done" / "Generating done" sin ningún
  mensaje de "does not match the generator used previously"). Confirmado por lectura directa de
  `build/CMakeCache.txt` tras la reconfiguración: `CMAKE_COMMAND:INTERNAL=D:/CMake/bin/cmake.exe`
  — ya no apunta a ningún directorio temporal de pip, el arreglo es persistente (queda escrito en
  la caché, no hace falta repetir la reconfiguración en builds futuros). **`cmake --build build
  --config Release` ejecutado de verdad a continuación, completo y verde: 62/62 pasos, `[exited
  with code 0]`**, incluyendo la reconstrucción real de `cargo rustc --release` para
  `engine-ffi` (2.70s, reutilizó el artefacto Rust ya compilado — no fue necesario recompilar
  Rust desde cero), el bridge `cxx`, `nanobind-static.lib`, `cpp/engine/engine.lib`,
  `engine_abi.dll`, los dos ejecutables de ejemplo del ABI (C++ y C), y el paso final
  `[62/62] Linking CXX shared module clients\python\engine.cp312-win_amd64.pyd` — el `.pyd` se
  relinkó de verdad en esta sesión, no fue una simulación ni una reutilización silenciosa de un
  artefacto viejo. Este es el primer `cmake --build build` confirmado en verde en toda la
  ejecución de este plan (Fase 0-8 nunca lo lograron, tal como Fase 6 dejó documentado
  explícitamente). No hizo falta ninguna recompilación completa desde cero de C++/Rust (coste que
  la propia tarea de esta fase advertía que podía no ser razonable) — el problema era puramente
  de qué `cmake.exe`/`ninja.exe` usar, no de artefactos corruptos o desactualizados.
- **Punto 3 — Suite Python.** `venv\Scripts\python.exe -m pytest clients/python/tests/`, tras
  limpiar `__pycache__`: **`212 passed, 2 errors in 15.61s`** — resultado exacto, sin cambios,
  frente al `212 passed, 2 errors` que dejó Fase 6. Los 2 errores son los mismos dos tests
  huérfanos ya documentados (`fixture 'abi_dll_path' not found` en
  `test_greeks_fixtures_cross_layer.py`/`test_payoff_fixtures_cross_layer.py`) — no se han
  tocado (siguen fuera de alcance de este plan, ver más abajo). **0 fallos** (`FAILED`) en toda
  la suite. La suite corrió contra el `.pyd` instalado en `venv\Lib\site-packages` (el de la
  Fase 3), no contra el recién recompilado en `build/` — ambos son binarios idénticos en
  comportamiento (mismo código fuente sin cambios en todo el plan; el `.pyd` de `build/` se
  ejercitó por separado vía los notebooks, que lo cargan directamente desde
  `../../../build/clients/python/engine.cp312-win_amd64.pyd`, ver punto 4) — no se ha
  reinstalado el wheel en el venv porque no hace falta (ningún cambio de código C++/Rust/Python
  del paquete en ninguna fase de este plan justificaría un reinstall, y esta fase tampoco
  introduce ninguno).
- **Punto 4 — Notebooks re-ejecutados.** Los 10 notebooks de `clients/python/notebooks/`
  (`01`...`09` + `demo_registry`) se re-ejecutaron de punta a punta con `jupyter nbconvert
  --to notebook --execute --inplace`, uno a uno: **los 10 terminaron con `exit=0`**, sin ninguna
  excepción ni celda fallida. Comparación `git diff` de cada notebook contra el `HEAD` que dejó
  Fase 5/7 (ningún notebook fue tocado por Fase 6/7/8, solo tests/documentación):
  - **8 de 10** (`01`-`08`) no tuvieron ningún cambio de contenido: el único `diff` presente era
    metadata de temporización de ejecución (`iopub.execute_input`/`status.busy`/`status.idle`/
    `shell.execute_reply`, timestamps ISO de esta sesión), confirmado filtrando el diff completo
    y comprobando que no queda ninguna línea `+`/`-` fuera de esas claves de metadata — **cero
    diferencias en outputs/valores numéricos/texto impreso**.
  - `09_option_strategies_and_greeks.ipynb`: una diferencia real de estructura JSON, investigada
    a fondo (no descartada a ciegas como "solo timestamp"): el kernel emitió el mismo texto de
    stdout (`"\n"` + `"intrinsic_value (Engine.evaluate_scenario) vs intrinsic_value_manual
    (NumPy): coinciden exactamente en las 14 estrategias, grid de 41 spots.\n"`) en **dos**
    mensajes `stream`/`stdout` separados en vez de uno solo combinado como antes — confirmado
    comparando el texto concatenado de ambas versiones byte a byte: **idéntico**. Es una
    diferencia de cómo Jupyter/ZMQ agrupó los flushes de `print(...)` entre dos ejecuciones del
    kernel (no determinista, timing de IO), no una regresión de contenido — mismo `assert
    np.array_equal(via_engine, via_numpy)` pasó en ambas ejecuciones (si hubiera fallado, la
    celda habría lanzado excepción y `nbconvert` habría terminado con código de error, cosa que
    no ocurrió).
  - `demo_registry.ipynb`: una diferencia real, también investigada: el `repr` de un objeto
    `quantdesk.engine.Engine` impreso en un output (`<quantdesk.engine.Engine at 0x2e0057907a0>`
    → `<quantdesk.engine.Engine at 0x2a70a802c60>`) — dirección de memoria del objeto Python,
    distinta por construcción entre dos procesos distintos del kernel, no un cambio de
    comportamiento.
  - **Ninguna de las dos discrepancias reales encontradas es una regresión** — ambas son ruido
    de re-ejecución esperado (timing de IO, direcciones de memoria), no atribuible a ningún
    cambio de Fases 6-8 (que no tocaron los notebooks en sí). Siguiendo el criterio del propio
    encargo de esta fase ("si coincide, no hace falta volver a commitear"), las 10 modificaciones
    de fichero se descartaron con `git checkout --` tras la comparación — el árbol de notebooks
    queda exactamente como lo dejó Fase 5, sin commit nuevo para ellos.
  - Nota metodológica: estos 10 notebooks importan el módulo `engine` **directamente desde
    `build/clients/python/engine.cp312-win_amd64.pyd`** (vía `sys.path`, confirmado leyendo la
    primera celda de `09_option_strategies_and_greeks.ipynb`: `"modulo engine importado desde:
    ...build/clients/python\engine.cp312-win_amd64.pyd"`), no desde el `.pyd` instalado en
    `venv\Lib\site-packages` que usa `pytest` (punto 3) — así que esta re-ejecución ejercita
    también, de forma independiente, el `.pyd` recién recompilado por el punto 2 de esta misma
    fase, con el mismo resultado numérico que antes de recompilar.
- **README.md / README_PYPI.md — orden editorial (punto 3 del encargo de esta fase).**
  `clients/python/README_PYPI.md` ya tenía el bloque "Después" de §1 como lo primero que aparece
  tras la introducción/`pip install` (confirmado leyendo el Markdown completo: título, párrafo de
  una frase, `pip install engine-quant`, y acto seguido el bloque de código — sin ninguna sección
  intermedia) — no requirió ningún cambio. `README.md` (el principal) **sí incumplía** el
  criterio: la sección "## Quick start with Python" (con el bloque "Después" dentro) aparecía
  tras **tres** secciones completas ("## Why Engine Quant?", "## Current capabilities",
  "## Architecture", ~61 líneas de contenido) en vez de justo después del bloque de
  introducción/badges/nota `[!IMPORTANT]` — no es "lo primero que ve un lector nuevo", está
  enterrado varias secciones más abajo, tal como el encargo de esta fase anticipaba como
  posibilidad a corregir. **Arreglo aplicado — movimiento de sección puro, sin reescritura de
  contenido:** la sección completa "## Quick start with Python" (con sus dos subsecciones
  "### Dynamic dict facade" y "### Custom products from Python", el bloque contiguo tal como lo
  dejó Fase 7) se movió a continuación inmediata del bloque de introducción/badges/nota
  `[!IMPORTANT]`, antes de "## Why Engine Quant?" — hecho con un script Python de corte/pegado
  por índice de línea (no con edición manual línea a línea, para evitar error humano en un bloque
  de 166 líneas), verificado después: `git diff --stat` muestra únicamente líneas movidas (mismo
  recuento de líneas añadidas/eliminadas, 61/61, cero reescritura de contenido dentro del bloque
  movido ni dentro de las secciones que cambiaron de posición relativa), sin líneas en blanco
  duplicadas ni huecos. Orden final confirmado por lectura directa: `# Engine Quant` → badges →
  nota `[!IMPORTANT]` → `## Quick start with Python` (bloque "Después" de §1 literal) →
  `### Dynamic dict facade` → `### Custom products from Python` → `## Why Engine Quant?` →
  `## Current capabilities` → `## Architecture` → `## Calibration` → resto sin cambios. Los
  anchors internos (`#dynamic-dict-facade`, citado desde "Custom products from Python") siguen
  siendo válidos tras el movimiento (mismo texto de cabecera, Markdown genera el mismo slug).
- **Criterio de aceptación GLOBAL del plan (§7), confirmado explícitamente:**
  1. `from quantdesk import Engine, HullWhite1F, IRSwap, Market` reproduce el bloque "Después"
     de §1 tal cual contra el build local, **re-verificado en esta sesión** (no solo heredado de
     Fase 1/7): ejecutado literalmente contra `venv\Scripts\python.exe` tras el `cmake --build
     build` de esta fase — `results.PV.scalar == 948.4537547220389`,
     `results.DV01.scalar == 480.18800212936185`,
     `results.UnilateralCVA.scalar == 626.7254432766481` — idénticos bit a bit a los valores ya
     reportados en Fase 1 y Fase 7. **Confirmado.**
  2. `engine_typed` no existe en el árbol salvo citas históricas explícitamente excluidas —
     confirmado en el punto 1 de esta fase, exactamente los mismos 21 ficheros que Fase 7 ya
     clasificó, ninguno nuevo. **Confirmado.**
  3. Toda la documentación viva (README, README_PYPI, notebooks, ejemplos) enseña `quantdesk`
     como único camino recomendado, con la fachada dinámica (`engine` crudo) documentada como
     alternativa de bajo nivel — contenido sin cambios desde Fase 7 (esta fase solo reordenó una
     sección de `README.md`, no reescribió ningún párrafo), confirmado además que el orden ahora
     sí presenta `quantdesk` como lo primero que ve un lector nuevo en `README.md`, y que ya lo
     era en `README_PYPI.md`. **Confirmado.**
  4. `pip install quantdesk` instala el paquete publicado por `release.yml` y `import quantdesk`
     funciona igual que el build local — **NO se puede confirmar en esta fase.** Depende
     enteramente de la Fase 10 (`quantdesk` no está publicado en PyPI todavía; `pyproject.toml`
     sigue declarando `[project].name = "engine-quant"`, no `quantdesk`; `release.yml` no tiene
     ningún job `publish-pypi`). Señalado aquí explícitamente como **pendiente de Fase 10**, no
     se da por bueno ni se aproxima.
- **Hallazgos que NO se consideraron regresiones que reabran una fase cerrada:** ninguno más allá
  de los ya documentados arriba (notebooks 09/demo_registry: ruido de re-ejecución no
  determinista, no contenido). No se encontró ningún hallazgo nuevo en esta fase que exigiera
  reabrir Fase 0-8 (más allá del propio aviso de build de Fase 6, ya resuelto arriba, y el orden
  de README.md, ya arreglado arriba — ninguno de los dos era un defecto de diseño de una fase
  cerrada, ambos eran huecos de verificación que esta misma fase existe para cerrar).
- **Ficheros tocados en esta sesión:** `README.md` (movimiento de sección, sin reescritura de
  contenido) y `build/CMakeCache.txt`/`build/build.ninja` (regenerados por la reconfiguración de
  CMake — no versionados, `build/` está en `.gitignore`, no se commitean). Los 10 notebooks se
  re-ejecutaron pero sus cambios se descartaron (`git checkout --`, ver punto 4) al no haber
  ninguna diferencia sustantiva que conservar.

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
