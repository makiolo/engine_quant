# Examples

Scripts pequeños, ejecutables directamente, que no encajan en `tests/` (no son
verificaciones automatizadas) ni justifican un notebook (`notebooks/`, PLAN.md §7.7). Los
tres usan `engine_typed` (fachada tipada sobre `pydantic`, PLAN_REAPI.md §6) -- es la forma
recomendada de construir `Trade`/`Model`/`Market`/`PricingContext`/`ExecutionContext` desde
Python; la fachada dinámica (dict crudo) sigue existiendo por debajo (`eng.create_product(name,
params_dict)`, la sigue usando Excel/C ABI) pero ya no es lo que muestran estos ejemplos.

- `price_flow.py` — flujo completo de `ENGINE.PRICE` (PLAN.md §7.15): `q.IRSwap`/
  `q.HullWhite1F`/`q.Market`/`q.PricingContext`/`q.ExecutionContext` construidos como objetos
  tipados, luego `Engine.price(...)` calculando un lote de medidas (`PV`, `DV01`,
  `ExpectedExposure`, `PFE95`, `UnilateralCVA`) de una sola vez. Incluye también
  `q.IRSwap.par(...)` (swap "a la par", PLAN_REAPI.md §3.2). `ExecutionContext(backend="auto")`
  sustituye por completo el antiguo backend global de proceso (PLAN.md §7.12, ya retirado).
- `price_flow_typed.py` — showcase de medidas con configuración real (PLAN_REAPI.md §6
  Fases 3-5): `q.DV01(bump=...)` y `q.DV01(bucketed=True)` (un delta por pillar en vez de un
  escalar) sobre una curva de mercado multi-pillar real, no de un solo punto.
- `price_batch_flow.py` — los tres niveles de la API de cálculo por lotes (PLAN.md §7.19):
  `price_batch` (lote homogéneo, mismo calendario), `price_many` (lote heterogéneo, agrupa
  internamente) y `price_grid` (explosión Trades × Models × Markets).

Requieren haber compilado el proyecto con CMake antes (ver `PLAN.md` en la raíz):

```bash
python clients/python/examples/price_flow.py build/clients/python
python clients/python/examples/price_flow_typed.py build/clients/python
python clients/python/examples/price_batch_flow.py build/clients/python
```
