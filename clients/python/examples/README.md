# Examples

Scripts pequeños, ejecutables directamente, que no encajan en `tests/` (no son
verificaciones automatizadas) ni justifican un notebook (`notebooks/`, PLAN.md §7.7).

- `calc_flow.py` — flujo completo de `ENGINE.CALC` (PLAN.md §7.15): `Trade`/`Model`/`Market`/
  `PricingContext`/`ExecutionContext` construidos por separado, luego `Engine.calc(...)`
  calculando un lote de medidas (`PV`, `DV01`, `ExpectedExposure`, `PFE95`, `UnilateralCVA`)
  de una sola vez. `ExecutionContext({"backend": "auto", ...})` sustituye por completo el
  antiguo backend global de proceso (PLAN.md §7.12, ya retirado).
- `calc_batch_flow.py` — los tres niveles de la API de cálculo por lotes (PLAN.md §7.19):
  `calc_batch` (lote homogéneo, mismo calendario), `calc_many` (lote heterogéneo, agrupa
  internamente) y `calc_grid` (explosión Trades × Models × Markets).

Requieren haber compilado el proyecto con CMake antes (ver `PLAN.md` en la raíz):

```bash
python clients/python/examples/calc_flow.py build/clients/python
python clients/python/examples/calc_batch_flow.py build/clients/python
```
