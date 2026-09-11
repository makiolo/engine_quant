# engine-quant

Cliente Python del motor de cálculo XVA (CVA/DVA/FVA/MVA/KVA): bindings nanobind sobre el
registry C++ del motor (modelos, productos y medidas), que a su vez delega en un core
numérico Rust (Burn) vía `cxx`. Ver el repositorio para el diseño completo (`PLAN.md`).

```bash
pip install engine-quant
```

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

result = eng.calc(product, ["PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"], model, market, pricing, execution)
print(result["UnilateralCVA"].scalar)  # CVA unilateral
```

El paquete se llama `engine-quant`; el módulo importable de la extensión nativa es `engine`
(compilada, sin dependencias Python propias: Rust y el runtime de C++ quedan embebidos en la
propia rueda). Desde `engine_typed` (fachada tipada, ver más abajo) el paquete `engine-quant`
sí depende de `pydantic>=2` en tiempo de ejecución.

Solo hay ruedas para Windows (`win_amd64`): es la única plataforma que compila y testea este
proyecto hoy (ver `PLAN.md`, §7.4).

## `engine_typed`: fachada tipada (opcional)

`engine_typed` (paquete Python puro, aditivo) traduce objetos `pydantic` al mismo
`Params`/dict que ya consume `engine.Engine` -- no sustituye la fachada dinámica de arriba, es
una fachada más sobre el mismo registry (ver `PLAN_REAPI.md`).

```python
import engine, engine_typed as q

trade = q.IRSwap(
    notional=1_000_000.0, fixed_rate=0.02,
    payment_times=[1.0, 2.0, 3.0, 4.0, 5.0], accruals=[1.0, 1.0, 1.0, 1.0, 1.0],
)
# swap "a la par": q.IRSwap.par(notional=..., payment_times=..., accruals=...)
# -- omitir fixed_rate directamente es un ValidationError, no PAR (propuesta 2).

eng = engine.Engine()
product = eng.create_product(trade.product_type, trade.to_params())
```

`engine_typed` también cubre `Model` (`q.HullWhite1F`/`q.HullWhite2F`), `Market`,
`PricingContext`, `ExecutionContext` y `Measure` (`q.PV()`, `q.DV01(bump=0.0002)`,
`q.DV01(bucketed=True)` -- un delta por pillar de la curva en vez de un escalar --,
`q.ExposureProfile()`, `q.UnilateralCVA()`, vía `.to_spec()`) -- ver
`clients/python/examples/calc_flow_typed.py` para el flujo completo sin dicts crudos. `PV`/
`DV01` descuentan por la curva de `Market` observada (ya no dependen del modelo);
`ExpectedExposure`/`PFE95`/`UnilateralCVA` siguen dependiendo del modelo (Monte Carlo).
`Engine.calc`/`calc_batch`/`calc_many`/`calc_grid` aceptan tanto strings "pelados" como tuplas
`(nombre, params)` en la misma llamada.
