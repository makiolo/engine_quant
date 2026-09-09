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
measure = eng.create_measure("UnilateralCVA")
result = measure.evaluate(model, product, {
    "monitoring_times": [0.0, 1.0, 2.0, 3.0],
    "n_paths": 5000.0,
    "seed": 13.0,
    "hazard_rate": 0.02,
    "recovery_rate": 0.4,
})
print(result.scalar)  # CVA unilateral
```

El paquete se llama `engine-quant` pero el módulo importable es `engine` (extensión nativa
compilada, sin dependencias Python en tiempo de ejecución: Rust y el runtime de C++ quedan
embebidos en la propia rueda).

Solo hay ruedas para Windows (`win_amd64`): es la única plataforma que compila y testea este
proyecto hoy (ver `PLAN.md`, §7.4).
