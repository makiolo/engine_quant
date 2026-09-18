# ADRs de la API REST — estado de Fase 0

Estos ADRs documentan decisiones iniciales propuestas por `PLAN_API_REST.md`. Son decisiones de arquitectura, no una promesa de que el servidor REST ya exista. Cualquier cambio debe conservar el benchmark/fixture que motivó la revisión y actualizar el estado en el informe de Fase 0.

| ADR | Decisión |
|---|---|
| [ADR-001](ADR-001.md) | Axum/Tower sobre Actix para v1 |
| [ADR-002](ADR-002.md) | Rust composition root; C++ como provider legacy |
| [ADR-003](ADR-003.md) | Cinco crates principales y módulos para el resto |
| [ADR-004](ADR-004.md) | Specs/enums y registry de kernels |
| [ADR-005](ADR-005.md) | Dispatch por batch |
| [ADR-006](ADR-006.md) | Tokio para I/O y pool CPU acotado |
| [ADR-007](ADR-007.md) | SIMD como política CPU |
| [ADR-008](ADR-008.md) | JSON solo en el edge |
| [ADR-009](ADR-009.md) | gRPC diferido |
| [ADR-010](ADR-010.md) | Arrow IPC antes de Flight |
| [ADR-011](ADR-011.md) | Buffers de salida asignados por Rust |
| [ADR-012](ADR-012.md) | Linking estático del legacy por defecto |
| [ADR-013](ADR-013.md) | Cancelación cooperativa |
| [ADR-014](ADR-014.md) | No reescribir C++ sin benchmark y paridad |
| [ADR-015](ADR-015.md) | QuantContext client-owned e inmutable |
| [ADR-016](ADR-016.md) | Sin GET /jobs/{id} en el modo stateless base |
