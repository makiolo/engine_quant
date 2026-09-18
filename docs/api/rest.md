# API REST v1 en Rust

## Estado

La vertical REST v1 está implementada como un binario Rust en `rust/crates/quant-api`.
El contrato público y sus esquemas viven en
[`openapi.v1.yaml`](openapi.v1.yaml); las pruebas del router y del SDK Python forman parte
de CI. La API es una interfaz remota del mismo dominio que consumen Python y Excel, no una
segunda implementación de valoración.

La vertical no se distribuye todavía en el instalador ni en las wheels publicadas. Para
ejecutarla desde el repositorio:

```bash
cargo run --manifest-path rust/Cargo.toml -p quant-api
```

El servidor escucha en `0.0.0.0:8080` por defecto. Los límites de workers, cola y memoria
se configuran con `QUANT_WORKER_COUNT`, `QUANT_QUEUE_CAPACITY`,
`QUANT_MEMORY_BUDGET_BYTES` y `QUANT_MAX_JOB_MEMORY_BYTES`.

## Arquitectura ejecutable

```text
Python (quantdesk.rest) ─┐
                         ├── HTTP/JSON ──► quant-api (Axum)
Excel / C++ / C ABI ─────┘                    │
                                             │ DTO + Problem Details
                                             ▼
                                      quant-domain
                                      QuantContext inmutable
                                             │
                                             ▼
                                      quant-engine
                           planner + registry + scheduler acotado
                                      │              │
                                      ▼              ▼
                              kernels Rust       providers legacy
                              CPU/SIMD/GPU       Rust/C++ vía adapters
```

`quant-domain` no conoce HTTP ni Tokio. `quant-engine` usa workers dedicados: Tokio coordina
la petición, pero el cálculo CPU no se ejecuta en un future. `quant-api` valida JSON,
aplica límites, traduce errores a `application/problem+json` y cancela cooperativamente
los chunks pendientes si se desconecta el cliente.

Excel sigue usando el camino nativo C++/C ABI porque un XLL no es un cliente HTTP en esta
versión. Esto mantiene una única semántica de dominio durante la migración; añadir un
producto o medida requiere actualizar también esa superficie nativa antes de publicarlo
como capacidad remota.

## Stateless y `QuantContext`

El servidor no crea sesiones ni depende de un registry de recursos entre requests. El
cliente conserva el snapshot completo y envía el contexto en cada operación:

```text
Context₀ + add_market   ──► Context₁ + MarketRef
Context₁ + add_model    ──► Context₂ + ModelRef
Context₂ + add_product  ──► Context₃ + ProductRef
Context₃ + add_portfolio ──► Context₄ + PortfolioRef
Context₄ + price/risk/xva ─► resultado
```

Cada mutación incrementa `revision`, conserva `parent_hash` y recalcula `context_hash`.
`ResourceRef` contiene `id`, `kind`, `version` y hash del spec. Los handles internos se
reconstruyen durante la request y una cache local solo puede ser una optimización:
perderla, reiniciar el proceso o cambiar de réplica no debe cambiar la semántica.

`operation_id` sirve para correlación e idempotencia lógica del cliente; no implica que el
servidor persista resultados. Las operaciones actuales son síncronas. No existe
`GET /jobs/{id}`, job store durable ni `continuation_token` reanudable.

## Operaciones v1

| Endpoint | Uso |
|---|---|
| `POST /v1/context:apply` | Aplica comandos tipados de market, model, product, portfolio, runs o remove y devuelve la nueva revisión. |
| `POST /v1/prices` | Pricing de uno o varios productos con medidas parametrizadas. |
| `POST /v1/portfolios:price` | Pricing batch de una cartera con resultados en orden y `PartialFailure` explícito. |
| `POST /v1/scenarios:run` | Reutiliza una cartera y ejecuta escenarios por chunks. |
| `POST /v1/risk:calculate` | Greeks densas con valoración base y bumps compartidos. |
| `POST /v1/xva:calculate` | EE/PFE/CVA y medidas XVA sobre el grafo de exposición Q/P. |
| `GET /health/live` y `GET /health/ready` | Liveness y readiness del proceso y del scheduler. |

Los aliases `/v1/risk` y `/v1/xva` se conservan por compatibilidad. Los errores tienen
forma RFC 9457-style con `code`, `trace_id` y, cuando aplica, `field_errors`.

## Flujo para nuevos objetos de primera clase

Un objeto nuevo no está terminado cuando solo existe en el kernel. Debe recorrer las mismas
fronteras:

1. **Dominio Rust:** añadir un spec declarativo/versionado, su ID/ref, validación y la
   capability/kernel que lo ejecuta. Mantener los datos serializables y sin handles físicos.
2. **Python:** exponer el constructor tipado y la fachada dinámica, más su serialización a
   spec. `quantdesk.rest` debe poder enviarlo mediante `context:apply` y referenciarlo en
   pricing, portfolio, risk o XVA.
3. **Excel:** añadir el constructor/handle, listado del registry y las UDFs o medidas que
   correspondan en el XLL. Excel no debe reimplementar la fórmula.
4. **REST:** incorporar el comando/spec al contrato OpenAPI y al `quant-domain`, mapearlo en
   `quant-api` y registrarlo en el planner. El request debe seguir siendo autocontenido.
5. **Paridad:** añadir un fixture JSON versionado y probar creación, hash, referencias,
   cálculo y errores en Rust, Python y Excel. Si existe un camino C ABI, incluirlo en la
   misma matriz.

Los specs REST admiten campos JSON abiertos para permitir evolución, pero esa tolerancia no
significa que un producto sea ejecutable: debe existir un kernel compatible registrado.
El contrato `engine.payoff/v1` es el patrón para productos declarativos compuestos; no se
deben introducir callbacks Python ni estado mutable en el hot path.

## Cliente Python

`quantdesk.rest.QuantRestClient` es un cliente puro de la biblioteca estándar. Mantiene
`QuantContext` localmente, ofrece `context_apply`, `add_market`, `add_model`, `add_product`,
`price`, `price_portfolio`, `run_scenarios`, `calculate_risk` y `calculate_xva`, y convierte
`application/problem+json` en `QuantRestError`. Véase
[`clients/python/README_REST.md`](../../clients/python/README_REST.md).

El SDK no introduce una sesión implícita ni una dependencia de `requests`/`httpx`. Un
transport inyectable permite probar el contrato sin levantar el servidor.

## Límites conocidos

- El límite de body por defecto es 1 MiB; los límites de productos, escenarios y memoria se
  validan antes de admitir trabajo.
- Los escenarios, risk y XVA devuelven JSON síncrono en esta fase; Arrow IPC y streaming
  quedan para una fase posterior.
- El servidor REST y el cliente Excel remoto todavía no forman parte de los artefactos
  publicados.
- La exposición XVA puede devolver referencias de artefacto; materializar cubos grandes no
  es obligatorio.
- REST comparte el dominio y contratos, pero no hace que un tipo nuevo aparezca
  automáticamente en todos los clientes: la checklist de primera clase anterior es parte
  del cambio.
