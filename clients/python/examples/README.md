# Examples

Scripts pequeños, ejecutables directamente, que no encajan en `tests/` (no son
verificaciones automatizadas) ni justifican un notebook (`notebooks/`, PLAN.md §7.7).

- `backend_selection.py` — selección de backend de cómputo (CPU/GPU) desde Python, PLAN.md
  §7.12: `with engine.backend("gpu"):` (gestor de contexto nativo) y su equivalente
  ilustrativo con `contextlib.contextmanager`.

Requieren haber compilado el proyecto con CMake antes (ver `PLAN.md` en la raíz):

```bash
python clients/python/examples/backend_selection.py build/clients/python
```
