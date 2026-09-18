# Portfolio y escenarios (Fase 4)

`Portfolio` es una lista declarativa de `Trade`: cada trade conserva su `TradeId`, referencia de
producto, cantidad y, desde esta fase, `NettingSetId` y `CollateralAgreementId`. Esos IDs no
activan todavía netting/XVA; permiten que el contrato no tenga que cambiar cuando llegue Risk.

`Engine::price_portfolio` agrupa por familia de producto y ejecuta cada grupo en batches limitados
por `EngineConfig::max_batch_items`. El resultado mantiene el orden de entrada para los trades
que se pudieron valorar y devuelve `PartialFailure` explícito para referencias o kernels no
compatibles. La cache del planner solo acelera: no es una fuente autoritativa y se puede evictar.

`Engine::run_scenarios` reutiliza el portfolio/modelo inmutable y procesa un `ScenarioSet` por
chunks. La salida puede ser grande, por lo que el endpoint `/v1/scenarios:run` limita el request y
mantiene la conexión durante la ejecución. En esta versión la respuesta es JSON síncrona; no hay
job store durable ni `GET /jobs`. `continuation_token` es `null`: ningún kernel incorporado es
reanudable. `ExecutionControl` se comprueba antes de cada chunk y Axum lo liga al lifetime de la
request mediante un guard: una desconexión cancela chunks pendientes, aunque un provider que ya
está dentro de una llamada puede terminarla cooperativamente. Repetir con el mismo `operation_id`
es idempotencia lógica del cliente, no almacenamiento de resultados en servidor.

Para reproducir una ejecución en otra réplica se envía el `QuantContext` completo. El hash del
contexto y los specs inline se vuelven a validar, de modo que un cache miss o un reinicio no cambia
la semántica.

El harness `benches/portfolio_scenarios.py` cubre 1/10/1000 y un tamaño grande configurable:

```text
python benches/portfolio_scenarios.py --trades 1,10,1000 --large 100000 --scenarios 1,10,1000
```

Mide agrupación, chunks, memoria aproximada y la comparación de crossings individuales frente a un
batch. No materializa una matriz de todos los escenarios durante la planificación.
