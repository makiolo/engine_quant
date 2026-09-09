# XVA Engine — Plan de Arquitectura

> Documento vivo. Se construye de forma incremental, sección a sección.
> Estado: **v0.3 — Fase 0 completada (scaffolding + smoke test end-to-end verificado)**

## 1. Visión

Motor de cálculo XVA (CVA/DVA/FVA/MVA/KVA, exposiciones, sensibilidades) con:

- **Core de cálculo vectorial** en Rust, ejecutable en CPU o GPU, sin acoplarse a un backend concreto.
- **Capa de orquestación** en C++ sobre el core Rust: registro de modelos, productos, medidas y métricas XVA como ciudadanos de primera clase ("first class").
- **Clientes múltiples** consumiendo la misma capa C++ con semántica de API equivalente:
  - Excel (XLL) → C++ directo.
  - Python (nanobind) → C++ directo → uso desde Jupyter.
  - (Opcional, a valorar) API universal de interoperabilidad Rust/C++/otros (FFI estable / gRPC / Arrow Flight / similar).

Objetivo de diseño central: **un producto o modelo nuevo se registra una vez y queda disponible en todos los clientes sin duplicar lógica.**

## 2. Arquitectura en capas

```
┌─────────────────────────────────────────────────────────┐
│  Clientes                                                │
│  ┌───────────────┐  ┌────────────────┐  ┌─────────────┐ │
│  │ Excel (XLL)   │  │ Python (nanobind)│  │ (futuro:    │ │
│  │               │  │  → Jupyter       │  │  API univ.) │ │
│  └───────┬───────┘  └────────┬────────┘  └──────┬──────┘ │
└──────────┼───────────────────┼───────────────────┼───────┘
           │                   │                   │
┌──────────▼───────────────────▼───────────────────▼───────┐
│  Capa C++ (orquestación / dominio)                        │
│  - Registro de modelos, productos, medidas, métricas XVA  │
│  - API pública común (misma semántica en todos los clientes)│
│  - Motor de escenarios / measures / calibración           │
└──────────────────────────┬─────────────────────────────────┘
                            │ FFI
┌──────────────────────────▼─────────────────────────────────┐
│  Core Rust (cálculo vectorial)                              │
│  - Kernels numéricos: analíticos + Monte Carlo               │
│  - Abstracción de backend: CPU (SIMD) / GPU                  │
└───────────────────────────────────────────────────────────┘
```

## 3. Componentes principales

### 3.1 Core Rust
- Motor vectorial de cálculo (analítico + Monte Carlo).
- Abstracción de backend de ejecución (CPU / GPU) — implementación intercambiable.
- Sin conocimiento de "producto" ni "modelo" de negocio: primitivas numéricas (paths, mallas, integración, RNG, reducciones, greeks/AAD si aplica).

### 3.2 Capa C++
- Registro extensible de:
  - **Modelos** (tipos de proceso, calibración).
  - **Productos** (payoffs arbitrarios).
  - **Medidas** (measures: risk-neutral, forward, etc.).
  - **Métricas XVA** (CVA, DVA, FVA, MVA, KVA, exposures, sensibilidades).
- API pública estable que los distintos clientes envuelven sin reimplementar lógica.
- Consume el core Rust vía FFI.

### 3.3 Clientes
- **Excel/XLL**: expone funciones de la API C++ como UDFs.
- **Python/nanobind**: bindings 1:1 (o casi) con la API C++, uso desde Jupyter.
- **(Opcional) API universal**: capa de interoperabilidad para consumidores fuera del ecosistema C++/Python/Excel.

## 4. Principios de diseño

- Un único punto de verdad para la lógica de negocio (capa C++); los clientes son "delgados".
- La API debe sentirse equivalente en Python y en Excel (mismos nombres de concepto, mismos parámetros, mismo modelo mental).
- Extensibilidad de modelos/productos sin tocar el core: patrón de registro (registry) + interfaces bien definidas.
- El backend CPU/GPU debe ser una decisión de configuración, no de diseño del producto/modelo.

## 5. Decisiones abiertas (a resolver de forma incremental)

- [x] **Backend GPU** → Abstracción propia multi-backend (ver §5.1).
- [x] **Alcance de la v1** → IRS + Hull-White 1 factor, exposición vía Monte Carlo (ver §5.2).
- [x] **FFI Rust↔C++** → crate `cxx` (bindings seguros bidireccionales, integración vía `cxx-build`).
- [x] **Build multiplataforma** → CMake como build system principal (C++/XLL/nanobind) + crate `Corrosion` para integrar el build de Cargo dentro de CMake.
- [x] **Sensibilidades** → AAD desde la Fase 1 (ver §5.3). Impacta el diseño del core Rust desde el inicio.
- [x] **Diseño del registry** → registro explícito centralizado (ver §5.4).
- [x] **API universal** → C ABI estable expuesta directamente desde la capa C++ (ver §5.5).
- [x] **Testing/validación numérica** → capas progresivas (ver §5.6).

### 5.1 Compute backend — abstracción propia multi-backend

Se define un trait `ComputeBackend` en el core Rust que encapsula las operaciones vectoriales
necesarias (generación de paths, evaluación de payoffs vectorizada, reducciones/agregaciones,
generación de números aleatorios). Cada backend concreto (CPU/rayon+SIMD, wgpu, CUDA en el
futuro) implementa ese trait. La capa C++ y los clientes nunca hablan con un backend concreto:
seleccionan el backend por configuración (ej. `ComputeBackend::Cpu` vs `ComputeBackend::Gpu(...)`).

Orden de implementación propuesto:
1. Backend **CPU** (rayon + SIMD vía `std::simd` o `wide`) — referencia funcional y de correctitud.
2. Trait `ComputeBackend` estabilizado a partir de las necesidades reales del backend CPU (evitar
   diseñar la interfaz en abstracto antes de tener un caso de uso real).
3. Backend **GPU** (wgpu como primera opción por portabilidad; CUDA queda abierto como backend
   adicional si el rendimiento lo justifica) implementando el mismo trait.

### 5.2 Caso base del prototipo (Fase 0-2)

- **Producto**: Interest Rate Swap (IRS) vanilla.
- **Modelo**: Hull-White de 1 factor para la curva de tipos.
- **Métrica**: perfil de exposición (EE/PFE) vía Monte Carlo → CVA unilateral simple como primera
  métrica XVA end-to-end.
- Sirve como caso de validación para: registry de modelos/productos, FFI Rust↔C++, y equivalencia
  de API entre Python y Excel.

### 5.3 AAD (differenciación automática) desde la Fase 1

Al decidir AAD desde el inicio en lugar de bump-and-reval, el core Rust debe ser **genérico sobre
el tipo numérico escalar** desde el primer kernel (no solo `f64`), de forma que el mismo código de
valoración pueda instanciarse tanto con `f64` (valoración pura) como con un tipo "dual"/tape-based
que propague derivadas (ej. vía un crate de autodiff en modo reverse, tipo `dfdx`/`enzyme`/
implementación propia de tape). Implicaciones sobre el diseño:

- El trait `ComputeBackend` (§5.1) y los kernels numéricos deben parametrizarse por un tipo
  escalar genérico (`T: Float + ...`) en vez de asumir `f64` directamente.
- El payoff de cada producto y la dinámica de cada modelo deben escribirse de forma genérica sobre
  ese tipo, para que el grafo de cómputo sea diferenciable sin reescribir lógica de negocio.
- La elección concreta del mecanismo de AAD (tape propio vs crate existente) queda pendiente y se
  resolverá en la Fase 1, una vez exista el kernel CPU de referencia (§5.1, punto 1) para validar
  contra bump-and-reval como método de contraste numérico.
- Bump-and-reval **no desaparece**: se mantiene como mecanismo de validación cruzada de las
  sensibilidades calculadas vía AAD, no como alternativa de producción.

### 5.4 Registry — registro explícito centralizado

Se evita el patrón de auto-registro estático (macros + inicialización estática global) por su
dificultad de depuración y sus problemas conocidos con linking estático/orden de inicialización.
En su lugar:

- Existe un punto único de arranque (ej. `engine::bootstrap::register_builtins(Registry&)`) que
  registra explícitamente cada modelo, producto y medida disponible: `registry.register_model<HullWhite1F>("HullWhite1F")`, `registry.register_product<IRSwap>("IRSwap")`, etc.
- Los registries de modelos, productos y medidas son estructuras independientes (no un registry
  monolítico), cada una con su propia interfaz base (`IModel`, `IProduct`, `IMeasure`).
- Extender el motor con un modelo/producto nuevo implica: (1) implementar la interfaz
  correspondiente, (2) añadir una línea de registro en el punto central. No requiere tocar el
  resto del motor ni los clientes.
- Este mismo punto central es lo que hace visible, de un vistazo, todo lo que el motor soporta —
  relevante tanto para debugging como para que los clientes (Python/Excel) puedan listar
  dinámicamente modelos/productos/medidas disponibles (ej. `list_models()`).

### 5.5 API universal — C ABI estable (Fase 6)

En vez de un protocolo con servidor/red (gRPC) o un formato especializado en datos columnares
(Arrow Flight), la vía elegida para interoperabilidad externa es exponer una **interfaz `extern
"C"` plana y versionada directamente desde la capa C++** (no desde Rust — Rust ya se consume
internamente vía `cxx`, esta ABI es la frontera pública del motor completo).

- Reutiliza la misma API pública que ya consumen los clientes Python/Excel (§3.2): no es una
  cuarta API distinta, es su expresión en C ABI para lenguajes sin binding dedicado (Julia, .NET,
  Go, etc. vía sus mecanismos nativos de FFI a C).
- Versionado explícito de la ABI (ej. sufijo de versión en símbolos o una función
  `engine_abi_version()`) para permitir evolución sin romper consumidores existentes.
- Los tipos complejos (vectores, curvas, resultados de exposición) se pasan mediante structs
  planos / punteros + longitud, evitando dependencias de serialización de terceros — coherente con
  mantener esta capa mínima y sin dependencias operativas (sin red, sin servidor).
- gRPC o Arrow Flight no se descartan de forma permanente: si en el futuro aparece un consumidor
  real que necesita servicio en red (gRPC) o transferencia masiva de datos vectoriales entre
  procesos (Arrow Flight), se pueden construir **encima** de esta C ABI sin rediseñarla.

### 5.6 Testing y validación numérica — capas progresivas

Cuatro capas de test, cada una con un propósito distinto y aplicándose progresivamente a medida
que el motor gana capacidades (no todas existen desde la Fase 0):

1. **Unit tests deterministas por kernel** (Rust, desde Fase 1) — cada kernel numérico (paths,
   integración, reducciones) se testea con RNG de seed fija, verificando outputs exactos o dentro
   de una tolerancia muy estrecha. Objetivo: detectar regresiones a nivel de primitiva.
2. **Convergencia Monte Carlo vs fórmula cerrada** (desde Fase 1-2) — para todo modelo/producto
   que tenga solución analítica conocida (ej. swap bajo Hull-White, opción bajo Black-Scholes), un
   test compara el resultado Monte Carlo contra el analítico dentro de una tolerancia relativa
   definida por el error estándar esperado (no un número arbitrario: se deriva del número de paths
   y la varianza del estimador). Objetivo: validar correctitud del modelo/producto, no solo del
   kernel.
3. **Consistencia AAD vs bump-and-reval** (desde Fase 1, ligado a §5.3) — toda sensibilidad
   calculada vía AAD se contrasta en tests contra su equivalente por diferencias finitas, dentro de
   una tolerancia acorde al step de bump usado. Objetivo: detectar errores de diferenciación sin
   depender solo de revisión manual.
4. **Equivalencia de API entre clientes** (desde Fase 3-4, cuando existan ambos clientes) — el
   mismo caso (IRS + Hull-White, §5.2) ejecutado desde Python y desde Excel debe producir el mismo
   resultado numérico dentro de tolerancia de precisión de punto flotante. Objetivo: garantizar que
   "la API se siente equivalente" (principio de §4) no es solo una aspiración de diseño sino algo
   verificado automáticamente.

Estas capas se ejecutan en CI de forma incremental: las capas 1-2 ya deben existir antes de cerrar
la Fase 1; la capa 3 antes de cerrar la Fase 1 (coincide con la introducción de AAD); la capa 4 se
añade en cuanto exista más de un cliente.

## 6. Roadmap por fases (borrador, pendiente de detallar)

1. **Fase 0** ✅ — Esqueleto de repos/build: CMake + Corrosion orquestando un workspace Rust mínimo + binding C++ trivial vía `cxx` + smoke test desde Python (ver §7.1, verificado end-to-end).
2. **Fase 1** — Core Rust: kernels genéricos sobre tipo escalar (para AAD, §5.3), backend `ComputeBackend` CPU (rayon/SIMD) + simulación Hull-White 1F + valoración IRS + primer mecanismo de AAD validado contra bump-and-reval.
3. **Fase 2** — Capa C++: registry de modelos/productos/medidas, cálculo de exposición (EE/PFE) y CVA unilateral end-to-end sobre IRS+Hull-White.
4. **Fase 3** — Cliente Python (nanobind) + Jupyter funcional.
5. **Fase 4** — Cliente Excel (XLL).
6. **Fase 5** — Backend GPU (wgpu) implementando `ComputeBackend`.
7. **Fase 6** — API universal / interoperabilidad externa.

## 7. Estructura de repos/carpetas (Fase 0)

Monorepo único (no multi-repo): un solo checkout contiene el core Rust, la capa C++ y todos los
clientes, orquestado desde un `CMakeLists.txt` raíz que usa **Corrosion** para importar el
workspace de Cargo como si fueran targets CMake nativos.

```
engine_quant/
├── CMakeLists.txt              # raíz: corrosion_import_crate(rust/) + add_subdirectory(cpp, clients)
├── cmake/
│   └── modules/                # Corrosion (vendored o FetchContent), FindNanobind, etc.
│
├── rust/
│   ├── Cargo.toml               # workspace manifest
│   └── crates/
│       ├── engine-core/         # kernels numéricos + trait ComputeBackend + tipo escalar genérico
│       │   ├── Cargo.toml
│       │   └── src/lib.rs
│       └── engine-ffi/          # frontera cxx: expone engine-core a C++
│           ├── Cargo.toml
│           ├── build.rs         # cxx_build::bridge(...)
│           └── src/lib.rs       # #[cxx::bridge] mod ffi { ... }
│
├── cpp/
│   └── engine/                  # capa de orquestación C++ (§3.2)
│       ├── include/engine/
│       │   ├── model.hpp        # interfaz IModel
│       │   ├── product.hpp      # interfaz IProduct
│       │   ├── measure.hpp      # interfaz IMeasure
│       │   ├── registry.hpp     # Registry<T> (§5.4)
│       │   └── bootstrap.hpp    # register_builtins(Registry&...)
│       ├── src/
│       │   ├── registry.cpp
│       │   └── bootstrap.cpp
│       └── CMakeLists.txt
│
├── clients/
│   ├── python/                  # binding nanobind (§3.3)
│   │   ├── src/engine_py_ext.cpp
│   │   ├── pyproject.toml
│   │   └── CMakeLists.txt
│   └── excel/                   # XLL (§3.3)
│       ├── src/
│       └── CMakeLists.txt
│
├── tests/
│   └── integration/             # capa 4 de §5.6: equivalencia Python vs Excel sobre el mismo caso
│
├── docs/
│   └── adr/                     # decisiones de arquitectura registradas incrementalmente
│
├── PLAN.md
└── .gitignore
```

Notas de diseño de esta estructura:

- **`engine-core` sin dependencia de `cxx`**: los kernels y el trait `ComputeBackend` son Rust
  puro, testeable con `cargo test` sin tocar C++ (capas 1-2 de §5.6 viven aquí, en
  `rust/crates/engine-core/tests/` o inline con `#[cfg(test)]`).
- **`engine-ffi` como frontera aislada**: todo el código `#[cxx::bridge]` vive en un crate
  separado que depende de `engine-core`. Si el mecanismo de FFI cambiara en el futuro, el impacto
  queda contenido a este crate.
- **`cpp/engine` no conoce Rust directamente**: consume `engine-ffi` a través del header generado
  por `cxx`/Corrosion; el registry, las interfaces `IModel`/`IProduct`/`IMeasure` y el bootstrap
  (§5.4) son C++ puro.
- **Los tests de capas 1-3 (§5.6) viven junto a lo que testean** (Rust: `cargo test`; C++: dentro
  de `cpp/engine` con su propio target de test) — `tests/integration/` en la raíz se reserva solo
  para la capa 4 (cross-cliente), que por definición no pertenece a un único módulo.
- **`clients/excel` y `clients/python` son delgados**: no deberían tener lógica de negocio, solo
  el código de binding/UDF que traduce hacia `cpp/engine`.

### 7.1 Cadena de humo de la Fase 0 (smoke test end-to-end)

Objetivo mínimo de la Fase 0: una única llamada trivial atravesando **todas** las capas, para
validar que el pipeline de build (CMake + Corrosion + cxx + nanobind) funciona de punta a punta
antes de escribir lógica de negocio real.

1. `engine-core`: función trivial, ej. `pub fn ping() -> f64 { 42.0 }`.
2. `engine-ffi`: bridge `#[cxx::bridge]` que expone `ping()` a C++.
3. `cpp/engine`: función `engine::ping()` que llama al bridge (de momento sin registry ni
   interfaces — esas se introducen en Fase 1-2).
4. `clients/python`: módulo nanobind que expone `engine.ping()`.
5. Test de humo en Python (`import engine; assert engine.ping() == 42.0`) ejecutado en CI.

**Estado: verificado end-to-end** (`engine.ping() == 42.0` desde Python, atravesando las 4 capas).

Gotchas encontrados al implementarlo, relevantes para cualquier crate/target que se añada después:

- **Nombres de target**: Corrosion reemplaza guiones por guiones bajos en el nombre del target
  CMake para crates `staticlib`/`cdylib` (el paquete Cargo `engine-ffi` da lugar al target CMake
  `engine_ffi`, no `engine-ffi`). El nombre del paquete Cargo no cambia, solo cómo se referencia
  desde `CMakeLists.txt` (ej. `corrosion_add_cxxbridge(... CRATE engine_ffi ...)`).
- **Include del header generado por `cxx`**: con `corrosion_add_cxxbridge(<cxx_target> CRATE
  <crate> FILES lib.rs)`, el header queda en `<cxx_target>/lib.h` (no `<crate>/src/lib.rs.h`) —
  se nombra por el `cxx_target` elegido, no por la ruta del archivo fuente.
- **Mismatch de CRT en Windows/MSVC**: el `cc`/`cxx-build` que compila el shim C++ generado desde
  `build.rs` no sigue el `CMAKE_BUILD_TYPE` de la parte C++; en concreto, con un `cargo build`
  no-release enlaza con la CRT de release (`/MD`) por defecto, lo que choca (`LNK2038`) contra un
  `CMakeLists.txt` configurado en `Debug` (`/MDd`). Solución para Fase 0: build de CMake en
  **Release**. Revisar en Fase 1 si se necesita build de Debug real (ej. vía
  `corrosion_add_target_local_rustflags` o forzando el profile de cargo).
- El entorno de build en Windows requiere el compilador de MSVC en el `PATH` (cargar
  `vcvars64.bat` / usar una "Developer Command Prompt" antes de invocar `cmake configure`/`build`),
  ya que tanto `cxx-build` (Rust) como CMake/Ninja necesitan `cl.exe`.

Este caso trivial **no** implementa IRS ni Hull-White todavía (eso es Fase 1-2, §5.2) — es
puramente un test de fontanería (plumbing) del pipeline de build multi-lenguaje.

### 7.2 Pendiente antes de escribir código

- [x] **Inicializar el repositorio git** → hecho (ver §7.3).
- [x] **Toolchain de Rust / SIMD** → stable pinneado + crate `wide` (ver §7.3).
- [x] **Dependencias C++ (Corrosion, nanobind, test framework)** → CMake `FetchContent` (ver §7.3).

### 7.3 Decisiones de toolchain resueltas

**Rust: stable + `wide`.** Se fija la versión exacta del toolchain en `rust/rust-toolchain.toml`
(canal `stable`, versión concreta a determinar al arrancar Fase 1 — la más reciente estable en ese
momento). El backend CPU (§5.1) usa la crate `wide` para SIMD portable en vez de `std::simd`
(`portable_simd`, nightly-only): evita atar el proyecto a nightly y a una API todavía inestable,
a costa de un poco menos de control de bajo nivel que `std::simd`. Si en el futuro `portable_simd`
se estabiliza, migrar es un cambio localizado al backend CPU, no a `engine-core` en general (el
trait `ComputeBackend` ya lo aísla).

**C++: CMake `FetchContent`.** El `CMakeLists.txt` raíz trae Corrosion, nanobind y el framework de
test C++ (a decidir en Fase 2, probablemente GoogleTest) vía `FetchContent_Declare(... GIT_TAG
<commit-fijo>)`, cada uno pinneado a un commit/tag exacto — no a una rama móvil. Se prioriza
simplicidad sobre build 100% offline: no hay submódulos que sincronizar manualmente, y el pin
explícito en el propio `CMakeLists.txt` documenta la versión igual que lo haría un submódulo.
Revisar esta decisión si el entorno de CI/desarrollo termina necesitando builds sin acceso a red.

**CMake mínimo**: 3.24 (buen soporte de `FetchContent` moderno y compatible con las versiones
recientes de Corrosion/nanobind en Windows/MSVC).

---
*Próxima iteración: arrancar Fase 1 — kernels genéricos sobre tipo escalar, backend `ComputeBackend`
CPU, simulación Hull-White 1F, valoración IRS y primer mecanismo de AAD (§5.3, §6).*
