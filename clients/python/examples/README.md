# Examples

Scripts pequeños, ejecutables directamente, que no encajan en `tests/` (no son
verificaciones automatizadas) ni justifican un notebook (`notebooks/`, PLAN.md §7.7). Los
cuatro usan `quantdesk` (fachada tipada sobre `pydantic`, PLAN_API_REFACTOR.md -- sustituye a
`engine_typed`) -- es la forma recomendada de construir `Trade`/`Model`/`Market` desde Python y
de valorarlos con `quantdesk.Engine`, que resuelve `PricingContext`/`ExecutionContext` una sola
vez en su constructor en vez de reconstruirlos a mano en cada llamada. La fachada dinámica
(dict crudo, módulo `engine`) sigue existiendo por debajo -- la sigue usando Excel/C ABI, y
`quantdesk.Engine` la sigue usando internamente -- pero ya no es lo que muestran estos ejemplos,
salvo un par de líneas puntuales en `price_flow.py`/`price_flow_typed.py` (el backend "auto" ya
resuelto, que `quantdesk.Engine` no expone) y en `greeks_flow.py` (ver su nota de módulo: el
modelo "GBM" univariante no tiene todavía un `ModelSpec` tipado en `quantdesk.model`, a
diferencia de `HullWhite1F`/`HullWhite2F`/`GbmBasket`).

- `price_flow.py` — flujo completo de `ENGINE.PRICE` (PLAN.md §7.15): `qd.IRSwap`/
  `qd.HullWhite1F`/`qd.Market` construidos como objetos tipados, `qd.Engine(...)` fija
  `PricingContext`/`ExecutionContext` en el constructor, luego `qd.Engine.price(...)` calcula un
  lote de medidas (`PV`, `DV01`, `ExpectedExposure`, `PFE95`, `UnilateralCVA`) de una sola vez.
  Incluye también `qd.IRSwap.par(...)` (swap "a la par", PLAN_REAPI.md §3.2).
  `Engine(backend="auto", ...)` sustituye por completo el antiguo backend global de proceso
  (PLAN.md §7.12, ya retirado). Contrasta puntualmente con la fachada dinámica cruda (`engine`)
  para consultar el backend "auto" ya resuelto ("cpu"/"gpu"), el único dato que
  `quantdesk.Engine` no expone.
- `price_flow_typed.py` — showcase de medidas con configuración real (PLAN_REAPI.md §6
  Fases 3-5): `qd.DV01(bump=...)` y `qd.DV01(bucketed=True)` (un delta por pillar en vez de un
  escalar) sobre una curva de mercado multi-pillar real, no de un solo punto, todo sobre
  `qd.Engine.price(...)`.
- `price_batch_flow.py` — los tres niveles de la API de cálculo por lotes (PLAN.md §7.19) sobre
  `qd.Engine`: `price_batch` (lote homogéneo, mismo calendario), `price_many` (lote heterogéneo,
  agrupa internamente) y `price_grid` (explosión Trades × Models × Markets).
- `greeks_flow.py` — flujo completo de PLAN_GREEKS.md §9.1: autoría programática del payoff →
  `qd.Engine.price(...)` → `qd.Engine.all_greeks(...)`, sin código nuevo por factor de riesgo.
  Muestra tanto una Greek concreta (`quantdesk.greeks.delta(...)` vía `Engine.price`) como el
  barrido automático (`Engine.all_greeks`, con `include_second_order=True` para la Gamma pura).

Requieren haber compilado el proyecto con CMake antes (ver `PLAN.md` en la raíz):

```bash
python clients/python/examples/price_flow.py build/clients/python
python clients/python/examples/price_flow_typed.py build/clients/python
python clients/python/examples/price_batch_flow.py build/clients/python
python clients/python/examples/greeks_flow.py build/clients/python
```
