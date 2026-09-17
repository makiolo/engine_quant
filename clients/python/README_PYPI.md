# engine-quant

Cliente Python del motor de cálculo XVA (CVA/DVA/FVA/MVA/KVA): bindings nanobind sobre el
registry C++ del motor (modelos, productos y medidas), que a su vez delega en un core
numérico Rust (Burn) vía `cxx`. Ver el repositorio para el diseño completo (`PLAN.md`).

```bash
pip install engine-quant
```

`engine-quant` instala dos paquetes: la extensión nativa `engine` (compilada, sin
dependencias Python propias) y `quantdesk` (Python puro, depende de `pydantic>=2`) -- la
fachada tipada recomendada, con una única clase `Engine` que construye
`Trade`/`Model`/`Market` y los valora en una sola llamada:

```python
from quantdesk import Engine, HullWhite1F, IRSwap, Market

model = HullWhite1F(a=0.10, b=0.03, sigma=0.01, r0=0.02)
trade = IRSwap(notional=1_000_000.0, fixed_rate=0.02,
                payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0] * 5)
market = Market(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02],
                 hazard_rate=0.02, recovery_rate=0.40)

engine = Engine(backend="auto", n_paths=5_000, n_steps=208, seed=7)

metrics = ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"]
results = engine.price(trade, model, market, metrics)
print(results.UnilateralCVA.scalar)  # CVA unilateral
```

`quantdesk` también cubre `Measure` (`q.PV()`, `q.DV01(bump=0.0002)`,
`q.DV01(bucketed=True)` -- un delta por pillar de la curva en vez de un escalar --,
`q.ExposureProfile()`, `q.UnilateralCVA()`, vía `.to_spec()`) -- ver
`clients/python/examples/price_flow.py`/`price_flow_typed.py` para los flujos completos. `PV`/
`DV01` descuentan por la curva de `Market` observada (ya no dependen del modelo);
`ExpectedExposure`/`PFE95`/`UnilateralCVA` siguen dependiendo del modelo (Monte Carlo).
`Engine.price`/`price_batch`/`price_many`/`price_grid` aceptan tanto strings "pelados" como
tuplas `(nombre, params)`/medidas tipadas en la misma llamada.

## Fachada dinámica (dict crudo)

`engine` (la extensión compilada) es la fachada de bajo nivel que usa `quantdesk` por dentro,
y que también usan Excel y el ABI C: un dict/`Params` genérico, sin pasar por `pydantic`.
Sigue siendo válida desde Python -- útil para JSON crudo, scripting rápido, o cuando no hace
falta la validación de tipos:

```python
import engine

eng = engine.Engine()
model = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
product = eng.create_product("IRSwap", {
    "notional": 1_000_000.0,
    "payment_times": [1.0, 2.0, 3.0, 4.0, 5.0],
    "accruals": [1.0, 1.0, 1.0, 1.0, 1.0],
})
market = engine.MarketSnapshot(pillars=[1.0, 2.0], zero_rates=[0.02, 0.02], hazard_rate=0.02, recovery_rate=0.4)
pricing = engine.PricingContext({"pricing_date": 0.0, "n_paths": 5000.0, "n_steps": 208.0, "seed": 7.0})
execution = engine.ExecutionContext({"backend": "auto", "precision": "FP64"})

result = eng.price(product, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"], model, market, pricing, execution)
print(result["UnilateralCVA"].scalar)  # CVA unilateral
```

Aquí, omitir `fixed_rate` en el dict de `IRSwap` sigue significando "swap a la par"
(`use_par_rate`) -- esta es la única fachada donde esa ambigüedad se mantiene (ver
`PLAN_REAPI.md` §3.2); `quantdesk.IRSwap` la elimina (`fixed_rate` requerido, `IRSwap.par(...)`
explícito).

Solo hay ruedas para Windows (`win_amd64`): es la única plataforma que compila y testea este
proyecto hoy (ver `PLAN.md`, §7.4).
