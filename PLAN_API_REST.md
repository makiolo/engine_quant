# PLAN_API_REST.md — API Rust de alto rendimiento para Engine Quant

> Estado: vertical REST v1 implementada; este documento conserva el diseño, gates y roadmap para las siguientes fases.
> Fecha: 2026-09-18.
> Alcance: nueva API y nueva orquestación en Rust, reutilizando el motor existente y código C++ donde aporte valor.
> Regla principal: ninguna decisión de este documento se considera validada por intuición; cada optimización relevante tiene un benchmark y un criterio de salida.

> **Estado verificable.** La implementación actual vive en `quant-domain`, `quant-engine` y
> `quant-api`. Están operativos `POST /v1/context:apply`, `/v1/prices`,
> `/v1/portfolios:price`, `/v1/scenarios:run`, `/v1/risk:calculate`, `/v1/xva:calculate`,
> sus aliases documentados y los endpoints de health. `clients/python/src/quantdesk/rest.py` es el
> SDK stateless de referencia. Las secciones de fases describen gates y trabajo futuro; el
> contrato vigente se resume en [`docs/api/rest.md`](docs/api/rest.md) y
> [`docs/api/openapi.v1.yaml`](docs/api/openapi.v1.yaml).

## 0. Resumen ejecutivo

La arquitectura recomendada es **Rust como composition root, API, aplicación, planificación y control de recursos; Rust y C++ como proveedores de kernels de cálculo**. La frontera Rust/C++ debe ser gruesa, síncrona y orientada a lotes. REST es el primer transporte por facilidad de adopción, no porque sea la vía óptima para mover matrices grandes. gRPC se añade solo si aparecen clientes internos que necesiten contratos Protobuf, streaming o generación multilenguaje. Para tablas y matrices masivas, Arrow IPC sobre HTTP debe evaluarse antes de desplegar Arrow Flight.

Las decisiones más importantes son:

1. **Axum + Tokio + Tower** para el servicio HTTP. Axum encaja mejor que Actix Web porque comparte el modelo `tower::Service` con el resto del ecosistema y permite reutilizar middleware con una composición explícita. No se espera que la elección del router sea material frente a Monte Carlo, colas, serialización o movimientos de memoria.
2. **Tokio solo para I/O y coordinación**. El cálculo CPU no se ejecuta en futures del runtime. Un scheduler con cola acotada entrega trabajos a un pool CPU fijo; una cola independiente gobierna cada GPU. `spawn_blocking` con semáforo sirve para la primera vertical, pero no es el diseño final de cargas largas.
3. **No crear once crates al principio**. `market`, `models`, `products`, `risk`, `xva` y `gpu` empiezan como módulos. Se extraen solo cuando exista una frontera de dependencias, ownership de equipo o tiempos de compilación que lo justifique.
4. **No modelar cada modelo y producto heterogéneo como dos trait objects cruzados**. El ejemplo `PricingModel::price(&dyn Product, ...)` obliga a realizar downcasts o a codificar una matriz de compatibilidad dentro de cada modelo. Se recomiendan specs/handles tipados, capabilities y un `ExecutionPlan` que agrupe trabajos homogéneos. Hay un único dispatch dinámico por lote hacia un `PricingKernel`.
5. **C++ deja de ser la capa de orquestación de la API nueva**. Conserva modelos y kernels legacy validados, más los clientes existentes durante la transición. Rust valida, planifica, agrupa, limita recursos y normaliza resultados. No se reescribe código C++ correcto sin una razón medida.
6. **`cxx` es la frontera interna**. Se exponen funciones planas y objetos opacos pequeños, nunca el árbol de clases C++. Los inputs numéricos se prestan como slices durante una llamada; los outputs se escriben preferentemente en buffers asignados por Rust.
7. **SIMD no es un backend equivalente a CPU/GPU**. SIMD es una política o implementación del backend CPU. La selección debe distinguir dispositivo (`Cpu`, `Gpu`) de estrategia CPU (`Auto`, escalar, AVX2, AVX-512, etc.) y mantenerse fuera del dominio.
8. **El API principal es batch**: `price_portfolio`, `run_scenarios` y `calculate_risk`. `price` es conveniencia para un lote de uno. El bridge nunca debe cruzarse una vez por path, escenario, cashflow o trade.
9. **JSON acaba en el adaptador HTTP**. El dominio y los kernels trabajan con structs validados, slices y buffers contiguos. `serde_json` es la opción inicial; cambiarlo por una librería SIMD solo se justifica con perfiles reales.
10. **El primer entregable no es GPU ni gRPC**: es una vertical REST → scheduler → cálculo existente, instrumentada, con límites y benchmarks de copia/bridge/batching.
11. **La sesión vive en el cliente**: `QuantContext` es un snapshot tipado, versionado y hashable que el cliente conserva y reenvía; el servidor no crea sesiones ni depende de handles entre requests.

No se recomienda añadir Go. Introduciría otro runtime, otro sistema de tipos, otra toolchain y otra frontera sin resolver ningún problema que Rust + Tokio no cubran ya. Go solo tendría sentido como cliente generado de gRPC o como servicio independiente propiedad de otro equipo.

## 1. Estado real del repositorio y migración, no greenfield

El repositorio no parte de cero:

- `rust/crates/engine-core` ya implementa cálculo, Monte Carlo, modelos Hull-White/GBM, payoff IR, AAD y backends Burn CPU/WGPU.
- `rust/crates/engine-ffi` ya usa `cxx` y genera una librería estática.
- `cpp/engine` contiene hoy dominio/orquestación, registries, `MarketSnapshot`, contextos, pricing batch/grid, payoff AST, clientes y C ABI.
- Python, Excel y la C ABI consumen actualmente la capa C++.
- El motor ya tiene separación Q/P, batching parcial y semillas deterministas en pruebas; también existe una limitación conocida: el RNG del backend de test se protege con un lock global.

Por tanto, una reestructuración total inmediata sería más arriesgada que útil. La migración será de tipo **strangler**:

```text
                         camino actual (se conserva)
Python / Excel / C ABI ───────────────► C++ orchestration ─► Rust core

                         camino nuevo
REST ─► Rust application/engine ─┬────► Rust compute existente
                                 └────► C++ legacy adapter vía cxx
```

Cada capacidad se mueve al camino nuevo solo después de demostrar paridad numérica y de rendimiento. Los clientes existentes no se rompen como condición previa a arrancar la API.

Las ideas previas de `ARCHITECTURE_REVIEW.md` que siguen siendo válidas son el IR de payoff orientado a datos, el trabajo batch, el uso compartido de escenarios y la separación Q para fair value/riesgo frente a P para forecast/P&L. Lo que cambia es el ownership: para esta API, Rust pasa a ser propietario de la orquestación.

## 2. Objetivos y no objetivos

### 2.1 Objetivos

- Latencia predecible bajo carga, con p50/p95/p99 y tiempo de cola visibles.
- Throughput proporcional a recursos sin oversubscription de Tokio, Rayon, Burn, OpenMP o BLAS.
- Una frontera Rust/C++ con cero serialización y cero copia para slices prestados cuando el lifetime lo permite.
- Una API de dominio clara que no conozca HTTP, Protobuf, `cxx`, Burn, CUDA/WGPU ni detalles SIMD.
- Agrupación y reutilización de curvas, paths, shocks, escenarios y resultados intermedios.
- Compatibilidad incremental con modelos legacy C++.
- Reproducibilidad de Monte Carlo y validación cruzada Rust/C++.
- Capacidad de añadir GPU sin contaminar productos o modelos con tipos del dispositivo.

### 2.2 No objetivos iniciales

- Ganar un benchmark sintético de routing HTTP.
- Exponer todo el API existente en la primera fase.
- Convertir REST en un protocolo para transferir gigabytes de paths en JSON.
- Diseñar un sistema distribuido de cálculo antes de medir el límite de una máquina.
- Crear plugins ABI-estables de C++ arbitrario. `cxx` es una frontera compilada en lockstep, no una ABI binaria universal.
- Reescribir modelos C++ solo para alcanzar una arquitectura “pura Rust”.
- Activar NUMA, affinity, huge pages, allocator personalizado o GPU por defecto sin evidencia.

## 3. Arquitectura recomendada

```text
┌──────────────────────────────── Clientes ────────────────────────────────┐
│ Python SDK       Excel/Office       C++/C ABI legacy       otros clientes │
│    │                  │                    │                    │         │
│ REST/JSON       REST o bridge        camino compatible     REST / futuro │
│ REST/Arrow          existente                              gRPC/Protobuf  │
└────┬─────────────────┬────────────────────┬────────────────────┬─────────┘
     │                 │                    │                    │
     ▼                 │                    │                    ▼
┌──────────────────────┴── quant-api (Axum) ┴─────────────────────────────┐
│ auth/rate limits · request limits · DTO/Serde · Problem Details         │
│ request-id · tracing · REST v1 · health/readiness · optional Arrow IPC  │
│ cada request recibe/devuelve QuantContext; no hay session registry      │
└────────────────────────────────┬─────────────────────────────────────────┘
                                 │ Command/Query validado; no JSON
                                 ▼
┌──────────────────────── quant-engine ───────────────────────────────────┐
│ application services · registries · capability matching                │
│ planner batch · cache de artefactos · budgets · cancellation/deadlines  │
│                                                                         │
│              ┌──────────── ComputeScheduler ────────────┐               │
│              │ cola acotada + admisión ponderada       │               │
│              └───────┬───────────────────┬─────────────┘               │
└──────────────────────┼───────────────────┼──────────────────────────────┘
                       │                   │
          pool CPU fijo│                   │cola por dispositivo GPU
                       ▼                   ▼
             ┌────────────────┐   ┌────────────────────┐
             │ quant-compute  │   │ quant-compute/gpu  │
             │ CPU/Burn/SIMD  │   │ WGPU/CUDA futuro   │
             │ AAD/MC/kernels │   │ memory budgeting   │
             └───────┬────────┘   └────────────────────┘
                     │ ExecutionPlan / domain buffers
                     │
              ┌──────▼────────────────────────────────────┐
              │ quant-domain                              │
              │ IDs · specs · MarketSnapshot · Portfolio  │
              │ Scenario · contexts · results · errors    │
              │ sin async · sin HTTP · sin hardware · FFI │
              └───────────────────────────────────────────┘

         proveedor adicional del mismo SPI de kernels
                       │
                       ▼
              ┌──────────────────────┐
              │ quant-cpp            │
              │ Rust adapters        │
              │ cxx bridge + shims   │
              └──────────┬───────────┘
                         │ slices/opaque handles; sin JSON
                         ▼
              ┌──────────────────────┐
              │ C++ legacy/models    │
              │ kernels ya validados │
              └──────────────────────┘

Observabilidad transversal: tracing → OpenTelemetry/OTLP, métricas, perfiles y benchmarks.
```

### Flujo de una petición

```text
HTTP async
  │ parseo + validación + límites
  ▼
Application Service
  │ resuelve IDs/specs y compila ExecutionPlan
  ▼
ComputeScheduler.submit(OwnedJob)
  │ espera asíncrona en oneshot; Tokio queda libre
  ▼
worker CPU / cola GPU
  │ un dispatch por batch; Rust kernel o C++ adapter
  ▼
PricingResult/RiskResult interno
  │ mapeo a DTO y serialización fuera del worker si no es costosa
  ▼
HTTP response o JobResult persistido
```

Para trabajos largos, el endpoint devuelve `202 Accepted` y un `job_id`. Para cálculos pequeños bajo un presupuesto configurable, puede responder síncronamente. Un timeout HTTP no cancela mágicamente una llamada C++ o un kernel GPU: la cancelación debe ser cooperativa y el resultado tardío debe descartarse o almacenarse según política.

## 4. Stack Rust recomendado

| Componente | Decisión | Motivo y límite |
|---|---|---|
| HTTP | **Axum** | Integración directa con Hyper/Tokio/Tower, extractors claros y middleware compartido. Actix Web también es rápido, pero no aporta una ventaja material al workload y añade otro modelo de servicio. |
| Runtime | **Tokio** | I/O, timers, signals, canales y coordinación. Nunca ejecutar un pricing largo en un task async. |
| Middleware | **Tower + tower-http** | Concurrency limit, load shedding, request IDs, tracing, body limits, CORS y timeouts. Ordenar capas deliberadamente. |
| JSON | **serde + serde_json** | Opción estable y mantenible. `simd-json` solo tras benchmark de payloads representativos; requiere buffers mutables y complica el pipeline. |
| Context packed | **CBOR determinista + SHA-256**, compresión zstd opcional | La forma canónica se usa para `context_hash`; elegir una implementación (`ciborium`/equivalente) tras verificar canonicalización de floats. No mantener JSON, MessagePack y CBOR como tres contratos distintos. |
| Errores | **thiserror** en librerías; mapping propio en API | No usar `anyhow` como contrato público. Puede usarse en `main`/bootstrap. |
| Observabilidad | **tracing + tracing-subscriber + tracing-opentelemetry + OpenTelemetry OTLP** | Contexto estructurado end-to-end. Los kernels emiten eventos agregados, no un span por path/trade. |
| Métricas | API de métricas compatible con OTel/Prometheus | Histogramas explícitos por etapa; labels de baja cardinalidad. |
| CPU | **pool Rayon propio o workers dedicados** | Tamaño fijo, nombre/afinidad configurable, cola acotada. No usar el pool global sin política. |
| gRPC | **Tonic, diferido** | Encaja con Tower y Protobuf. Añadir solo ante caso de uso; no mantener REST y gRPC por anticipación. |
| API schema | OpenAPI generado o validado desde DTO REST | El schema es parte del contrato y se prueba por snapshot/compatibilidad. |
| Benchmark | Criterion + harness macro propio | Criterion para microbench estadístico; harness dedicado para end-to-end y saturación. |

### Axum frente a Actix Web

La decisión no se basa en que Axum sea universalmente “más rápido”. Ambos pueden manejar muchas más peticiones que las que un motor Quant CPU-bound podrá valorar. Axum gana aquí por coherencia:

- Tower permite la misma semántica de middleware y backpressure que Tonic si se añade después.
- El estado compartido es Rust convencional (`Arc<AppState>`).
- Es sencillo separar adaptadores HTTP de application services.
- Reduce la cantidad de abstracciones específicas del framework.

Solo se reconsidera Actix si un benchmark end-to-end con cálculo simulado muestra una mejora material de p99/CPU y el perfil atribuye el coste al framework, no a JSON, allocator o logging. Es poco probable.

### Tokio y Tower: configuración inicial

- Runtime Tokio multithread para la API, con número de core threads limitado y separado conceptualmente del pool CPU.
- `ConcurrencyLimitLayer` y load shedding antes de admitir trabajo caro.
- Límite de body y validación de `n_paths`, `n_steps`, trades, escenarios y bytes de salida antes de reservar memoria.
- Timeout de admisión/cola separado de deadline de cálculo.
- Request ID propagado; trace ID devuelto en errores.
- Compresión HTTP solo para respuestas que se beneficien; comprimir arrays de doubles puede consumir más CPU de la que ahorra.
- No aplicar capas indiscriminadamente: el orden de timeout, tracing, auth y load shedding cambia qué latencia se mide y qué respuesta observa el cliente.

## 5. Workspace Cargo y dependencias entre capas

La estructura objetivo es deliberadamente más pequeña que la lista conceptual original:

```text
engine_quant/
├── Cargo.toml                         # workspace nuevo en raíz o delega a rust/
├── rust/
│   ├── Cargo.toml
│   └── crates/
│       ├── quant-domain/
│       │   └── src/
│       │       ├── lib.rs
│       │       ├── ids.rs
│       │       ├── market.rs
│       │       ├── model.rs
│       │       ├── product.rs
│       │       ├── portfolio.rs
│       │       ├── scenario.rs
│       │       ├── context.rs
│       │       ├── result.rs
│       │       └── error.rs
│       ├── quant-engine/
│       │   └── src/
│       │       ├── lib.rs
│       │       ├── engine.rs
│       │       ├── service.rs
│       │       ├── registry.rs
│       │       ├── planner.rs
│       │       ├── scheduler.rs
│       │       ├── cache.rs
│       │       ├── risk.rs
│       │       ├── xva.rs
│       │       └── spi.rs             # traits implementados por compute/cpp
│       ├── quant-compute/
│       │   └── src/
│       │       ├── lib.rs
│       │       ├── cpu.rs
│       │       ├── simd.rs
│       │       ├── gpu.rs             # feature opcional, no crate inicial
│       │       ├── mc.rs
│       │       ├── aad.rs
│       │       ├── models/
│       │       ├── products/
│       │       └── payoff/
│       ├── quant-cpp/
│       │   ├── Cargo.toml
│       │   ├── build.rs
│       │   └── src/
│       │       ├── lib.rs
│       │       ├── bridge.rs
│       │       ├── adapter.rs
│       │       └── error.rs
│       ├── quant-api/
│       │   └── src/
│       │       ├── main.rs
│       │       ├── state.rs
│       │       ├── routes.rs
│       │       ├── dto.rs
│       │       ├── error.rs
│       │       └── telemetry.rs
│       ├── quant-grpc/                 # no se crea hasta una fase posterior
│       └── quant-test-support/         # fixtures/goldens compartidos, si se justifica
├── cpp/
│   ├── legacy/                         # o cpp/engine durante la transición
│   │   ├── include/
│   │   ├── src/
│   │   └── CMakeLists.txt
│   └── bridge/
│       ├── include/quant_cpp_bridge.hpp
│       └── src/quant_cpp_bridge.cpp
├── benches/
│   ├── datasets/
│   └── reports/
└── proto/                              # solo cuando exista quant-grpc
```

No debe hacerse un rename masivo de `engine-core` antes de la vertical REST. Inicialmente `quant-compute` puede ser el crate actual `engine-core` detrás de un adapter. La extracción/renombrado ocurre cuando las nuevas interfaces estén probadas.

### Grafo de dependencias permitido

```text
quant-domain
    ▲
    │
quant-engine  (define application services y el SPI PricingKernel)
    ▲   ▲
    │   │
quant-compute     quant-cpp             # ambos implementan quant-engine::spi
       ▲             ▲
       └──────┬──────┘
              │
          quant-api                     # composition root

quant-grpc ──► quant-engine (+ adapters en su composition root)
```

Reglas verificables con CI (`cargo deny`, un test de arquitectura o inspección de metadata):

- `quant-domain` no depende de Axum, Tokio, Serde, Tonic, CXX, Burn, Rayon, WGPU o OpenTelemetry.
- `quant-engine` no depende de Axum/Tonic/CXX y no conoce `ffi::*`.
- `quant-compute` y `quant-cpp` implementan el SPI; no se importan entre sí.
- `quant-api` es quien elige e inyecta providers, scheduler, caches y observabilidad.
- DTO HTTP/Protobuf nunca entran en kernels.
- Los tipos de Burn/CUDA/WGPU no salen de `quant-compute`.
- Los tipos C++ opacos no salen de `quant-cpp`.

### ¿Cuándo extraer más crates?

`quant-market`, `quant-products`, `quant-models`, `quant-risk`, `quant-xva` y `quant-gpu` se extraen únicamente si se cumple al menos uno:

- necesitan ser versionados o consumidos de forma independiente;
- hay una frontera de ownership de equipo;
- reducen de forma medida el coste de compilación;
- requieren features/dependencias pesadas que conviene aislar;
- su grafo ya es acíclico y estable.

Crear crates solo porque hay cajas en un diagrama aumenta versiones, boilerplate, tiempos de CI y riesgo de ciclos. `gpu` en particular es una implementación de compute, no un bounded context de negocio.

## 6. Modelo de dominio y API de Engine

### 6.1 Tipos centrales

- `MarketSnapshot`: inmutable, versionado, validado al crear; usa `Arc<[f64]>`/buffers contiguos para curvas, superficies y matrices. Incluye layout, dimensiones, unidades, day-count y as-of explícitos.
- `ModelSpec`: configuración declarativa de un modelo. No contiene un backend de hardware.
- `ProductSpec`: trade/producto declarativo o payoff IR compilable.
- `Portfolio`: trades con `TradeId`, cantidad, netting set, collateral set y metadatos de baja cardinalidad. No es simplemente `Vec<Product>` si XVA es objetivo.
- `PricingContext`: fecha, medida Q/P, tolerancias, paths/steps, seed, medidas pedidas y políticas numéricas.
- `ExecutionPolicy`: preferencia de dispositivo, paralelismo, deadline, determinismo y presupuesto. Vive en application/engine, no en dominio financiero.
- `ScenarioSet`: shocks estructurados y susceptibles de vectorización; no callbacks arbitrarios.
- `PricingResult`/`RiskResult`: buffers densos más índices/metadata; no `HashMap<String, Value>` en el hot path.

La separación Q/P existente debe mantenerse en el contrato, no esconderse en un booleano ambiguo:

```rust
pub enum ProbabilityMeasure {
    RiskNeutralQ,
    PhysicalP { forecast_model: ForecastModelId },
}
```

### 6.2 Handles, IDs y registries

Se recomiendan **specs y handles tipados en la librería; IDs/versiones en el transporte**:

```rust
#[repr(transparent)]
pub struct MarketId(uuid::Uuid);

#[repr(transparent)]
pub struct ModelId(uuid::Uuid);

pub struct MarketHandle(Arc<MarketSnapshot>);
pub struct ModelHandle(Arc<ModelEntry>);
pub struct ProductHandle(Arc<ProductEntry>);
```

- `Arc` comparte objetos inmutables y evita copias grandes.
- Si hay delete/reuse en un registry en memoria, usar IDs generacionales (`slotmap`) o UUID, nunca un índice reutilizable desnudo.
- Los handles no son identidad durable entre réplicas. REST debe aceptar definiciones inline o IDs que resuelva un catálogo compartido/versionado.
- Un registry local sirve para caching y clientes embebidos; no debe convertirse accidentalmente en estado de sesión imprescindible para escalar horizontalmente.
- El objeto que el cliente conserva entre llamadas no es un `MarketHandle`, `ModelHandle` ni un puntero Rust/C++; es un `QuantContext` declarativo, versionado y serializable. Los handles internos se reconstruyen por request y se descartan al terminar.
- Las claves de cache incluyen versión/hash canónico de market/model/product/context, backend y versión de kernel.

### 6.3 Interfaz pública propuesta

```rust
pub struct Engine {
    planner: Planner,
    scheduler: Arc<ComputeScheduler>,
    registries: Registries,
}

impl Engine {
    pub fn new(config: EngineConfig, providers: Vec<Arc<dyn PricingKernel>>) -> Result<Self>;

    pub fn create_market(&self, spec: MarketSpec) -> Result<MarketHandle>;
    pub fn create_model(&self, spec: ModelSpec) -> Result<ModelHandle>;
    pub fn create_product(&self, spec: ProductSpec) -> Result<ProductHandle>;

    // API síncrona útil para embedding/tests. Ejecuta el plan en el caller o scheduler sync.
    pub fn price(
        &self,
        product: &ProductHandle,
        model: &ModelHandle,
        market: &MarketHandle,
        ctx: &PricingContext,
    ) -> Result<PricingResult>;

    pub fn price_portfolio(
        &self,
        portfolio: &Portfolio,
        model: &ModelHandle,
        market: &MarketHandle,
        ctx: &PricingContext,
    ) -> Result<PortfolioPricingResult>;

    pub fn run_scenarios(
        &self,
        request: ScenarioRequest<'_>,
    ) -> Result<ScenarioResult>;

    pub fn calculate_risk(
        &self,
        request: RiskRequest<'_>,
    ) -> Result<RiskResult>;
}

// Fachada async de aplicación usada por Axum: posee el job y no bloquea Tokio.
pub struct PricingService {
    engine: Arc<Engine>,
    dispatcher: Arc<JobDispatcher>,
}

impl PricingService {
    pub async fn submit_price(&self, command: PriceCommand) -> Result<PricingResult>;
    pub async fn submit_portfolio(&self, command: PortfolioCommand) -> Result<JobId>;
}
```

La API síncrona y la fachada async no deben duplicar lógica. La segunda crea un job owned, lo envía al scheduler y espera por un canal oneshot.

### 6.4 Contexto de sesión propiedad del cliente

El flujo de los notebooks debe conservar su ergonomía:

```python
engine = qd.Engine(backend="cpu", seed=7)
market = qd.Market(...)
model = qd.HullWhite1F(...)
swap = qd.IRSwap(...)
result = engine.price(swap, model, market)
```

En REST no se mantiene un `engine`, `market`, `model` o `swap` vivo en el servidor. El cliente crea y va actualizando un documento `QuantContext`; cada request es una función que recibe un contexto y devuelve el siguiente contexto:

```text
Context₀ + add_market  ──► Context₁ + MarketRef
Context₁ + add_model   ──► Context₂ + ModelRef
Context₂ + add_product ──► Context₃ + ProductRef
Context₃ + price       ──► Context₃' + PricingResult
Context₃' + add_trade  ──► Context₄ + PortfolioRef
Context₄ + risk        ──► Context₄' + RiskResult
```

`Contextₙ` es un snapshot inmutable desde el punto de vista de cada request. El cliente puede tratarlo como una estructura persistente copy-on-write: crear una nueva versión no muta accidentalmente una rama anterior. El servidor materializa objetos temporales, ejecuta y los libera; no hay `session_id` que consultar en una tabla de sesiones.

El contexto acumula definiciones y configuración, no necesariamente todos los resultados numéricos. Por defecto, un `PricingResult` se devuelve fuera del contexto. Si el usuario quiere replay/auditoría, puede añadir un `RunRecord` pequeño (hashes, command, seed, métricas y referencias); incrustar cada matriz completa en el contexto haría crecer el payload sin límite.

Tipos de dominio/contrato:

```rust
pub struct QuantContext {
    pub schema: ContextSchema,          // "quant.context/v1"
    pub context_id: ContextId,          // generado por el cliente
    pub revision: u64,
    pub parent_hash: Option<ContextHash>,
    pub context_hash: ContextHash,
    pub engine: EngineSpec,
    pub markets: BTreeMap<MarketId, MarketSpec>,
    pub models: BTreeMap<ModelId, ModelSpec>,
    pub products: BTreeMap<ProductId, ProductSpec>,
    pub portfolios: BTreeMap<PortfolioId, PortfolioSpec>,
    pub pricing: PricingDefaults,
    pub runs: Vec<RunRecord>,            // opcional y acotado; metadata, no cubos
}

pub struct EngineSpec {
    pub execution: ExecutionPolicy,
    pub provider_preference: Option<ProviderId>,
}

pub struct PricingDefaults {
    pub pricing_date: Date,
    pub n_paths: u64,
    pub n_steps: u64,
    pub seed: u64,
    pub measure: ProbabilityMeasure,
}

pub struct ContextEnvelope {
    pub context: QuantContextEncoding,
    pub operation_id: OperationId,       // idempotencia/replay del cliente
    pub command: ContextCommand,
}

pub enum QuantContextEncoding {
    Inline(QuantContext),                // JSON legible o CBOR/MessagePack
    Packed { media_type: String, bytes: Vec<u8> },
}
```

En el API HTTP se usa JSON inline para desarrollo y payloads pequeños. Para producción, el mismo tipo puede viajar como CBOR/MessagePack comprimido, firmado y opcionalmente cifrado. El encoding es una optimización de transporte, no otro modelo de dominio.

Reglas de la sesión client-owned:

- `context_id` no identifica una fila del servidor; es una identidad lógica del cliente.
- `context_hash` es hash canónico de la representación validada; `parent_hash` permite al cliente detectar ramas o respuestas fuera de orden.
- La canonicalización recomendada es CBOR determinista con mapas ordenados y números finitos normalizados; `context_hash = SHA-256(canonical_context_bytes)`. JSON es una vista de transporte/debug, no la fuente del hash, porque el orden de claves y la representación de floats pueden variar.
- `revision` es monotónica dentro de una rama local. El servidor no consulta una revisión anterior para aceptar una nueva: valida la cadena incluida en el request.
- Dos pestañas pueden crear ramas del mismo contexto. El servidor no hace merge implícito; el SDK ofrece `fork`, `replace` y, más adelante, un merge explícito por recursos con conflictos.
- El servidor no confía en IDs internos ni en resultados enviados por el cliente; recalcula hashes y valida referencias.
- El cliente puede guardar el contexto en un notebook, fichero, base local o secreto gestionado por él. El servidor no necesita conocer su historial.
- Todas las operaciones deben ser deterministas o declarar la fuente de no determinismo (`seed`, timestamp de market, versión de kernel).

#### Contexto completo frente a contexto compacto

Un contexto completamente autocontenido es la definición más clara de stateless, pero puede repetir curvas, superficies o portfolios grandes en cada llamada. Se soportan tres representaciones con la misma semántica:

| Representación | Uso | Límite |
|---|---|---|
| JSON inline | Notebook, depuración, contextos pequeños | Más bytes, parseo y allocations |
| Context token packed | Producción con contexto mediano | El servidor debe tener claves de firma/cifrado; sigue existiendo un límite de tamaño |
| Multipart metadata + Arrow buffers | Mercados/superficies/escenarios grandes | Más complejidad de transporte; cada request sigue incluyendo los bytes necesarios |

Un `Content-AddressedRef` no es mágicamente stateless. Si solo contiene un hash y el servidor debe recuperar los bytes, existe un almacenamiento externo y debe declararse como dependencia. El modo por defecto no presupone una cache o blob store server-side.

#### No confundir stateless con “sin memoria física”

El worker puede usar memoria temporal, caches no autoritativas y pool de threads durante una request. Lo que no puede hacer en modo stateless es depender de ellos para reconstruir el contexto de la siguiente request. Una cache se puede perder sin cambiar la corrección; un `QuantContext` no.

## 7. Heterogeneidad: traits, enums, generics y dispatch

### 7.1 Por qué no usar `PricingModel::price(&dyn Product, ...)`

Esta firma parece extensible, pero desplaza el problema:

- cada modelo debe saber hacer downcast de cada producto;
- una pareja incompatible falla en runtime;
- el modelo termina absorbiendo lógica de producto y medida;
- el dispatch ocurre demasiado cerca del hot loop;
- no expresa capacidades como discounting, stochastic factors, path dependency o AAD;
- cruzar `dyn Product` hacia C++ no es viable como una interfaz pequeña de `cxx`.

Los generics son excelentes dentro de kernels homogéneos, pero no pueden ser el contenedor heterogéneo público sin monomorfizar todas las combinaciones. Los enums son ideales para el conjunto built-in cerrado; los registries/trait objects sirven en el borde extensible.

### 7.2 Diseño híbrido recomendado

```rust
pub enum ModelSpec {
    HullWhite1F(HullWhite1FSpec),
    Heston(HestonSpec),
    Sabr(SabrSpec),
    External { provider: ProviderId, kind: String, payload: Arc<[u8]> },
}

pub enum ProductSpec {
    IrSwap(IrSwapSpec),
    EuropeanOption(EuropeanOptionSpec),
    PayoffIr(PayoffProgram),
    External { provider: ProviderId, kind: String, payload: Arc<[u8]> },
}

bitflags::bitflags! {
    pub struct Capabilities: u64 {
        const ANALYTIC       = 1 << 0;
        const MONTE_CARLO    = 1 << 1;
        const PATH_DEPENDENT = 1 << 2;
        const AAD            = 1 << 3;
        const CPU            = 1 << 4;
        const GPU            = 1 << 5;
        const SCENARIOS      = 1 << 6;
    }
}

pub trait PricingKernel: Send + Sync {
    fn descriptor(&self) -> &KernelDescriptor;
    fn supports(&self, request: &PlanRequirements) -> SupportLevel;
    fn execute(
        &self,
        batch: &CompiledBatch,
        output: &mut BatchOutput,
        control: &ExecutionControl,
    ) -> Result<()>;
}
```

El `Planner`:

1. valida producto/modelo/market/measure;
2. deriva capabilities y dependencias;
3. selecciona un provider compatible según política y coste observado;
4. agrupa trades por kernel, modelo, layout de producto, dependencias de mercado y dispositivo;
5. compila un `CompiledBatch` contiguo;
6. hace **un dispatch dinámico por grupo**;
7. dentro del kernel usa generics/enums y loops estáticos.

Así pueden coexistir `HullWhiteRust`, `HestonRust`, `CppHullWhite`, `CppSabr` y `CppQuantLibModel` sin presentar cada clase C++ como un trait object Rust. Son providers/kernels registrados con capabilities y formatos de entrada estables.

### 7.3 Compatibilidad modelo-producto

No debe resolverse por nombres o `Any`. Cada factory produce descriptores:

```rust
pub struct KernelDescriptor {
    pub id: KernelId,
    pub provider: ProviderId,
    pub model_kinds: &'static [ModelKind],
    pub product_kinds: &'static [ProductKind],
    pub measures: &'static [MeasureKind],
    pub capabilities: Capabilities,
    pub input_layout_version: u32,
}
```

Las combinaciones inválidas fallan durante `compile_plan`, antes de reservar paths o entrar en C++.

## 8. Backend de ejecución sin contaminar el dominio

El enum conceptual `Backend { Cpu, Simd, Gpu }` mezcla dos dimensiones. SIMD ejecuta en CPU. Se recomienda:

```rust
pub enum DevicePreference {
    Auto,
    Cpu,
    Gpu { device: Option<u32> },
}

pub enum CpuVectorPolicy {
    Auto,
    Scalar,
    PortableSimd,
    Avx2,
    Avx512,
}

pub struct ExecutionPolicy {
    pub device: DevicePreference,
    pub cpu_vector: CpuVectorPolicy,
    pub max_threads: Option<usize>,
    pub deterministic: bool,
    pub deadline: Option<std::time::Instant>,
    pub memory_budget_bytes: u64,
}
```

`ProductSpec`, `ModelSpec`, `MarketSnapshot` y `PricingContext` no importan estos tipos. El planner combina requisitos del dominio con `ExecutionPolicy` y produce un target físico:

```rust
pub enum ExecutionTarget {
    Cpu(CpuTarget),
    Gpu(GpuTarget),
    LegacyCpp(CppTarget),
}
```

`Auto` no significa “GPU siempre”. Debe seleccionar por modelo de coste medido: tamaño del lote, bytes host→device, residencia de datos, memoria disponible, warm-up y kernels compatibles. Para lotes pequeños, CPU suele ganar aunque GPU tenga mayor throughput.

Burn puede seguir siendo el backend tensorial existente, pero no debe confundirse con la arquitectura. Si un kernel escalar/SIMD manual supera a Burn para curvas o PV analítico, ambos pueden implementar el mismo SPI. No se obliga a que todo sea tensor.

## 9. Paralelización, backpressure y scheduling

### 9.1 Regla de ownership de threads

Debe haber una sola capa responsable de paralelizar cada dimensión:

- Tokio: conexiones, I/O, timers y coordinación.
- Scheduler: concurrencia entre jobs.
- Kernel CPU: paralelismo dentro de un batch, si compensa.
- Backend tensorial/BLAS/C++: threads internos solo si el scheduler lo sabe.
- GPU: colas por dispositivo y streams controlados.

Permitir que Tokio lance N jobs, cada job use Rayon con N threads y cada kernel C++ active OpenMP/BLAS con N threads provoca oversubscription, cache thrashing y p99 inestable. El `KernelDescriptor` debe declarar su modelo de threading (`SingleThreaded`, `InternalParallelism`, `ExclusiveDevice`) y el scheduler ajustar permisos.

### 9.2 Diseño inicial

```rust
pub struct ComputeScheduler {
    cpu_pool: rayon::ThreadPool,
    admission: WeightedSemaphore,
    queue: BoundedJobQueue,
    gpu_queues: Vec<GpuQueue>,
}

pub struct JobCost {
    pub cpu_units: u32,
    pub estimated_bytes: u64,
    pub gpu_bytes: u64,
}
```

- Cola acotada: rechazar con `ResourceExhausted`/HTTP 429 o 503 antes de agotar RAM.
- Admisión ponderada: un PV analítico y 10 millones de paths no consumen el mismo permiso.
- El request se transforma a `OwnedJob`: `Arc` para snapshots inmutables y buffers owned para datos que sobrevivirán al handler.
- El worker devuelve por oneshot. Esperar el receiver es async y no bloquea Tokio.
- Si un job se cancela antes de arrancar, se elimina de la cola. En ejecución requiere checkpoints cooperativos por chunk de paths/escenarios.
- No prometer deadline duro para una llamada legacy C++ no interrumpible. Registrar `deadline_exceeded_but_compute_continued`.

### 9.3 `spawn_blocking` frente a pool dedicado

En Fase 1 se permite:

```rust
let permit = cpu_semaphore.acquire_owned().await?;
let result = tokio::task::spawn_blocking(move || {
    let _permit = permit;
    engine.price_owned(job)
}).await??;
```

Esto proporciona una vertical rápida, pero tiene límites: el pool blocking de Tokio admite por defecto muchos threads y las tareas ya iniciadas no se abortan. Antes de Monte Carlo paralelo se migra a un pool fijo propio. Para loops persistentes o workers con estado C++ thread-affine, usar threads dedicados con canales, no `spawn_blocking`.

### 9.4 Affinity y NUMA

No activar en la primera versión. Tiene sentido cuando:

- el host es multi-socket;
- el working set supera LLC;
- los contadores muestran tráfico remoto NUMA;
- los jobs son suficientemente largos para amortizar scheduling.

Entonces se crean pools por nodo NUMA, se aplica first-touch de buffers en el nodo de ejecución y se intenta mantener market/path data local. Se reserva al menos capacidad para Tokio/SO. La afinidad es una opción de despliegue, nunca un supuesto del dominio.

## 10. Diseño del bridge Rust ↔ C++ con `cxx`

### 10.1 Principios

- Rust llama a un **shim C++ estable y plano**, no a la jerarquía de dominio legacy.
- Un crossing representa un batch completo o un kernel significativo.
- No se cruzan JSON, `serde_json::Value`, `Params` dinámicos ni nombres repetidos en el hot path.
- Los strings se resuelven a enums/IDs durante planificación.
- Los objetos C++ se tratan como opacos.
- No se almacena en C++ ningún puntero prestado desde Rust más allá de la llamada.
- Toda precondición de shape/layout se valida en Rust y se reafirma en el shim en builds de test/debug.
- No se permite que una excepción o panic atraviese una frontera no declarada como fallible.

### 10.2 Superficie sugerida

```rust
#[cxx::bridge(namespace = "quant::bridge")]
mod ffi {
    #[repr(u8)]
    enum CppErrorCode {
        Ok = 0,
        InvalidArgument = 1,
        Unsupported = 2,
        NumericalFailure = 3,
        ResourceExhausted = 4,
        Internal = 255,
    }

    struct CppStatus {
        code: CppErrorCode,
        message: String,
    }

    struct BatchShape {
        trades: u64,
        scenarios: u64,
        factors: u64,
        times: u64,
    }

    unsafe extern "C++" {
        include!("quant-cpp/include/quant_cpp_bridge.hpp");

        type CppKernel;

        fn new_kernel(kind: u32, config: &[u8]) -> Result<UniquePtr<CppKernel>>;

        fn capabilities(self: &CppKernel) -> u64;

        // Rust asigna output. C++ no retiene ningún slice.
        fn price_batch(
            self: &CppKernel,
            shape: BatchShape,
            market: &[f64],
            model: &[f64],
            products: &[f64],
            output: &mut [f64],
        ) -> CppStatus;
    }
}
```

En producción, `config: &[u8]` no debe ser JSON; debe ser un pequeño encoding versionado o structs compartidos cuando el schema sea estable. Para payloads numéricos grandes se pasan slices separados con un descriptor de layout, no un blob autorreferencial.

El shim convierte un `rust::Slice<const double>` a `std::span<const double>` sin copiar:

```cpp
inline std::span<const double> as_span(rust::Slice<const double> xs) noexcept {
    return {xs.data(), xs.size()};
}

inline std::span<double> as_span(rust::Slice<double> xs) noexcept {
    return {xs.data(), xs.size()};
}
```

La clase `CppKernel` encapsula el mínimo estado estable: una instancia legacy o un dispatcher interno. No exporta `IModel`, `IProduct`, `IMeasure`, QuantLib handles ni templates.

### 10.3 Ownership y lifetimes

| Caso | Representación | Regla |
|---|---|---|
| Objeto C++ exclusivo | `cxx::UniquePtr<CppKernel>` | Opción por defecto. El adapter Rust es propietario; destrucción RAII en C++. Solo deleter estándar. |
| Objeto C++ compartido | `cxx::SharedPtr<T>` | Solo si la librería ya usa ownership compartido y `T` es thread-safe. Cada clone tiene coste atómico. |
| Compartir adapter en Rust | `Arc<CppKernelAdapter>` que posee `UniquePtr` | Preferible a `SharedPtr` cuando Rust controla el sharing; si el tipo C++ no es `Sync`, usar un worker owner/thread confinement, no inventar `unsafe impl Sync`. |
| Input temporal Rust | `&[f64]`, `&str` | Préstamo válido solo durante la llamada. C++ no lo guarda, mueve ni libera. |
| Output en buffer Rust | `&mut [f64]` | Acceso exclusivo durante la llamada; C++ rellena exactamente la capacidad acordada. |
| Buffer C++ retenido | `UniquePtr<CppBuffer>` + método slice | Zero-copy mientras el owner C++ viva; si Rust necesita un `Vec`, habrá copia. |
| Ownership Rust hacia C++ | `Vec<T>`/`Box<T>` de `cxx` | Transferencia puede mover ownership sin copiar el payload, pero C++ debe usar `rust::Vec`, no asumir `std::vector`. |

Las referencias con lifetimes complejos no deben aparecer en el contrato público del bridge. Un método que devuelve una vista debe ligar inequívocamente su lifetime a `&self`; para resultados de larga vida, el owner opaco viaja con la vista.

### 10.4 `UniquePtr` y `SharedPtr`

- `UniquePtr<T>` es adecuado para tipos C++ opacos y expresa ownership sin ambigüedad.
- `SharedPtr<T>` no convierte un tipo mutable en thread-safe. Solo modela recuento de referencias.
- Evitar árboles de `SharedPtr` cruzando el bridge: ocultan ciclos y multiplican atomics.
- Si un modelo es costoso de construir y seguro para concurrencia, Rust puede cachear `Arc<CppAdapter>`; si no lo es, el pool mantiene una instancia por worker.
- No usar custom deleters en la firma `cxx`; envolver el recurso en una clase C++ con destructor normal.

### 10.5 Slices, vectores, matrices y superficies

El formato canónico interno será contiguo row-major con descriptor explícito:

```rust
pub struct MatrixView<'a> {
    pub values: &'a [f64],
    pub rows: usize,
    pub cols: usize,
    pub row_stride: usize,
}
```

En el bridge se pasan campos primitivos más slice, no este struct Rust si cambia con frecuencia. Validaciones:

- `rows * row_stride <= values.len()` con overflow checked;
- layout/version explícitos;
- endian nativo solo dentro del proceso;
- política de NaN/Inf definida;
- unidades y orden de ejes documentados;
- límite de elementos antes de multiplicar dimensiones.

`Vec<f64>` garantiza alineación para `f64`, no necesariamente 32/64 bytes para kernels AVX. Primero usar loads unaligned o la abstracción del backend; si el perfil exige alineación mayor, introducir un `AlignedBuffer` owner específico y mantenerlo detrás del adapter. No suponer que `std::span` aporta alineación.

### 10.6 Strings y enums

- `&str` ↔ `rust::Str`: vista sin copia; solo durante la llamada.
- `String` ↔ `rust::String`: ownership Rust; útil para errores/control plane, no hot path.
- `CxxString`/`std::string`: usar solo si C++ debe ser propietario.
- Nombres de modelo/producto/measure se convierten una vez a IDs numéricos.
- Enums compartidos tienen discriminantes explícitos y tests de compatibilidad. No persistir su representación binaria sin versión.
- Para extensiones desconocidas usar `provider_id + kind_id`, no añadir strings a cada path.

### 10.7 Errores y excepciones

`cxx` puede traducir excepciones C++ a `cxx::Exception` cuando una función se declara `Result<T>`, pero se pierde tipado rico. Estrategia:

1. Constructor/operaciones que no pueden devolver un status separado: se declaran `Result<T>`.
2. El shim C++ captura excepciones conocidas y las normaliza.
3. Para el hot path con output preasignado, se devuelve `CppStatus { code, message }`; no se usa una excepción para errores de negocio esperables.
4. `catch (...)` en el shim produce `Internal` sin dejar escapar la excepción.
5. Rust convierte inmediatamente a `QuantError::Legacy { code, message, provider }`.
6. Ningún `panic!`, `assert!`, indexación no validada o `unwrap()` puede alcanzar el bridge de producción.

```text
C++ domain/numerical exception
        │ catch in shim
        ▼
CppStatus / cxx::Result
        │ map once
        ▼
QuantError
        ├── REST Problem Details + HTTP status
        └── futuro gRPC Status
```

No se expone texto de excepción interno al cliente por defecto. Se loguea con trace ID y el API devuelve código estable y detalle sanitizado.

### 10.8 Callbacks

Evitar callbacks en pricing. Complican reentrancia, lifetimes, thread affinity, panics y cancelación; `cxx` tampoco ofrece una abstracción general de closures capturantes en ambas direcciones.

- Progreso: el worker actualiza estado temporal y lo emite por streaming mientras vive la conexión; el estado durable, si se quiere, lo conserva el cliente o un job store explícito fuera del modo stateless.
- Cancelación: token opaco y polling por chunks, o segmentar el cálculo; no invocar un closure Rust por path.
- Logging C++: sink C++ thread-safe que agrega o integra con un callback muy grueso, nunca en inner loops.
- Si un callback es imprescindible, debe ser una función libre sin captura, documentada `noexcept`, y cualquier panic Rust debe quedar imposible por construcción.

## 11. Zero-copy y low-copy: qué es real

“Zero-copy” solo puede afirmarse indicando owner, lifetime y layout. Cruzar una frontera de lenguaje en el mismo proceso puede ser zero-copy; cruzar la red no.

| Flujo | ¿Copia? | Diseño recomendado |
|---|---:|---|
| Rust `&[f64]` → C++ `rust::Slice<const double>` → `std::span<const double>` | No | C++ consume durante la llamada y no retiene el puntero. |
| Rust `&mut [f64]` → C++ output | No | Rust reserva una vez; C++ escribe in-place. Mejor opción para arrays de resultado. |
| Rust `Vec<f64>` movido como `rust::Vec<double>` | Payload no; header/ownership sí | Útil si C++ acepta el tipo Rust y será owner temporal. No equivale a `std::vector`. |
| Rust `Vec<f64>` → C++ `std::vector<double>` | Sí, salvo cambio de API | Sustituir parámetro por span o mantener `rust::Vec`. |
| C++ `std::vector<double>` visto desde Rust como `CxxVector<double>` | No para consultar prestado | Rust puede mantener el owner C++; convertir a `Vec<f64>` requiere copia. |
| C++ buffer opaco + `as_slice()` | No | Mantener `UniquePtr<CppBuffer>` vivo. Bueno para resultados grandes consumidos inmediatamente. |
| HTTP JSON → structs Rust | Sí | Parseo/asignaciones inevitables. No enviar paths/matrices masivas en JSON. |
| Protobuf por gRPC → structs/buffers | Sí/low-copy parcial | La red y decode siguen existiendo; packed doubles no son automáticamente el buffer final del kernel. |
| FlatBuffers recibidos | Lectura sin unpacking posible | El kernel puede requerir copia/repack por alineación/layout; validar antes de prometer zero-copy. |
| Arrow IPC/Flight → Arrow buffers | Potencialmente zero/low-copy | Excelente para columnas/buffers compatibles; metadata y red siguen costando. |
| CPU host → GPU | Sí normalmente | PCIe/UMA transfer; reutilizar buffers/pinned memory y mantener datos residentes. |

Patrón preferido para resultados conocidos:

```rust
let mut values = vec![0.0_f64; expected_len];
cpp.price_batch(inputs, &mut values)?;
// values ya es el resultado Rust; no hay std::vector intermedio.
```

Si el tamaño solo lo conoce C++, ofrecer primero una operación barata `result_shape`, o devolver un descriptor de tamaño. Evitar el patrón “C++ crea vector → Rust copia → JSON copia” para cada resultado.

Para matrices enormes que deben sobrevivir a la llamada hay dos alternativas:

1. owner C++ opaco y vistas prestadas en Rust, si el consumidor puede procesarlas antes de liberar;
2. buffer asignado por Rust y relleno por C++, si el resultado debe integrarse en caches/respuestas Rust.

La segunda suele simplificar ownership y observabilidad de memoria.

## 12. Batching, planificación y reutilización

El motor expone operaciones gruesas:

```rust
engine.price_portfolio(...)
engine.run_scenarios(...)
engine.calculate_risk(...)
```

`engine.price(...)` delega a `price_portfolio` con un trade. No se implementan miles de llamadas Rust→C++ desde un loop de aplicación.

### 12.1 Unidad de agrupación

Agrupar por:

- provider/kernel;
- familia y parametrización compatible de modelo;
- layout/schedule de producto;
- market snapshot y dependencias de curva/superficie;
- measures que puedan compartir artefactos;
- conjunto de escenarios;
- target físico.

### 12.2 Grafo de artefactos

El planner construye un DAG interno, no una secuencia de measure names:

```text
validated trades
      │
      ├── cashflow plan ─────────────┐
      ├── market dependencies        │
      └── model factors              │
                  │                  │
                  ▼                  ▼
             shared paths       discount factors
                  │                  │
                  └──────┬───────────┘
                         ▼
                 scenario valuation
                    │     │      │
                    ▼     ▼      ▼
                   PV    EE     PFE
                               │
                               ▼
                         CVA/DVA/FVA...
```

Una petición de EE, PFE y CVA no simula tres veces. Los artefactos cacheables declaran key, bytes, coste de recomputación y lifetime. La cache está acotada por bytes, no por número de entradas.

## 13. API REST

### 13.1 Endpoints v1

```text
POST /v1/context/markets                recibe Context + MarketSpec; devuelve Context + MarketRef
POST /v1/context/models                 recibe Context + ModelSpec; devuelve Context + ModelRef
POST /v1/context/products               recibe Context + ProductSpec; devuelve Context + ProductRef
POST /v1/context/portfolios             recibe Context + PortfolioSpec; devuelve Context + PortfolioRef
POST /v1/prices                         recibe Context + refs; cálculo pequeño/síncrono
POST /v1/portfolios:price               recibe Context + PortfolioRef; sync bajo umbral
POST /v1/grids:price                    recibe Context + trades × models × markets
POST /v1/scenarios:run                  recibe Context + ScenarioRequest; streaming o sync
POST /v1/scenarios:evaluate             evalúa un producto sobre un escenario puntual
POST /v1/risk:calculate                 recibe Context + RiskRequest; streaming o sync
POST /v1/paths:simulate                 simula trayectorias Q/P para diagnóstico o reutilización
POST /v1/calibration:run                calibra un modelo y devuelve ModelSpec/fit metadata
POST /v1/context:apply                  lote de mutaciones tipadas en una sola request
GET  /health/live
GET  /health/ready
GET  /metrics                           según despliegue, no público
```

Estos endpoints no son CRUD de recursos almacenados. Son operaciones funcionales sobre un documento enviado por el cliente: el path ayuda a que el flujo se parezca a los notebooks, pero cada request debe contener `context` y el servidor olvida el resultado al terminar. `context:apply` permite evitar cinco round-trips al construir una sesión inicial.

No se debe añadir `POST /v1/sessions` ni `GET /v1/sessions/{id}` en el modo por defecto. Esos nombres implican estado server-side. Si en el futuro existe un catálogo persistido de mercados/modelos, será otro bounded context y otro contrato, no una optimización silenciosa de este API.

### 13.2 Ejemplo notebook-like en REST

El SDK Python debe ocultar el envelope, pero el wire API debe ser explícito. Ejemplo de construcción paso a paso:

```http
POST /v1/context/markets
Content-Type: application/json

{
  "context": {
    "schema": "quant.context/v1",
    "context_id": "ctx-01H...",
    "revision": 0,
    "parent_hash": null,
    "engine": {"execution": {"device": "auto", "precision": "fp64"}},
    "markets": {}, "models": {}, "products": {}, "portfolios": {},
    "pricing": {"n_paths": 5000, "n_steps": 208, "seed": 7, "measure": "risk_neutral_q"}, "runs": []
  },
  "operation_id": "op-0001",
  "market_id": "eur-2026-09-18",
  "market": {
    "as_of": "2026-09-18",
    "currency": "EUR",
    "discount_curve": {"times": [0.5, 1.0, 2.0], "zero_rates": [0.02, 0.021, 0.023]}
  }
}
```

Respuesta:

```json
{
  "context": {
    "schema": "quant.context/v1",
    "context_id": "ctx-01H...",
    "revision": 1,
    "parent_hash": "sha256:...",
    "context_hash": "sha256:...",
    "engine": {"execution": {"device": "auto", "precision": "fp64"}},
    "markets": {"eur-2026-09-18": {"...": "..."}},
    "models": {}, "products": {}, "portfolios": {},
    "pricing": {"n_paths": 5000, "n_steps": 208, "seed": 7, "measure": "risk_neutral_q"}, "runs": []
  },
  "market_ref": {"id": "eur-2026-09-18", "hash": "sha256:..."}
}
```

El cliente conserva exactamente `context` de la respuesta y lo envía al siguiente endpoint:

```text
POST /v1/context/models   Context₁ + HullWhite1FSpec   → Context₂ + ModelRef
POST /v1/context/products Context₂ + IrSwapSpec       → Context₃ + ProductRef
POST /v1/prices           Context₃ + refs + measures   → Context₃' + PricingResult
```

La interfaz Python equivalente puede ser:

```python
client = QuantRestClient(base_url="https://quant.example")
ctx = qdrest.Context.new(
    engine={"execution": {"device": "auto", "precision": "fp64"}},
    pricing={"n_paths": 5_000, "n_steps": 208, "seed": 7,
             "measure": "risk_neutral_q"},
)
ctx, market = client.add_market(ctx, "eur", market_spec)
ctx, model = client.add_model(ctx, "hw", qdrest.HullWhite1F(a=.1, b=.02, sigma=.01))
ctx, swap = client.add_product(ctx, "swap", qdrest.IRSwap(
    notional=1_000_000, fixed_rate=.025,
    payment_times=[1, 2, 3, 4, 5], accruals=[1, 1, 1, 1, 1],
))
price = client.price(ctx, product=swap, model=model, market=market,
                     measures=[qdrest.PV(), qdrest.DV01()])
```

El SDK devuelve `Context` nuevo en las mutaciones y conserva el mismo contexto semántico en una valoración pura. Si se solicita `record_run=True`, devuelve además `ctx2 = ctx.with_run(price.metadata)`; no incorpora automáticamente los arrays de resultados.

### 13.2.1 Mapa de los notebooks Python al wire API

La compatibilidad buscada es semántica y de nombres, no una traducción literal de objetos Python a objetos vivos del servidor:

| Python/notebook | REST stateless | Datos que se mantienen en `QuantContext` |
|---|---|---|
| `qd.Engine(backend, n_paths, n_steps, seed, pricing_date)` | `Context.new(engine=..., pricing=...)` o `POST /v1/context/config` | `EngineSpec`, `PricingDefaults`, `ExecutionPolicy` |
| `qd.Market(...)` | `POST /v1/context/markets` | `MarketSpec` versionado por `MarketId` |
| `qd.HullWhite1F(...)`, `qd.Heston(...)` | `POST /v1/context/models` | `ModelSpec`, no estado del calibrador |
| `qd.IRSwap(...)`, `qd.Payoff(...)` | `POST /v1/context/products` | `ProductSpec`/Payoff IR canónico |
| `qd.Portfolio([...])` | `POST /v1/context/portfolios` | referencias a productos, cantidades y netting IDs |
| `engine.price(...)` | `POST /v1/prices` | contexto de entrada; resultado fuera o `RunRecord` opcional |
| `engine.price_batch(...)` | `POST /v1/portfolios:price` con batch homogéneo | portfolio y defaults de pricing |
| `engine.price_many(...)` | `POST /v1/portfolios:price` con `grouping="heterogeneous"` | portfolio heterogéneo |
| `engine.price_grid(...)` | `POST /v1/grids:price` | referencias a listas de trades/modelos/markets |
| `engine.all_greeks(...)` | `POST /v1/risk:calculate` con `measure="all_greeks"` | `RiskRequest` opcional y factores |
| `engine.hessian(...)` / `hvp(...)` | `POST /v1/risk:calculate` con `kind="hessian"`/`"hvp"` | selección de factores y vector HVP |
| `engine.simulate_paths(...)` | `POST /v1/paths:simulate` | model/market y `PricingContext`; paths normalmente solo en la respuesta |
| `engine.evaluate_scenario(...)` | `POST /v1/scenarios:evaluate` | producto y schema de observables; scenario puntual en request |
| `engine.calibrate(...)` | `POST /v1/calibration:run` | market, calibrator spec y, si se desea, `ModelSpec` resultante |
| `PricingContext` | `context.pricing` + override `pricing` de la operación | fecha, paths, steps, seed, medida Q/P |
| `ExecutionContext` | `context.engine.execution` + override `execution` | backend, precisión, dispositivo, límites |
| `PriceResult`/`BatchResult`/`GridResult` | `PricingResultDto` con filas/celdas indexadas | no se añade completo al contexto por defecto |

Los endpoints de cálculo admiten `pricing`/`execution` como override puntual, igual que los métodos Python aceptan un contexto opcional. Un override no muta el `QuantContext`; para cambiar defaults se envía `POST /v1/context/config` y se conserva la nueva revisión.

La forma del SDK Python recomendado es deliberadamente fina:

```python
ctx, market = client.add_market(ctx, "eur", qdrest.Market(...))
ctx, model = client.add_model(ctx, "hw", qdrest.HullWhite1F(...))
ctx, trades = client.add_products(ctx, [qdrest.IRSwap(...), qdrest.IRSwap(...)])

batch = client.price_batch(ctx, trades, model=model, market=market,
                           measures=[qdrest.PV(), qdrest.UnilateralCVA()])
grid = client.price_grid(ctx, trades, models=[model], markets=[market, stressed],
                         measures=[qdrest.PV()])
greeks = client.all_greeks(ctx, product=trades[0], model=model, market=market)
```

El cliente Python puede mantener una clase `Context` mutable por conveniencia, pero internamente debe tratar cada respuesta como snapshot nuevo. No debe intentar serializar `_native.Engine`, `UniquePtr`, `Arc` ni handles de C++.

### 13.3 Operaciones y tipos REST

Los DTO se definen en `quant-api::dto` y se mapean explícitamente a tipos de dominio:

```rust
#[derive(Serialize, Deserialize)]
pub struct AddMarketRequest {
    pub context: ContextEnvelopeDto,
    pub operation_id: OperationIdDto,
    pub market_id: String,
    pub market: MarketDto,
}

#[derive(Serialize, Deserialize)]
pub struct PriceRequest {
    pub context: ContextEnvelopeDto,
    pub operation_id: OperationIdDto,
    pub market: ResourceRefDto,
    pub model: ResourceRefDto,
    pub products: Vec<ResourceRefDto>,
    pub pricing: PricingRequestDto,
    pub output: OutputPolicyDto,
}

#[derive(Serialize)]
pub struct PriceResponse {
    pub context: ContextEnvelopeDto,
    pub result: PricingResultDto,
    pub provenance: CalculationProvenanceDto,
}
```

Tipos mínimos del wire contract:

- `ContextEnvelopeDto`: `schema`, `context_id`, `revision`, hashes, encoding y límites.
- `ResourceRefDto`: `id`, `hash`, `kind`, `version`; nunca un puntero/handle de servidor.
- `ContextCommandDto`: unión etiquetada (`add_market`, `add_model`, `add_product`, `add_portfolio`, `record_run`).
- `PricingRequestDto`: measures parametrizadas, seed/deadline/budget y referencias.
- `PricingResultDto`: valores, shapes, units, measure metadata y warnings.
- `CalculationProvenanceDto`: kernel/provider/version, seed, backend efectivo, context hash y timings agregados.

El endpoint `context:apply` recibe una lista ordenada de comandos y devuelve una única revisión nueva. El servidor valida de forma transaccional: si falla el comando 3, no existe una respuesta parcial que el cliente deba adivinar; devuelve errores por índice y el cliente conserva el contexto anterior.

### 13.4 Contrato stateless y límites reales

- Cualquier réplica puede atender cualquier request; no se requieren sticky sessions.
- Reiniciar el proceso no invalida contextos ya guardados por el cliente.
- Un contexto no da acceso a otro contexto: todos los datos necesarios van en el envelope o se resuelven mediante una referencia externa autorizada.
- Los caches server-side son solo aceleradores: una cache miss debe recomputar desde el context recibido.
- `context_hash` protege integridad lógica; autenticación/autorización se resuelve con credenciales del request, no guardando una sesión cuantitativa.
- El servidor impone `max_context_bytes`, `max_revision`, `max_resources`, `max_inline_array_elements` y `max_runs` para impedir crecimiento no acotado.
- Contextos con PII o curvas sensibles deben poder viajar cifrados; firmar no oculta contenido.
- El servidor debe rechazar `context_id`/hashes mal formados y recalcular el hash canónico, no aceptarlos como verdad.

### 13.5 Sesiones largas y trabajos asíncronos

La semántica stateless entra en tensión con el endpoint tradicional `POST → 202 → GET /jobs/{id}`. Ese GET requiere guardar el job y su resultado. Hay tres opciones, que no deben mezclarse sin declararlo:

1. **Síncrono o streaming mientras vive la conexión**: recomendado para la primera versión. El cliente conserva el contexto y recibe el resultado; si pierde la conexión, repite y puede reutilizar el mismo `operation_id` localmente.
2. **Continuación transportada por el cliente**: el servidor devuelve un checkpoint/continuation token autocontenido y el cliente lo vuelve a enviar. Solo es posible si el cálculo es reanudable por chunks y el token no contiene memoria prohibitiva.
3. **Job store explícito**: añadir una infraestructura stateful (DB/Redis/object store) para `job_id`, resultados y cancelación. Debe ser un modo opcional y documentado, no el comportamiento base.

En modo stateless no se ofrecen `GET /v1/jobs/{id}` ni cancelación durable. La cancelación durante una conexión puede ser cooperativa; una cancelación posterior requiere que el cliente reenvíe el contexto y un checkpoint o aceptar que una ejecución ya iniciada termine.

### 13.6 Compresión, empaquetado y large data

El contexto debe tener una representación canónica para hashing, pero no necesariamente una representación textual en cada request:

```text
QuantContext (typed Rust/Python)
  → canonical form
  → JSON (debug) | CBOR/MessagePack + zstd (production)
  → HTTP Content-Encoding / media type
```

Para curvas/superficies/matrices grandes:

- el contexto contiene metadata, shape, dtype, units y hash;
- los bytes se envían en la misma request como parte binaria Arrow IPC/multipart;
- el servidor valida el hash y construye views temporales;
- la respuesta no copia el buffer a JSON.

No se debe enviar una lista Python/JSON de millones de doubles en cada operación. Si el cliente realmente no puede reenviar el dataset, hace falta un almacenamiento externo o una sesión server-side; ambas cosas son una decisión explícita contra el modo puramente stateless.

### 13.7 Compatibilidad y contrato HTTP

- Versionar en path mayor (`/v1`) y en schemas/IR internos.
- `operation_id` idempotente dentro del contexto; soportar `Idempotency-Key` como protección adicional de transporte. La idempotencia evita efectos duplicados lógicos, pero no obliga al servidor a conservar resultados entre requests.
- Importes, monedas, calendarios, fechas y convenciones explícitos; no depender de defaults silenciosos.
- Números no finitos no viajan en JSON estándar: decidir rechazo o representación de error, nunca strings ad hoc.
- Arrays pequeños pueden ser JSON; outputs grandes usan content negotiation (`application/vnd.apache.arrow.stream`) en la misma respuesta o un destino externo elegido por el cliente. “Descargar como artefacto” requiere almacenamiento y no forma parte del modo stateless base.
- Respuesta de error `application/problem+json` basada en RFC 9457, con `code`, `trace_id` y detalles de campo estables.
- Límites de tamaño y coste se comunican antes del cálculo.

### 13.8 Seguridad y aislamiento de recursos

- Límite de body antes de parsear por completo.
- Checked arithmetic para shapes y bytes.
- Máximos por tenant/API key: concurrencia, queue cost, memoria, paths y output.
- No aceptar nombres de librería, paths o símbolos C++ desde el request.
- Model/provider allowlist; no carga dinámica arbitraria.
- Los contextos de un tenant no pueden obtener resultados/cache keys de otro.
- Deshabilitar detalles internos de C++ en errores externos.

## 14. REST, gRPC y formatos de datos

### 14.1 JSON

Ruta inicial:

```text
HTTP body → bytes → serde_json/DTO → validación → Command → domain/plan
```

JSON es correcto para control plane, definiciones de trades, parámetros, resultados pequeños y depuración humana. No debe existir dentro del motor ni cruzar C++. Tampoco conviene derivar `Serialize/Deserialize` indiscriminadamente en tipos de dominio: eso acopla el contrato externo con la representación interna. `quant-api::dto` realiza el mapping explícito.

`serde_json` es el baseline. Antes de adoptar `simd-json`, medir payloads reales incluyendo la conversión a dominio y allocations; acelerar solo el tokenizer puede no cambiar la latencia total.

### 14.2 gRPC/Tonic

Tonic tiene sentido cuando exista al menos uno:

- clientes service-to-service que requieran stubs y evolución Protobuf;
- streaming bidireccional real;
- integración con infraestructura gRPC ya desplegada;
- necesidad medida de reducir el coste de JSON en mensajes moderados.

No tiene sentido añadirlo “por si acaso”. gRPC no elimina serialización, copias, límites de mensaje ni transferencia de red; Excel tampoco se beneficia naturalmente. Si se añade:

- `quant-grpc` es otro adapter que llama al mismo `PricingService`;
- proto messages nunca llegan a `quant-domain`;
- REST y gRPC comparten semántica, error codes y tests contractuales;
- large results se transmiten por chunks con backpressure, sin construir dos copias completas;
- no se crea una segunda implementación del engine.

### 14.3 Protobuf, FlatBuffers, Arrow y Arrow Flight

| Tecnología | Usar cuando | No usar para |
|---|---|---|
| Protobuf | RPCs tipados, schemas evolutivos, mensajes de control y arrays moderados | “Zero-copy” de matrices gigantes o acceso columnar analítico |
| FlatBuffers | Mensajes persistidos/mmap o acceso aleatorio sin unpacking cuando el schema está muy estable | API v1 sin benchmark; dominio rico que cambia rápidamente |
| Arrow IPC | Curvas/tablas/escenarios/resultados columnares grandes entre Python/Rust/C++ | Comandos de negocio pequeños o árboles de producto complejos |
| Arrow Flight | Servicio remoto intensivo en datasets Arrow, streaming y catálogo/DoGet/DoPut | Primer endpoint REST o sustituto automático de gRPC |

Secuencia recomendada:

1. REST/JSON para comandos y outputs pequeños.
2. Arrow IPC stream como media type del mismo HTTP para resultados tabulares grandes.
3. Protobuf/Tonic si hay un consumidor concreto.
4. Flight solo si el patrón dominante ya es mover datasets Arrow y sus operaciones justifican otro servicio/protocolo.

FlatBuffers se considera mediante benchmark contra Arrow/Protobuf. No se añade como cuarta representación preventiva.

## 15. Estrategia de errores end-to-end

```rust
#[derive(Debug, thiserror::Error)]
pub enum QuantError {
    #[error("invalid request: {message}")]
    Validation { code: &'static str, message: String, field: Option<String> },
    #[error("resource not found")]
    NotFound { kind: &'static str, id: String },
    #[error("unsupported combination: {reason}")]
    Unsupported { reason: String },
    #[error("resource limit exceeded: {resource}")]
    ResourceExhausted { resource: &'static str, limit: u64 },
    #[error("deadline exceeded")]
    DeadlineExceeded,
    #[error("cancelled")]
    Cancelled,
    #[error("numerical failure: {code}")]
    Numerical { code: &'static str, detail: String },
    #[error("legacy backend {provider}: {code}")]
    Legacy { provider: String, code: u32, message: String },
    #[error("internal error")]
    Internal { error_id: uuid::Uuid },
}
```

Mapeo REST orientativo:

| `QuantError` | HTTP | Reintentar |
|---|---:|---|
| Validation | 400/422 | No, salvo corregir input |
| NotFound | 404 | No |
| Unsupported | 422 | No |
| ResourceExhausted | 429 si cuota; 503 si capacidad temporal | Sí, con backoff |
| DeadlineExceeded | 504 | Tal vez |
| Cancelled | 409 o 499 interno de observabilidad | No automático |
| Numerical | 422 si datos; 500 si bug | Depende del código |
| Legacy/Internal | 500 | Según idempotencia |

Reglas:

- El código estable manda; el texto es diagnóstico.
- Errores internos tienen `error_id`/trace ID, sin stack o parámetros sensibles.
- Los resultados parciales solo se devuelven si el contrato los modela explícitamente con estado por fila; nunca como éxito silencioso.
- Un panic de worker se captura en el límite del job cuando sea seguro, se marca como `Internal` y se considera alerta. No se intenta recuperar si pudo corromper estado C++ compartido.

## 16. Observabilidad de rendimiento

### 16.1 Métricas obligatorias

Histogramas:

- `http_request_duration_seconds{route,status_class}`: p50/p95/p99.
- `request_decode_duration_seconds{format}`.
- `engine_plan_duration_seconds{operation}`.
- `engine_queue_duration_seconds{pool}`.
- `engine_compute_duration_seconds{kernel,provider,device}`.
- `engine_bridge_duration_seconds{operation}`.
- `response_encode_duration_seconds{format}`.
- `mc_simulation_duration_seconds{model,device}`.
- `kernel_duration_seconds{kernel,device}`.
- `gpu_transfer_duration_seconds{direction,device}`.

Gauges/counters:

- queue depth, in-flight jobs y rechazos;
- bytes estimados/residentes de cache;
- bytes input/output/host↔device;
- paths, scenarios y trades procesados;
- cancelaciones y deadlines excedidos;
- cache hit/miss por tipo de artefacto;
- errores por código/provider;
- workers activos/ociosos;
- fallback GPU→CPU y causa.

No usar `trade_id`, `portfolio_id`, `market_id` ni trace ID como labels de métricas: cardinalidad explosiva. Pueden vivir en traces/logs muestreados.

### 16.2 Trazas

Span tree mínimo:

```text
HTTP request
 ├─ decode_validate
 ├─ compile_plan
 ├─ queue_wait
 ├─ execute_batch
 │   ├─ rust_kernel | cpp_bridge
 │   ├─ monte_carlo
 │   └─ reductions
 └─ encode_response
```

No crear un span por path, trade o escenario. Para un batch, emitir counts/bytes como campos. OpenTelemetry se inicializa en `quant-api`; el dominio no depende del SDK. El exporter nunca bloquea el hot path y tiene cola/límites propios.

### 16.3 Allocations, CPU, memoria y contención

- Contar allocations/op en benchmarks con `dhat` o un allocator instrumentado solo en perfil de test.
- Linux: `cargo flamegraph`/`perf`, `heaptrack`, contadores de hardware y, si procede, eBPF.
- Windows: ETW/WPA, Visual Studio Profiler o VTune para el stack mixto.
- Generar símbolos de Rust y C++ en builds de profiling; conservar frame pointers donde lo requiera la herramienta.
- Medir mutex wait, Rayon steals, queue wait y threads activos.
- Para C++/Rust mixto, perfilar el proceso completo; un flamegraph solo de Rust ocultaría el coste legacy.
- No cambiar el allocator global hasta tener evidencia. Evaluar mimalloc/jemalloc por plataforma con benchmark de servicio, no microbenchmark aislado.

### 16.4 Bridge overhead

Instrumentar en ambos lados de la llamada:

```text
Rust adapter total
  = validation/layout
  + cxx call
  + C++ shim
  + legacy kernel
  + result mapping
```

El “overhead del bridge” no se deduce restando dos ejecuciones con inputs diferentes. Debe usarse el mismo kernel no-op/sum, mismo buffer y mismas optimizaciones.

## 17. Testing y validación

### 17.1 Pirámide

1. **Unit tests de dominio**: invariantes, shapes, convenciones, calendario, IDs, errors.
2. **Unit tests de kernels**: fórmulas cerradas, límites, monotonicidad, conservación y casos degenerados.
3. **Property testing** (`proptest`): no arbitraje/invariantes, serialización DTO, shapes, overflow, agrupación equivalente a ejecución individual.
4. **Integration Rust**: planner + scheduler + Rust provider.
5. **Bridge tests**: ownership, empty slices, grandes buffers, exceptions, concurrencia, destrucción y sanitizers.
6. **Golden pricing tests**: fixtures versionados con inputs, outputs, tolerancias y procedencia del oráculo.
7. **Cross-validation Rust vs C++**: mismo market/model/product/seed; comparar resultados por tolerancia adecuada.
8. **API contract tests**: OpenAPI, Problem Details, límites, idempotencia y compatibilidad v1.
9. **Load/soak tests**: saturación, p99, memoria estable, cancelación, shutdown y cache eviction.
10. **Benchmark regression**: tendencias en hardware fijo, no bloquear PR por ruido de laptop/CI compartida.

### 17.2 Monte Carlo determinista

- Semilla explícita y requerida para pruebas.
- RNG counter-based recomendado para independizar resultados de scheduling: clave derivada de `(seed, scenario_id, path_id, factor_id, step)`.
- Dividir paths no debe reutilizar streams.
- Comparación exacta solo en mismo backend/build cuando esté garantizada; CPU SIMD, FMA y GPU pueden cambiar bits.
- Cross-backend se valida estadísticamente: media, varianza, cuantiles, intervalos y error estándar.
- Los golden MC guardan también `n_paths`, algoritmo RNG y versión.
- Evitar un RNG global de proceso para producción; el lock global actual de tests es señal de una deuda a retirar antes de paralelismo real.

### 17.3 Tolerancias

Toda golden especifica tolerancia absoluta y relativa, unidades y razón. No usar un único epsilon universal. Precios analíticos, AAD, finite differences, MC y GPU tienen presupuestos distintos.

### 17.4 Sanitizers y herramientas mixtas

- Rust: Miri para componentes puros seleccionados; sanitizers nightly donde el pipeline lo permita.
- C++/bridge: ASan/UBSan en Linux, AddressSanitizer en MSVC/Clang-cl; TSan en un target compatible.
- Fuzzing de DTO/shape/bridge con inputs pequeños y límites; el fuzzer no ejecuta millones de paths.
- Valgrind solo donde no haya alternativa más rápida y compatible.

## 18. Compilación Rust + C++

### 18.1 Autoridad de build

El repositorio actual usa CMake + Corrosion para que CMake conduzca Cargo. La nueva API Rust invierte el host. Hay que evitar el ciclo:

```text
Cargo build.rs → CMake top-level → Corrosion → Cargo ...   # prohibido
```

Objetivo: separar un target C++ **leaf** que no invoque Rust.

```text
cpp/legacy/CMakeLists.txt
  └─ quant_legacy STATIC        # modelos/kernels C++ sin Corrosion

quant-cpp/build.rs
  ├─ cxx_build bridge/shim
  ├─ cmake crate → quant_legacy (si es necesario)
  └─ cargo:rustc-link-lib / search
```

Si los sources C++ y dependencias son simples, `cxx-build` puede compilarlos directamente. En este repositorio, CMake ya gestiona simdjson y una superficie C++ amplia, por lo que es más seguro extraer un subproyecto C++ leaf y reutilizar CMake en lugar de duplicar flags/source lists en `build.rs`.

El camino legacy puede seguir usando el top-level CMake/Corrosion durante la migración. Ambos builds consumen el mismo código C++ leaf, pero nunca se invocan recursivamente.

### 18.2 `build.rs` conceptual

```rust
fn main() {
    let dst = cmake::Config::new("../../cpp/legacy")
        .profile("Release")
        .define("QUANT_LEGACY_BUILD_TESTS", "OFF")
        .build();

    cxx_build::bridge("src/bridge.rs")
        .file("../../cpp/bridge/src/quant_cpp_bridge.cpp")
        .include("../../cpp/bridge/include")
        .include("../../cpp/legacy/include")
        .std("c++20")
        .compile("quant-cpp-bridge");

    println!("cargo:rustc-link-search=native={}", dst.join("lib").display());
    println!("cargo:rustc-link-lib=static=quant_legacy");
    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=../../cpp/bridge");
}
```

El estándar puede permanecer C++17 si el legacy lo requiere; `std::span` en el shim exige C++20. Si se mantiene C++17, usar un view `{ptr, len}` interno o el slice de `cxx` directamente. No migrar todo C++ a C++20 solo por `span`.

### 18.3 Linking estático o dinámico

**Estático por defecto** para el servicio:

- despliegue reproducible y un artefacto;
- `cxx` compilado en lockstep;
- menos problemas de búsqueda de DLL/soname;
- posibilidad de LTO dentro de cada mundo, aunque LTO cruzado Rust/C++ no se da por supuesto.

**Dinámico** cuando lo exijan licencia/vendor, tamaño, actualización independiente o múltiples procesos. La ABI C++ no es un contrato estable: envolver la biblioteca dinámica detrás de un C ABI del vendor o de shims versionados. Si el código C++ es inestable o puede abortar/corromper el proceso, la alternativa arquitectónica no es un `SharedPtr`: es aislamiento en un sidecar con IPC/gRPC/Arrow, aceptando coste de serialización.

### 18.4 Perfiles de build

- Release reproducible con dependencias lockeadas.
- `lto = "thin"` y `codegen-units` se deciden por benchmark de build/runtime.
- No activar `panic = "abort"` por rendimiento antes de diseñar recuperación; un panic no puede cruzar FFI.
- Símbolos separados para profiling/crash analysis.
- Features pesadas (`gpu`, `grpc`, C++ providers opcionales) opt-in.
- Matriz CI: Rust puro Linux/Windows; bridge Linux/Windows; GPU compile-check; test GPU en runner dedicado cuando exista.

## 19. Plan incremental por fases

### Fase 0 — Baseline, contratos y presupuestos

**Objetivo**

Medir el sistema actual y congelar fixtures antes de mover ownership. Definir SLOs y tamaños representativos.

**Crates/áreas**

- actuales `engine-core`, `engine-ffi`, `cpp/engine`;
- nuevo directorio `benches/`; documento OpenAPI preliminar.

**Interfaces públicas**

- ninguna nueva estable;
- schemas internos de benchmark `BenchmarkCase` y fixtures versionados.

**Pruebas**

- golden de IRS/Hull-White, payoff Q/P, exposure/CVA y calibración;
- paridad actual Python/C++/Rust;
- inventario de defaults y errores actuales.

**Benchmarks**

- todos los de §21, al menos bridge/copy/batch/MC baseline.

**Riesgos arquitectónicos**

- optimizar casos irreales;
- goldens que consagran bugs;
- hardware de benchmark no controlado.

**Gate de salida**

Dataset pequeño/medio/grande acordado, métricas repetibles y presupuesto de memoria por caso.

### Fase 1 — Vertical REST + Engine mínimo

**Objetivo**

`POST /v1/context/*` y `POST /v1/prices` ejecutan una valoración existente mediante Axum y un adapter Rust. El cliente envía un `QuantContext` completo, recibe una nueva revisión y el servidor no crea sesiones. Sin gRPC, GPU ni CRUD server-side.

**Crates afectados**

- crear `quant-domain`, `quant-engine`, `quant-api`;
- adaptar, no partir todavía, `engine-core`.

**Interfaces públicas**

- `EngineConfig`, `Engine::price`, `PricingService::submit_price`;
- `QuantContext`, `ContextEnvelope`, `ContextHash`, `ContextCommand` y `ResourceRef`;
- `POST /v1/context:apply` para crear market/model/product en un solo round-trip;
- DTO REST v1 mínimo para un IRS/Hull-White/PV;
- `QuantError` y Problem Details.

**Pruebas**

- unit/domain;
- request→response golden y replay con el `context` devuelto;
- dos réplicas/procesos atienden alternativamente la misma cadena de contexto;
- reinicio del servidor no invalida el siguiente request;
- body/resource limits;
- health/readiness;
- cancellation antes de ejecución.

**Benchmarks**

- Axum mock vs engine directo;
- decode/encode separado de compute;
- tamaño JSON vs packed context y coste de hash/compresión;
- p50/p95/p99 con 1, 8 y saturación de concurrencia.

**Riesgos**

- mezclar DTO con dominio;
- ejecutar cálculo en Tokio;
- crear estado de sesión no replicable;
- devolver el contexto completo sin límites y provocar payloads crecientes.

**Gate**

Overhead HTTP medido y descompuesto; Tokio mantiene latencia de health bajo carga de pricing.

### Fase 2 — Market, Model, Product y planificación

**Objetivo**

Introducir specs/handles internos, registry de providers, capability matching y `ExecutionPlan`, materializados desde un `QuantContext` client-owned. Mantener `price` como wrapper de batch uno.

**Crates afectados**

- `quant-domain`, `quant-engine`, adapter de `engine-core`, `quant-api`.

**Interfaces públicas**

- `MarketSpec/Snapshot/Handle`, `ModelSpec/Handle`, `ProductSpec/Handle`;
- `QuantContext` persistente, mutaciones tipadas y hashes canónicos;
- `PricingContext`, `ExecutionPolicy`;
- `PricingKernel`, `KernelDescriptor`, `CompiledBatch`.

**Pruebas**

- combinaciones compatibles/incompatibles;
- contexto serializado/deserializado preserva hashes y referencias;
- ramas de contexto no se mezclan silenciosamente;
- hashes/versiones de snapshots;
- planner determinista;
- inline specs frente a IDs producen el mismo plan.

**Benchmarks**

- coste de compile plan y cache hit/miss;
- handle/Arc frente a copias de market grande;
- batch uno frente a API anterior.

**Riesgos**

- registry excesivamente dinámico;
- enums gigantes sin escape para providers;
- caching incorrecto por omitir versión/contexto.

**Gate**

El hot path no usa string lookup/downcast por trade; plan y resultados reproducibles.

### Fase 3 — Bridge C++ invertido y adapters legacy

**Objetivo**

Rust host llama a C++ con `cxx`. Migrar un modelo legacy real y demostrar inputs prestados + output preasignado.

**Crates afectados**

- crear `quant-cpp`; extraer target C++ leaf y shim;
- `quant-engine` para provider registration;
- CI/build scripts.

**Interfaces públicas**

- `CppKernelAdapter` implementa `PricingKernel`;
- bridge versionado con `CppStatus`, shapes y slices;
- ningún header legacy expuesto a Rust salvo shim.

**Pruebas**

- empty/unaligned/large slices;
- lifetime/destrucción;
- excepción por categoría;
- concurrencia según thread-safety declarada;
- ASan/UBSan;
- cross-validation Rust/C++.

**Benchmarks**

- no-op/scalar/slice/batch bridge;
- slice prestado vs copia a `std::vector`;
- output Rust prealloc vs C++ vector + copia;
- punto de equilibrio de batch.

**Riesgos**

- ciclo CMake/Cargo;
- falsa suposición `Send/Sync`;
- excepciones no capturadas;
- ABI/config distinta por plataforma.

**Gate**

Sin serialización; cero copia de inputs numéricos; coste de bridge amortizado bajo el umbral acordado.

### Fase 4 — Portfolio y escenarios

**Objetivo**

Pricing heterogéneo agrupado y escenarios compartidos usando `QuantContext`, con respuesta síncrona o streaming mientras vive la conexión. Introducir netting/collateral IDs aunque la lógica XVA completa llegue después. Un job store duradero queda fuera del modo stateless y se trata como extensión explícita.

**Crates afectados**

- `quant-domain::portfolio/scenario`;
- planner/scheduler/cache de `quant-engine`;
- providers Rust/C++;
- endpoints de streaming y continuation tokens; job store solo como feature stateful explícita.

**Interfaces públicas**

- `Portfolio`, `Trade`, `ScenarioSet`;
- `price_portfolio`, `run_scenarios`;
- `ContextCommand::add_portfolio` y `ContextCommand::record_run`;
- streaming/backpressure, `operation_id` e idempotencia lógica;
- continuation token solo para kernels reanudables.

**Pruebas**

- batch = concatenación lógica de singles;
- contexto enviado a una segunda réplica produce el mismo resultado;
- orden estable de resultados;
- sharing de escenarios;
- partial failures explícitos;
- cancelación de conexión y eviction de caches no autoritativas.

**Benchmarks**

- 1/10/1k/100k trades;
- 1/10/1k escenarios;
- grouping time, cache hit, memoria pico y bytes copiados;
- comparación miles de crossings vs uno batch.

**Riesgos**

- explosión cartesiana materializada;
- output mayor que RAM;
- cache sin límites;
- confundir `202 + GET /jobs` con una API stateless;
- semántica de netting añadida demasiado tarde.

**Gate**

Trabajo se procesa por chunks/batches acotados; no hay multiplicación accidental de memoria.

### Fase 5 — Risk y Greeks

**Objetivo**

Grafo de measures/artefactos, bump-and-revalue compartido, AAD donde proceda y resultados densos indexados.

**Crates afectados**

- módulos `risk` en `quant-engine` y `quant-compute`;
- adapters C++ si existe riesgo legacy.

**Interfaces públicas**

- `RiskRequest`, `RiskMeasure`, `RiskFactor`, `RiskResult`;
- `calculate_risk`;
- provenance numérica por measure.

**Pruebas**

- AAD vs bump;
- bump central/one-sided;
- invariantes y bucket aggregation;
- reutilización de base valuation;
- tolerancias por factor.

**Benchmarks**

- AAD vs N bumps;
- shared graph vs ejecución measure-by-measure;
- memoria del tape y de matrices de riesgo.

**Riesgos**

- un único trait monolítico de measure;
- tapes AAD enormes;
- reutilizar artefactos con contextos incompatibles.

**Gate**

Greeks validadas y coste incremental visible; requests combinadas no repiten simulación innecesaria.

### Fase 6 — Monte Carlo paralelo y scheduler de producción

**Objetivo**

Sustituir `spawn_blocking` como solución general por pools fijos, RNG particionable, chunks, cancelación y control de oversubscription.

**Crates afectados**

- `quant-engine::scheduler`;
- `quant-compute::mc`;
- adapters C++ y deployment config.

**Interfaces públicas**

- `JobCost`, `ExecutionControl`, progreso agregado;
- políticas de threads/memoria;
- RNG versionado.

**Pruebas**

- misma semilla bajo distinto scheduling;
- streams no solapados;
- queue fairness;
- overload/load shedding;
- shutdown y cancelación por chunk.

**Benchmarks**

- scaling 1..N cores;
- nested parallelism off/on;
- chunk size;
- throughput y p99 bajo mezcla small/large;
- bandwidth, LLC misses y NUMA counters.

**Riesgos**

- starvation de jobs cortos;
- global RNG/locks;
- oversubscription BLAS/OpenMP/Burn/Rayon;
- falsa reproducibilidad bitwise cross-device.

**Gate**

Curva de scaling y saturación entendida; memoria y p99 permanecen acotados.

### Fase 7 — SIMD CPU

**Objetivo**

Optimizar kernels dominantes identificados, no “vectorizar la arquitectura”. Runtime feature detection y fallback correcto.

**Crates afectados**

- `quant-compute::simd/cpu`; posibles shims C++ SIMD si ganan.

**Interfaces públicas**

- ninguna de dominio;
- `CpuVectorPolicy` de application ya existente.

**Pruebas**

- scalar vs SIMD por ULP/tolerancia;
- tails y longitudes pequeñas;
- NaN/Inf;
- CPUs sin feature.

**Benchmarks**

- kernel aislado y end-to-end;
- aligned/unaligned;
- AoS vs SoA;
- auto-vectorizer vs intrinsics/portable SIMD.

**Riesgos**

- regresión en lotes pequeños;
- cambios numéricos por FMA;
- código específico difícil de mantener;
- bandwidth-bound sin mejora real.

**Gate**

Mejora end-to-end material en workload objetivo; fallback y paridad cubiertos.

### Fase 8 — GPU

**Objetivo**

GPU solo para kernels/batches con beneficio neto incluyendo transfers y warm-up. Gestionar residencia y memoria por dispositivo.

**Crates afectados**

- feature `gpu` en `quant-compute` (Burn WGPU actual, CUDA a evaluar);
- scheduler GPU y métricas.

**Interfaces públicas**

- `DevicePreference::Gpu`, device selection y error/fallback policy;
- no hay cambios en productos/modelos.

**Pruebas**

- CPU vs GPU estadístico/numérico;
- OOM controlado;
- device unavailable;
- varios jobs/dispositivos;
- warm/cold start.

**Benchmarks**

- H2D/kernel/D2H separados;
- break-even por batch;
- data residency y reuse;
- throughput concurrente y memoria máxima.

**Riesgos**

- GPU más lenta en casos pequeños;
- f64 pobre/limitado según hardware;
- dependencia/compilación pesada;
- OOM o colas que bloquean.

**Gate**

Tabla de routing Auto basada en datos; fallback explícito, nunca silencioso si cambia precisión/semántica.

### Fase 9 — XVA

**Objetivo**

Extender portfolio/netting/collateral a EE/PFE/CVA/DVA/FVA/MVA/KVA con DAG compartido y lineage completo.

**Crates afectados**

- módulos `xva` en domain/engine/compute; extraer `quant-xva` solo si ya existe una frontera real.

**Interfaces públicas**

- `NettingSet`, `CollateralAgreement`, funding/default specs;
- `XvaRequest`, `XvaResult`, exposure cubes referenciados como artefactos.

**Pruebas**

- casos analíticos/simplificados;
- invariantes (hazard cero, recovery límites, simetrías aplicables);
- golden y validación independiente;
- netting/collateral;
- stress y determinismo estadístico.

**Benchmarks**

- exposure cube materializado vs streaming/reduction;
- sharing entre XVAs;
- portfolio/netting scaling;
- memoria, spill y output Arrow.

**Riesgos**

- intentar representar cubos completos siempre;
- semántica incorrecta de netting/collateral;
- mezclar Q y P;
- extraer crates antes de estabilizar el modelo.

**Gate**

Validación cuantitativa independiente y memoria acotada por diseño; las measures reutilizan el mismo exposure pipeline.

## 20. Decisiones arquitectónicas y trade-offs

Estas decisiones deben convertirse en ADRs breves cuando se acepten:

| ADR | Decisión recomendada | Trade-off aceptado |
|---|---|---|
| 001 | Axum/Tower sobre Actix para v1 | Se prioriza coherencia del stack sobre perseguir diferencias marginales de router. |
| 002 | Rust composition root; C++ como provider legacy | Migración y build más complejos temporalmente; reduce duplicación futura de orquestación. |
| 003 | Cinco crates principales, módulos para el resto | Menos aislamiento inicial a cambio de menos ceremonia/ciclos. |
| 004 | Specs/enums + kernel registry; no doble `dyn` model/product | Extensiones externas usan provider descriptors, no implementan cualquier combinación arbitraria. |
| 005 | Dispatch por batch | Algo más de planificación y buffers contiguos a cambio de hot loops estáticos. |
| 006 | Tokio I/O; pool CPU fijo | Hay que operar dos recursos/pools, pero se protege la latencia async. |
| 007 | SIMD como política CPU | API más exacta; algo más de estructura en `ExecutionPolicy`. |
| 008 | JSON solo en edge | Mapping DTO explícito y repetitivo; dominio no queda contaminado. |
| 009 | gRPC diferido | No hay stubs gRPC iniciales; se evita mantener dos transports sin cliente. |
| 010 | Arrow IPC antes de Flight | Menos features de servicio, menor coste operativo para validar el formato. |
| 011 | Buffers output asignados por Rust | A veces exige consulta previa de shape; simplifica ownership y elimina copia. |
| 012 | Linking estático del legacy por defecto | Binario mayor; despliegue y compatibilidad más simples. |
| 013 | Cancelación cooperativa | No ofrece preemption dura de C++/GPU; evita matar procesos o corromper estado. |
| 014 | No reescribir C++ sin benchmark/paridad | Arquitectura temporalmente híbrida; reduce riesgo de regresión cuantitativa. |
| 015 | `QuantContext` client-owned e inmutable | Se repite contexto o se comprime/empaqueta; el cliente asume la responsabilidad de conservarlo. |
| 016 | Sin `GET /jobs/{id}` en stateless base | Trabajos largos deben ser sync/streaming, reanudables con token o usar un job store explícito. |

### Qué debe vivir en Rust

- API REST/gRPC y DTO mapping.
- Auth/admission/backpressure/jobs.
- Specs, IDs, resultados y errores canónicos.
- Registries y capability matching nuevos.
- Planner/DAG, batching, caching y scheduling.
- Nuevos kernels cuando Rust ofrezca igual o mejor mantenibilidad/rendimiento.
- Monte Carlo, AAD, reductions y backends ya presentes que den paridad.
- Observabilidad y configuración de ejecución.

### Qué puede seguir en C++

- Modelos legacy validados y costosos de portar.
- Integraciones QuantLib/vendor y algoritmos con fuerte dependencia C++.
- Kernels que midan mejor o para los que no haya aún sustituto Rust.
- C ABI, Python/Excel y registries actuales durante transición.
- Builders legacy que se adapten una vez a formatos planos.

### Qué no debe seguir creciendo en C++ para la API nueva

- Routing, jobs, auth, cuotas, scheduling y observabilidad del servicio.
- Nuevos DTO JSON o protocolos de red.
- Otra copia del planner de portfolio/scenarios.
- Nuevos árboles de clases que deban cruzar el bridge.

## 21. Primeros benchmarks antes de continuar

Estos benchmarks son requisito de Fase 0, no trabajo de optimización posterior.

### 21.1 Matriz de datasets

Definir al menos:

| Caso | Trades | Escenarios | Paths × steps | Datos de mercado | Propósito |
|---|---:|---:|---:|---:|---|
| S | 1 | 1 | 0 o 10k×50 | KB | latencia interactiva |
| M | 1k | 100 | 100k×250 | MB | servicio normal |
| L | 100k | 1k | definido por chunks | 100MB+ | capacidad/memoria |

No ejecutar el caso L materializando el producto cartesiano completo; precisamente debe validar chunking.

### 21.2 Benchmark 1 — coste base del cálculo actual

Medir directo, sin HTTP:

- PV analítico;
- portfolio homogéneo y heterogéneo;
- Monte Carlo/exposure;
- payoff Q y P;
- C++ orchestration actual frente a Rust core directo.

Reportar tiempo, allocations, RSS pico, CPU, threads y bytes de output.

### 21.3 Benchmark 2 — `cxx` y copias

Casos:

- función no-op y escalar;
- sum de `&[f64]` de 0, 8, 1K, 1M y 100M elementos;
- Rust slice→span;
- Rust slice→`std::vector` con copia;
- C++ vector→Rust copy;
- Rust preallocated output;
- una llamada por item vs una por batch.

Resultado esperado: determinar el tamaño mínimo de lote para que bridge+dispatch sea <1% del tiempo total. El 1% es un objetivo inicial, no dogma; si el kernel es de microsegundos quizá convenga mantenerlo en Rust o fusionar operaciones.

### 21.4 Benchmark 3 — serialización

Para los mismos payloads:

- serde_json parse/encode;
- Protobuf packed doubles;
- Arrow IPC stream;
- FlatBuffers solo si hay prototipo real.

Medir wall time, CPU, bytes wire, allocations y memoria pico. Separar generación de datos, encoding, transporte loopback y decoding.

### 21.5 Benchmark 4 — batching y reutilización

- `N × price(single)` frente a `price_portfolio(N)`;
- EE+PFE+CVA separados frente a DAG compartido;
- scenarios por crossing frente a matrix batch;
- cache cold/warm.

Medir speedup, copies, paths generados y memoria.

### 21.6 Benchmark 5 — scheduler y colas

Mezcla 90% jobs cortos / 10% largos y otras distribuciones. Medir throughput, p50/p95/p99, queue time, fairness, rechazo y latencia de `/health/ready`. Comparar:

- cálculo accidental dentro de Tokio como control negativo;
- `spawn_blocking` + semaphore;
- pool Rayon fijo;
- workers dedicados.

### 21.7 Benchmark 6 — Monte Carlo scaling

- 1, 2, 4, ..., N cores;
- varios chunk sizes;
- RNG y reduction costs;
- nested parallelism;
- memory bandwidth/LLC misses;
- misma semilla y tests estadísticos.

La meta no es 100% de scaling ideal, sino localizar el techo y su causa.

### 21.8 Benchmark 7 — HTTP real

Tres kernels simulados: 0 ms, 1 ms y 100 ms, además de un caso Quant real. Comparar Axum y, solo si fuese necesario, Actix. Medir con conexiones persistentes, HTTP/1.1 y HTTP/2 relevantes, payload real y tracing en modo producción. Si la diferencia de router desaparece al añadir cálculo/JSON, la decisión queda cerrada.

### 21.9 Benchmark 8 — Context client-owned y stateless

Medir la sesión completa del SDK:

- cinco mutaciones individuales frente a `context:apply`;
- contexto JSON inline frente a CBOR/MessagePack + zstd;
- hash/canonicalización y compresión por revisión;
- payload acumulado con 1, 10, 100 y 1.000 recursos;
- replay alternando dos réplicas/procesos y después de reiniciar el servidor;
- ramas concurrentes, respuestas fuera de orden e `operation_id` repetido;
- mercados pequeños frente a superficies Arrow en multipart.

Reportar bytes enviados por operación, allocations de decode, tiempo de reconstrucción de handles, memoria temporal y coste de una cache miss. Este benchmark decide qué parte del contexto se devuelve completa, cuándo se usa encoding packed y cuál es el límite de `max_context_bytes`.

### 21.10 Benchmark 9 — GPU break-even

No antes de Fase 8:

- cold/warm initialization;
- H2D, kernel y D2H por separado;
- lotes crecientes;
- datos residentes vs no residentes;
- f64 real en hardware objetivo.

`Auto` solo usa GPU por encima del break-even observado con margen.

### 21.11 Umbrales que deben fijarse con negocio/operación

- SLO p95/p99 por operación y tamaño.
- máxima espera en cola.
- máximo RSS/VRAM por worker.
- throughput mínimo de portfolio/scenarios.
- tolerancia numérica por measure/backend.
- presupuesto de overhead de transporte/bridge.
- política de rechazo/fallback y coste por request.

Sin estos umbrales, “alto rendimiento” no es verificable.

## 22. Definition of Done de la arquitectura inicial

La arquitectura v1 está lista para crecer cuando:

- REST ejecuta una vertical real sin bloquear Tokio.
- La misma cadena `QuantContext` puede alternar réplicas y sobrevivir al reinicio del servidor.
- No existe un registry de sesiones necesario para reconstruir market/model/product.
- Los DTO no se filtran a domain/compute.
- Existe una cola acotada y límites de memoria/coste.
- Rust y C++ implementan el mismo SPI de kernels o adapters equivalentes.
- Un batch cruza `cxx` una vez y usa slices/buffer output sin copia evitable.
- Errores C++ llegan como `QuantError` con códigos estables.
- p50/p95/p99 se pueden descomponer en decode, queue, compute, bridge y encode.
- Hay paridad golden Rust/C++ en el caso migrado.
- Los benchmarks baseline se ejecutan en hardware controlado y guardan resultados.
- El contexto tiene hash canónico, límites de tamaño, ramas explícitas y una política documentada para datos grandes.
- gRPC, SIMD y GPU siguen siendo decisiones reversibles hasta que sus gates se cumplan.

## 23. Orden de trabajo inmediato

1. Ejecutar Fase 0 y publicar el informe de benchmark.
2. Escribir ADR-001 a ADR-016 y acordar SLOs, límites de contexto y política de datos grandes.
3. Crear `QuantContext`/DTO/canonical hash en `quant-domain` y `quant-api` alrededor del `engine-core` actual.
4. Implementar `context:apply` + `POST /v1/prices` con el flujo notebook-like y scheduler acotado.
5. Implementar el SDK Python `QuantRestClient` que mantenga el contexto localmente.
6. Añadir portfolio batch del provider Rust.
7. Invertir un único provider C++ mediante `quant-cpp` y medir el bridge.
8. Decidir la siguiente migración usando perfiles, no simetría del diagrama.

## 24. Referencias técnicas verificadas

- CXX describe una frontera sin serialización y con overhead nulo o despreciable para los tipos soportados: <https://cxx.rs/>.
- `UniquePtr`, `SharedPtr`, `CxxVector`, callbacks y traducción de `Result`/excepciones tienen restricciones concretas que deben respetarse: <https://cxx.rs/binding/uniqueptr.html>, <https://cxx.rs/binding/sharedptr.html>, <https://cxx.rs/binding/cxxvector.html>, <https://cxx.rs/binding/fn.html>, <https://cxx.rs/binding/result.html>.
- La integración async directa C++↔Rust todavía no debe asumirse; CXX documenta canales/callbacks como workaround: <https://cxx.rs/async.html>. Este plan mantiene el bridge síncrono dentro de workers.
- Axum usa middleware Tower en vez de un sistema propio: <https://docs.rs/axum/latest/axum/middleware/>.
- Tokio advierte que cálculo CPU dentro de async bloquea progreso, que `spawn_blocking` tiene un límite alto por defecto y que un executor CPU dedicado puede ser más apropiado: <https://docs.rs/tokio/latest/tokio/task/fn.spawn_blocking.html>.
- Tonic implementa servicios gRPC sobre la abstracción Tower, lo que permite añadirlo como adapter sin cambiar aplicación/dominio: <https://docs.rs/tonic/latest/tonic/server/>.

## Conclusión

La arquitectura conceptual inicial es correcta en su dirección general — clientes, transporte, Rust, dominio y compute híbrido — pero tiene demasiadas cajas y dos abstracciones problemáticas: `Simd` como backend hermano de CPU y el doble trait object modelo/producto. La versión recomendada reduce las capas a límites ejecutables: transporte, aplicación/planner, dominio, scheduler y providers de kernel. Rust posee el sistema; C++ aporta capacidad legacy detrás de un bridge pequeño.

El flujo REST debe sentirse como un notebook, pero no debe copiar su estado en el servidor. El notebook remoto es el objeto `QuantContext` que el cliente conserva: `add_market`, `add_model`, `add_product`, `price`, `price_portfolio` y `calculate_risk` son transformaciones de ese documento. La ausencia de estado server-side mejora la escalabilidad y la recuperación ante reinicios, a cambio de repetir o empaquetar el contexto y de renunciar al polling durable de jobs salvo que se añada almacenamiento explícito.

El éxito no depende de que Axum gane microbenchmarks ni de activar GPU pronto. Depende de agrupar trabajo, controlar concurrencia, reutilizar artefactos, mantener buffers en un owner claro, evitar copias en la frontera y medir el pipeline completo. REST es la primera interfaz; batching y memoria son el verdadero diseño de rendimiento.
