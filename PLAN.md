# XVA Engine — Plan de Arquitectura

> Documento vivo. Se construye de forma incremental, sección a sección.
> Estado: **v0.13 — Fase 4 completada (cliente Excel vía XLL sobre el registry C++) +
> empaquetado/distribución (wheel Python + XLL autocontenidos, release automática en CI, §7.9)
> + instalador Windows todo-en-uno (wizard .exe, §7.10) + Fase 5 completada (backend GPU
> ejercitado con benchmarks reales, se mantiene opcional, §7.11; seleccionable desde Python
> (`with engine.backend(...)`) y Excel (`ENGINE.SET_BACKEND`), §7.12) + Fase 6: API universal
> en C ABI (`engine/abi.h`), verificada con GoogleTest y una sonda en C puro, aún sin
> distribuirse en ninguna release (§7.13) + Fase 7: `MarketSnapshot`/`ICalibrator` — calibrar
> `HullWhite1F` a una curva de mercado (real o fabricada) en las cinco capas (Rust/C++/C
> ABI/Python/Excel), §7.14**

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

- [x] **Backend GPU** → Framework tensorial [Burn](https://burn.dev), no una abstracción propia (ver §5.1).
- [x] **Alcance de la v1** → IRS + Hull-White 1 factor, exposición vía Monte Carlo (ver §5.2).
- [x] **FFI Rust↔C++** → crate `cxx` (bindings seguros bidireccionales, integración vía `cxx-build`).
- [x] **Build multiplataforma** → CMake como build system principal (C++/XLL/nanobind) + crate `Corrosion` para integrar el build de Cargo dentro de CMake.
- [x] **Sensibilidades** → AAD desde la Fase 1 vía el autodiff en modo reverse de Burn (ver §5.3). Impacta el diseño del core Rust desde el inicio.
- [x] **Diseño del registry** → registro explícito centralizado (ver §5.4).
- [x] **API universal** → C ABI estable expuesta directamente desde la capa C++ (ver §5.5).
- [x] **Testing/validación numérica** → capas progresivas (ver §5.6).

### 5.1 Compute backend — framework tensorial Burn, no una abstracción propia

**Revisado en Fase 1** (ver §7.5): la primera versión de esta sección definía un trait
`ComputeBackend` propio (generación de paths, payoffs vectorizados, reducciones, RNG) con un
backend CPU manual (rayon + SIMD vía `wide`). Se abandona esa abstracción propia en favor de
[Burn](https://burn.dev), un framework tensorial de Rust ya maduro que resuelve exactamente el
mismo problema (cómputo vectorial portable entre CPU y GPU tras un único tipo `Backend` genérico)
sin mantener código propio de bajo nivel:

- Los kernels numéricos y la lógica de valoración se escriben genéricos sobre
  `B: burn::tensor::backend::Backend`, operando sobre `Tensor<B, D>` en vez de sobre un escalar
  `f64`/tipo dual propio (ver §5.3): una trayectoria Monte Carlo completa (todos los paths a la
  vez) es un tensor de forma `[n_paths]`, y el "paralelizar entre paths" (antes responsabilidad de
  `rayon` en el backend CPU manual) pasa a ser responsabilidad de Burn/del backend elegido.
- La capa C++ y los clientes siguen sin hablar nunca con un backend concreto (principio de §4
  intacto): elegir CPU o GPU sigue siendo una decisión de configuración, ahora expresada
  literalmente como qué alias de tipo de Burn se instancia (`CpuBackend = burn::backend::NdArray<f64>`
  vs, tras la feature `gpu` del crate `engine-core`, `GpuBackend = burn::backend::Wgpu<f64>`).
- `f64` como tipo de elemento flotante en ambos backends (Burn usa `f32` por defecto, pensado para
  entrenar redes neuronales): la precisión importa más que el rendimiento en valoración de
  derivados, y este motor nunca entrena nada, solo reutiliza el motor tensorial/autodiff de Burn.

Orden de implementación real (Fase 1):
1. Backend **CPU** vía `burn-ndarray` (feature `ndarray` de Burn) — referencia funcional y de
   correctitud, es la que usan todos los tests de Fase 1.
2. Backend **GPU** vía `burn-wgpu` (feature `wgpu` de Burn, portable: Vulkan/Metal/DX12/WebGPU) tras
   la feature `gpu` de `engine-core`, no compilada por defecto (árbol de dependencias y tiempo de
   compilación considerables) — alias de tipo ya presente en `backend.rs`, ejercitado con
   benchmarks reales en Fase 5 (ver §7.11): sigue opcional, no por defecto. CUDA (`burn-cuda`)
   queda abierto como backend adicional si hiciera falta más rendimiento que wgpu, con el mismo
   cambio de una línea.

### 5.2 Caso base del prototipo (Fase 0-2)

- **Producto**: Interest Rate Swap (IRS) vanilla.
- **Modelo**: Hull-White de 1 factor para la curva de tipos.
- **Métrica**: perfil de exposición (EE/PFE) vía Monte Carlo → CVA unilateral simple como primera
  métrica XVA end-to-end.
- Sirve como caso de validación para: registry de modelos/productos, FFI Rust↔C++, y equivalencia
  de API entre Python y Excel.

### 5.3 AAD (diferenciación automática) desde la Fase 1 — autodiff de Burn

**Revisado en Fase 1** (ver §7.5): la primera versión de esta sección optaba por un tipo `Dual`
propio (AAD forward-mode de una variable, `val + eps·ε`). Se abandona esa implementación manual en
favor del autodiff en modo reverse que ya trae Burn (`burn::backend::Autodiff<B>`, un decorador de
backend): envolver cualquier backend base con `Autodiff` lo equipa transparentemente con
`backward()`/`grad()`, sin tocar la lógica de valoración.

- El core Rust sigue siendo genérico, pero sobre `B: Backend` (§5.1) en vez de sobre un tipo
  escalar propio: la misma función de valoración sirve para cómputo puro (`B = CpuBackend`) o para
  sensibilidades (`B = Autodiff<CpuBackend>`), marcando con `.require_grad()` el/los tensores
  respecto a los que se quiere diferenciar.
- Frente al `Dual` forward-mode manual (una pasada por sensibilidad), el modo reverse de Burn
  calcula el grafo de cómputo una vez y obtiene **todas** las sensibilidades de una única pasada
  `backward()` — relevante en cuanto se necesite un vector de griegas completo (todas las curvas de
  Hull-White, no solo `r0`) en vez de una sensibilidad a la vez.
- Bump-and-reval **no desaparece**: se mantiene como mecanismo de validación cruzada de las
  sensibilidades calculadas vía AAD (`tests/aad_vs_bump_reval.rs`), no como alternativa de
  producción.

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
   verificado automáticamente. **Parcial desde Fase 4** (ver §7.8): CI no tiene Excel instalado, así
   que solo se automatiza la mitad "el bridge de Excel invoca el mismo `engine::Registries`/
   `IMeasure::evaluate` que Python, con los mismos parámetros/semillas" (`clients/excel/tests`); la
   verificación con Excel real cargando el `.xll` queda documentada como manual
   (`clients/excel/README.md`).

Estas capas se ejecutan en CI de forma incremental: las capas 1-2 ya deben existir antes de cerrar
la Fase 1; la capa 3 antes de cerrar la Fase 1 (coincide con la introducción de AAD); la capa 4 se
añade en cuanto exista más de un cliente.

## 6. Roadmap por fases (borrador, pendiente de detallar)

1. **Fase 0** ✅ — Esqueleto de repos/build: CMake + Corrosion orquestando un workspace Rust mínimo + binding C++ trivial vía `cxx` + smoke test desde Python (ver §7.1, verificado end-to-end).
2. **Fase 1** ✅ — Core Rust sobre Burn (§5.1, §5.3): kernels/modelos/productos genéricos sobre
   `Backend`, backend CPU (`burn-ndarray`) + simulación Hull-White 1F + valoración IRS + AAD (modo
   reverse de Burn) validado contra bump-and-reval (ver §7.5, verificado con las 4 capas de test de
   §5.6 que ya aplican en esta fase).
3. **Fase 2** ✅ — Capa C++: registry de modelos/productos/medidas, cálculo de exposición (EE/PFE) y CVA unilateral end-to-end sobre IRS+Hull-White (ver §7.6).
4. **Fase 3** ✅ — Cliente Python (nanobind) + Jupyter funcional (ver §7.7).
5. **Fase 4** ✅ — Cliente Excel (XLL) sobre el registry C++ (ver §7.8).
6. **Fase 5** ✅ — Backend GPU: alias `GpuBackend` (`burn-wgpu`) ejercitado con benchmarks reales
   sobre IRS+Hull-White; se mantiene tras feature opcional `gpu`, no por defecto (ver §7.11).
   Seleccionable desde los clientes: `with engine.backend("gpu"):` en Python,
   `ENGINE.SET_BACKEND("gpu")` en Excel (ver §7.12).
7. **Fase 6** ✅ (mecanismo) — API universal en C ABI: `engine/abi.h`/`engine_abi` (SHARED),
   misma superficie que Python/Excel (registry + selección de backend de §7.12) expresada en
   `extern "C"` plano, verificada con GoogleTest y una sonda en C puro (ver §7.13). Pendiente
   de decidir cómo se distribuye fuera de este repo (no forma parte de ninguna release hoy).
8. **Fase 7** ✅ — `MarketSnapshot` (curva de mercado, real o fabricada) + `ICalibrator`/
   `Registry<ICalibrator>` (nuevo cuarto registry, mismo patrón que modelos/productos/
   medidas): calibra `a`/`b` de `HullWhite1F` a una curva por mínimos cuadrados vía AAD
   (reutiliza la infraestructura de §5.3), en las cinco capas — Rust, registry C++, C ABI,
   Python (nanobind), Excel (ver §7.14).

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
- [x] **Toolchain de Rust** → stable pinneado (ver §7.3).
- [x] **Dependencias C++ (Corrosion, nanobind, test framework)** → CMake `FetchContent` (ver §7.3).

### 7.3 Decisiones de toolchain resueltas

**Rust: stable + Burn.** Se fija la versión exacta del toolchain en `rust/rust-toolchain.toml`
(canal `stable`, versión concreta a determinar al arrancar Fase 1 — la más reciente estable en ese
momento). El cómputo vectorial CPU/GPU y el AAD (§5.1, §5.3) delegan en el framework tensorial
[Burn](https://burn.dev) (crate `burn`, pinneada a una versión exacta en
`rust/crates/engine-core/Cargo.toml`) en vez de en primitivas propias (`wide` para SIMD, un tipo
`Dual` manual): decisión revisada en Fase 1 tras implementar primero la versión manual y comprobar
que Burn resuelve el mismo problema con menos código propio que mantener (ver §7.5). `engine-core`
activa solo las features de Burn que necesita (`ndarray`, `autodiff`, y `wgpu` tras la feature
`gpu` propia) con `default-features = false`, para no arrastrar el resto del ecosistema de Burn
(datasets, entrenamiento, etc.) que este motor no usa.

**C++: CMake `FetchContent`.** El `CMakeLists.txt` raíz trae Corrosion, nanobind y el framework de
test C++ (a decidir en Fase 2, probablemente GoogleTest) vía `FetchContent_Declare(... GIT_TAG
<commit-fijo>)`, cada uno pinneado a un commit/tag exacto — no a una rama móvil. Se prioriza
simplicidad sobre build 100% offline: no hay submódulos que sincronizar manualmente, y el pin
explícito en el propio `CMakeLists.txt` documenta la versión igual que lo haría un submódulo.
Revisar esta decisión si el entorno de CI/desarrollo termina necesitando builds sin acceso a red.

**CMake mínimo**: 3.24 (buen soporte de `FetchContent` moderno y compatible con las versiones
recientes de Corrosion/nanobind en Windows/MSVC).

### 7.4 CI — GitHub Actions

Repositorio: `github.com/makiolo/engine_quant`. Workflow en `.github/workflows/ci.yml`, disparado en
todo `push` y `pull_request` ("cada commit"). Dos jobs, cada uno cubriendo las capas testeables que
existen hoy (§5.6 layers 1-2 en Rust; el resto de capas de §5.6 se añaden a medida que exista
lógica real que testear en Fase 1+):

- **`rust-tests`** (`ubuntu-latest`) — `cargo test --workspace --locked` dentro de `rust/`. Cubre
  los unit tests deterministas de `engine-core` (§5.6 capa 1) y valida que `engine-ffi` (el bridge
  `cxx`) compila en una plataforma distinta a Windows — señal temprana de portabilidad, relevante
  de cara al backend GPU multiplataforma (§5.1). rustup respeta `rust/rust-toolchain.toml`
  automáticamente, sin paso explícito de instalación de toolchain.
- **`build-and-smoke-test`** (`windows-latest`, la plataforma principal del proyecto) — build
  completo vía CMake + Corrosion + `cxx` + nanobind (`ilammy/msvc-dev-cmd` para tener `cl.exe` en
  el `PATH`, replicando el recipe verificado manualmente en §7.1) seguido del smoke test de Python
  (`engine.ping() == 42.0`). Build en `Release` por el mismatch de CRT documentado en §7.1.

Pendiente para cuando exista contenido real: añadir tests de C++ (capas 2-3 de §5.6, requiere el
framework de test decidido en Fase 2) y el job de equivalencia Python↔Excel (capa 4, Fase 3-4).

### 7.5 Fase 1 — core Rust: kernels, backend, Hull-White, IRS, AAD

Todo el código de esta fase vive en `rust/crates/engine-core` (sin dependencia de `cxx`, testeable
con `cargo test` puro, ver notas de §7). Módulos:

- `backend.rs` — alias de tipo sobre los backends de Burn (§5.1): `CpuBackend = NdArray<f64>`
  (siempre disponible), `GpuBackend = Wgpu<f64>` (tras la feature `gpu` del crate, no compilada por
  defecto), y `Autodiff<B>` reexportado como el decorador que añade AAD en modo reverse (§5.3) a
  cualquiera de los dos.
- `kernel.rs` — primitivas genéricas reutilizables por cualquier modelo: `TimeGrid` (sin cambios,
  es aritmética `f64` pura) y un paso de Euler-Maruyama genérico sobre `B: Backend`, operando sobre
  `Tensor<B, 1>` de forma `[n_paths]` en vez de sobre un escalar — vectorizado sobre todos los paths
  Monte Carlo a la vez.
- `models/hull_white.rs` — Hull-White 1F **con nivel de reversión de largo plazo constante**
  (equivalente matemático a Vasicek) en vez de `theta(t)` calibrado a una curva de mercado:
  simplificación deliberada para no necesitar la infraestructura de calibración/curvas de Fase 2
  todavía, documentada en el propio módulo (sin relación con el cambio a Burn). Aporta la fórmula
  cerrada afín del bono cero-cupón y la simulación del tipo corto, ambas genéricas sobre `B:
  Backend` y vectorizadas sobre paths.
- `products/irs.rs` — IRS valorado por réplica en bonos cero-cupón bajo curva única. Limitación
  documentada: `IrSwap::npv` requiere que la fecha de valoración coincida con una fecha de reseteo
  del swap (`IrSwap::is_reset_date` / `remaining_from`); valorar a mitad de un periodo ya fijado
  queda para cuando haga falta.
- `exposure.rs` — perfil EE/PFE vía Monte Carlo (simula el tipo corto vectorizado sobre `CpuBackend`,
  revalora el swap restante analíticamente en cada trayectoria) y CVA unilateral simple con hazard
  rate plana. Concreto sobre `CpuBackend` (no genérico sobre `B`): el perfil de exposición no se
  diferencia en Fase 1, solo la valoración puntual (§5.3), así que no necesita `Autodiff`.
- `tests/aad_vs_bump_reval.rs` — capa 3 de §5.6: instancia el mismo código de valoración con
  `B = Autodiff<CpuBackend>`, marca el parámetro de interés con `.require_grad()`, llama
  `.backward()` y compara el gradiente (`.grad(&grads)`) contra diferencias finitas centrales sobre
  el mismo código instanciado con `B = CpuBackend`.

**Migración de la primera implementación (Scalar/Dual/ComputeBackend manuales) a Burn**: la
versión inicial de Fase 1 implementaba todo esto a mano (trait `Scalar` propio, tipo `Dual` para
AAD forward-mode, trait `ComputeBackend` con `CpuBackend` sobre `rayon`+`wide`). Se sustituyó por
Burn una vez la versión manual ya funcionaba y estaba testeada, tras confirmar que Burn cubre el
mismo terreno (tensores vectorizados CPU/GPU + autodiff en modo reverse) con una librería madura en
vez de código propio de bajo nivel que mantener — ver §5.1 y §5.3 para el razonamiento completo de
la decisión. La migración fue mecánica: los tests de cada módulo (incluida la convergencia MC vs
fórmula cerrada y AAD vs bump-and-reval) se reescribieron sobre la nueva API y siguen verificando
exactamente las mismas propiedades matemáticas que antes.

Las cuatro capas de test de §5.6 que aplican en Fase 1 (1: unit deterministas; 2: convergencia MC vs
fórmula cerrada; 3: AAD vs bump-and-reval) ya corren en `rust-tests` de CI sin cambios en el
workflow — `cargo test --workspace --locked` las cubre todas.

### 7.6 Fase 2 — registry C++: modelos, productos, medidas

Todo el código nuevo de esta fase vive en `cpp/engine` (ver estructura de §7). Antes de esta
fase, `engine.hpp`/`engine.cpp` eran la única API C++ y exponían funciones sueltas
("cadena de humo ampliada" de §7.5, fijas a un IRS 5y anual). Esta fase añade la capa de
orquestación real (§3.2, §5.4) por encima de esa fachada, sin romperla: las funciones de Fase
0-1 (`ping`, `hull_white_zero_coupon_bond`, `irs_unilateral_cva_5y`, ...) siguen existiendo tal
cual.

**Generalización previa en Rust** (`engine_core::api`, nuevo módulo junto a `smoke`): antes de
que el registry C++ pudiera tener sentido hacía falta que la capa Rust dejara de asumir un IRS
fijo a 5 años anuales. `irs_hull_white_exposure_profile` acepta un IRS arbitrario
(`payment_times`/`accruals` propios) y devuelve el perfil EE/PFE completo (no solo el CVA
agregado como hacía `smoke::irs_unilateral_cva_5y`); `unilateral_cva_from_exposure` calcula el
CVA a partir de un perfil ya calculado, separando ambos pasos para que la capa de medidas C++
pueda componerlos sin recalcular la simulación Monte Carlo. Ambas funciones son `f64` puro (sin
tipos de Burn en la firma, mismo principio que documenta `crate::smoke`: mantener `engine-ffi`
como frontera aislada, PLAN.md §7), y se bridgean a C++ vía un struct compartido de `cxx`
(`ExposureProfileResult { times, ee, pfe_95 }`, PLAN.md §5.5: "los tipos complejos ... se pasan
mediante structs planos").

**Registry — diseño concreto** (implementa la decisión de §5.4):

- `params.hpp` — `Params = std::unordered_map<std::string, ParamValue>` con
  `ParamValue = std::variant<double, std::vector<double>, bool>`: bag de parámetros genérico
  que hace uniforme la firma de cualquier factory del registry (`Registry<Interface>::create`),
  sin importar qué modelo/producto/medida concreto construye. Helpers `get_double`/`get_bool`/
  `get_vector` lanzan `std::out_of_range`/`std::invalid_argument` con el nombre de la clave en
  el mensaje si falta o el tipo no coincide.
- `model.hpp`/`product.hpp`/`measure.hpp` — interfaces `IModel`/`IProduct`/`IMeasure` (cada una
  con un único método propio además del destructor virtual y `type_name()`) e implementaciones
  concretas del caso base de §5.2: `HullWhite1FModel`, `IrSwapProduct`, `ExposureProfileMeasure`
  (perfil EE/PFE) y `UnilateralCvaMeasure` (CVA). Todas se construyen exclusivamente desde un
  `Params` (incluidas las medidas, que no lo necesitan para construirse — solo para
  `evaluate()` — pero llevan un constructor `Concrete(const Params&)` igualmente para que
  `Registry<T>::register_type<Concrete>` sea uniforme entre las tres interfaces).
  `IrSwapProduct` trata la clave `"fixed_rate"` como opcional: si está ausente, el swap se
  marca "a la par" (`use_par_rate() == true`) y el tipo fijo se calcula en Rust, que es quien
  tiene `r0` disponible en el momento de evaluar la medida.
- `IMeasure::evaluate` hace `dynamic_cast` de `model`/`product` a los tipos concretos que sabe
  evaluar y lanza `std::invalid_argument` si no coinciden — limitación conocida y documentada
  del caso base de Fase 2 (un único modelo y un único producto soportados), no un defecto de
  diseño del registry en sí (el registry en sí es agnóstico a cuántos tipos existan).
  `UnilateralCvaMeasure::evaluate` reutiliza `ExposureProfileMeasure::evaluate` por composición
  en vez de duplicar la llamada a `irs_hull_white_exposure_profile`.
- `registry.hpp` — `Registry<Interface>` genérico y header-only (sin `.cpp`: es una plantilla),
  un registry independiente por interfaz (`Registry<IModel>`, `Registry<IProduct>`,
  `Registry<IMeasure>`, no uno monolítico, tal como fija §5.4). `register_type<Concrete>(name)`
  registra una factory `std::make_unique<Concrete>(params)`; `create(name, params)` lanza
  `std::out_of_range` si `name` no está registrado.
- `bootstrap.hpp`/`bootstrap.cpp` — `struct Registries { Registry<IModel> models; Registry<IProduct> products; Registry<IMeasure> measures; }`
  y `register_builtins(Registries&)` como punto único de arranque (§5.4): registra
  `"HullWhite1F"`, `"IRSwap"`, `"ExposureProfile"`, `"UnilateralCVA"`. Añadir un modelo/
  producto/medida nuevo implica una línea aquí, nada más.

**Testing C++** (PLAN.md §7.3 dejaba pendiente decidir el framework): GoogleTest
(`v1.15.2` pinneado) vía `FetchContent` en `cpp/engine/tests/CMakeLists.txt`, mismo patrón que
ya usan Corrosion (raíz) y nanobind (`clients/python`) — `gtest_force_shared_crt` a `ON` antes
de `FetchContent_MakeAvailable` para evitar el mismatch de CRT en MSVC (mismo tipo de gotcha ya
documentado en §7.1 para el CRT de Rust, pero del lado de gtest). `cpp/engine/tests/test_registry.cpp`
cubre el wiring end-to-end del registry (alta de builtins, creación vía `Params`, EE≥0 y
PFE≥EE, CVA positivo con hazard rate>0 y ~0 con hazard rate=0, rechazo de tipos incompatibles) —
son tests de integración del registry/composición de medidas, no repiten la validación numérica
fina que ya cubre Rust (§5.6 capas 1-3, siguen viviendo en `rust-tests`). `ctest` se añade como
paso nuevo de `build-and-smoke-test` en CI (§7.4), entre el build y el smoke test de Python.

**Verificado end-to-end** (build local Release, CMake+Corrosion+cxx+nanobind+gtest): `cargo
test --workspace` (Rust, incluye `engine_core::api`), `ctest` (6/6 tests C++ del registry) y el
smoke test de Python de Fase 1 (que sigue ejercitando la fachada de Fase 0-1 directamente, sin
pasar por el registry) pasan los tres.

### 7.7 Fase 3 — cliente Python (nanobind) sobre el registry + Jupyter

Todo el código nuevo de esta fase vive en `clients/python` (ver estructura de §7). Antes de
esta fase, `engine_py_ext.cpp` solo exponía la cadena de humo de Fase 0-1 (`ping`,
`hull_white_zero_coupon_bond`, `irs_unilateral_cva_5y`); esta fase añade el binding 1:1 (o
casi) con la API pública del registry C++ de Fase 2 (§7.6) sin tocar esas funciones, que
siguen existiendo tal cual.

- **`engine.Engine`** — clase Python que envuelve `Registries` + `register_builtins`
  (PLAN.md §5.4): se instancia una vez y `register_builtins` se ejecuta en el constructor,
  en vez de exigir que el cliente Python llame a una función de bootstrap suelta.
  `list_models()`/`list_products()`/`list_measures()` exponen `Registry<T>::list()`;
  `create_model(name, params)`/`create_product(name, params)`/`create_measure(name)`
  exponen `Registry<T>::create(name, params)`.
- **`Params` como `dict` nativo de Python**, no una clase dedicada: una función auxiliar
  (`dict_to_params`, en `engine_py_ext.cpp`) convierte cada entrada del dict al
  `engine::ParamValue` (`double`/`std::vector<double>`/`bool`) correspondiente,
  comprobando `bool` antes que `double` porque en Python `bool` es subtipo de `int`/`float`.
  Coherente con PLAN.md §4 ("la API debe sentirse equivalente en Python y en Excel", no
  idéntica letra a letra al C++ subyacente: un dict es más idiomático en Python que un tipo
  `Params` envuelto).
- **`engine.Model`/`engine.Product`/`engine.Measure`** — bindings opacos de
  `IModel`/`IProduct`/`IMeasure` (solo `type_name` como propiedad de solo lectura;
  el `dynamic_cast` a los tipos concretos sigue viviendo enteramente en C++, dentro de
  `IMeasure::evaluate`). **`engine.MeasureResult`** expone los campos de
  `MeasureResult` como atributos de solo lectura (`times`/`primary`/`secondary`/
  `has_scalar`/`scalar`).
- **Traducción de excepciones**: nanobind traduce automáticamente `std::out_of_range` (tipo
  no registrado, `Registry<T>::create`) a `IndexError` y `std::invalid_argument` (medida
  incompatible con el modelo/producto recibido, `IMeasure::evaluate`) a `ValueError`, sin
  código adicional en el binding. Adicionalmente, pasar un `Model` donde se espera un
  `Product` (o viceversa) a `Measure.evaluate` ya lo rechaza nanobind en la frontera
  Python/C++ con `TypeError`, antes de llegar al `dynamic_cast` de C++ — una comprobación de
  tipos más temprana que la que ve el test C++ equivalente (`MeasureRejectsWrongProductType`,
  §7.6), no un defecto del binding.
- **`clients/python/tests/test_registry.py`** — equivalente Python de
  `cpp/engine/tests/test_registry.cpp` (mismos cinco casos, más el de tipos intercambiados
  vía `TypeError` en vez de `FakeProduct`+`std::invalid_argument`): confirma que el binding
  expone la misma semántica del registry C++, no solo que compila. Se ejecuta igual que
  `test_smoke.py` (`python clients/python/tests/test_registry.py <dir-del-build>`), añadido
  como paso nuevo de `build-and-smoke-test` en CI (§7.4), a continuación del smoke test de
  Python existente.
- **`clients/python/notebooks/demo_registry.ipynb`** — notebook Jupyter (Jupyter funcional,
  objetivo explícito de esta fase en el roadmap de §6) que recorre el mismo caso base de
  §5.2 (Hull-White 1F + IRS a la par a 5 años) paso a paso desde Python: listar tipos
  registrados, crear modelo y producto, calcular y graficar (matplotlib) el perfil EE/PFE,
  calcular el CVA unilateral, y una celda final que vuelve a listar los tipos registrados
  como demostración de la extensibilidad del registry (§5.4: un modelo/producto/medida
  nuevo añadido en `bootstrap.cpp` aparece ahí sin tocar el notebook). Notebook sin
  ejecutar en el repo (celdas de código con `outputs: []`/`execution_count: null`): requiere
  haber compilado el proyecto y tener `jupyterlab`/`matplotlib` instalados, ver
  `clients/python/notebooks/README.md`.

**Verificado end-to-end** (build local Release, mismo build que §7.6): `engine.Engine()`
ejercitado manualmente desde Python (miniconda 3.12, coincide con el `engine.cp312-*.pyd`
generado) reproduce los mismos resultados que `test_registry.cpp` (EE/PFE no negativos con
PFE≥EE, CVA positivo con hazard rate>0 y ~0 con hazard rate=0, `IndexError` en tipo no
registrado); `clients/python/tests/test_registry.py` y `clients/python/tests/test_smoke.py`
(sin regresión) pasan ambos, igual que los 6/6 tests de `ctest` de Fase 2.

**Pendiente para cuando exista más de un cliente**: la capa 4 de test de §5.6 (equivalencia
de API entre clientes) sigue esperando a la Fase 4 (cliente Excel/XLL) — con un único
cliente (Python) todavía no hay nada con lo que comparar.

### 7.8 Fase 4 — cliente Excel (XLL) sobre el registry C++

Todo el código nuevo de esta fase vive en `clients/excel` (ver estructura de §7). A
diferencia de Python (nanobind: binding directo, sin escribir a mano la frontera C↔C++),
Excel no tiene un generador de bindings equivalente: un XLL es una DLL corriente que Excel
carga con `LoadLibrary` y con la que habla a través de una API C plana (`XLCALL.H`,
`Excel12`/`Excel12v`) — no hay wrapper que abstraiga la construcción manual de `XLOPER12`.

**Qué se propaga solo y qué no** (relevante para juzgar cuánto esfuerzo exige extender el
motor): un modelo/producto/medida nuevo registrado en `bootstrap.cpp` (§5.4) queda
disponible en Python **y** en Excel con **cero** cambios en `clients/python`/`clients/excel`
— `list_models`/`create_model`/`Measure.evaluate` (y sus equivalentes `ENGINE.*` en Excel)
son genéricos sobre el nombre registrado, no una función por tipo concreto; ese es el punto
central de tener un registry (§4: "un producto o modelo nuevo se registra una vez y queda
disponible en todos los clientes sin duplicar lógica"). Lo que **sí** exige tocar ambos
clientes es una operación de la capa C++ que no encaja en el patrón
`Registries`/`Registry<T>::create`/`IMeasure::evaluate` (un método nuevo de `Registries`, o
una función suelta al estilo de las de Fase 0-1) — ninguno de los dos bindings tiene
reflexión sobre C++, así que el suelo real es una línea en `engine_py_ext.cpp` (un
`m.def(...)`, ya mínimo de por sí) más los dos sitios de `engine_excel.cpp` descritos más
abajo (antes tres, ver la revisión de esta sección).

**Vendoring de `XLCALL.H`/`XLCALL.CPP`** (`clients/excel/thirdparty/xlcall`, ver `NOTICE.md`
ahí): el *Microsoft Excel Developer's Toolkit*, header y fuente oficiales que cualquier XLL
de terceros necesita para hablar con Excel. Se vendorizan sin modificar (mismo fichero que
usan xlw/xll12/xll22/xll24) en vez de depender de un framework de terceros más amplio (se
evaluó `xlladdins/xll24`, descartado por no tener build de CMake y no tener licencia
explícita — solo se vendoriza el `XLCALL.H`/`.CPP` que es indiscutiblemente de Microsoft,
libremente redistribuible desde hace más de dos décadas). `XLCALL.CPP` resuelve el punto de
entrada `MdCallBack12` en tiempo de ejecución vía `GetProcAddress(GetModuleHandle(NULL),
"MdCallBack12")`: no hace falta enlazar contra ninguna `.lib` de importación (evita el
problema clásico de arquitectura/versión de `XLCALL32.LIB`).

**Bridge `xloper.hpp`/`.cpp`** — traducción entre `XLOPER12` y `engine::Params`/
`engine::MeasureResult`, sin llamar nunca a `Excel12`/`Excel12v` (por eso es testeable con
GoogleTest sin Excel instalado, `clients/excel/tests`):

- Argumentos de tipo `"Q"` (`XLOPER12` por referencia): según la documentación de
  `xlfRegister`, Excel los coacciona siempre a uno de `xltypeNum`/`Str`/`Bool`/`Err`/`Multi`/
  `Missing`/`Nil` (nunca `xltypeRef`/`SRef`, ya desreferenciados) — no hace falta un
  `xlCoerce` manual.
- Cadenas Excel12: `XCHAR` (= `WCHAR`) con la longitud en la posición 0, sin terminador nulo
  (máx. 32767 caracteres); conversión UTF-8 (lo que espera `engine::Params`) vía
  `WideCharToMultiByte`/`MultiByteToWideChar`.
- **`params` como rango clave/valor** (columna A = nombre, columnas siguientes = valor(es)),
  no una UDF por cada modelo/producto/medida: análogo Excel del `dict` nativo que usa el
  binding Python (`dict_to_params`, §7.7) para la misma razón — "la API debe sentirse
  equivalente" (§4) sin ser idéntica letra a letra al `Params` de C++. Un valor con una única
  celda numérica → `double`; con varias → `vector<double>`; con una única celda booleana →
  `bool` (comprobado antes que numérico, mismo motivo que en Python).
- Todo valor de retorno se reserva en el heap y se marca `xlbitDLLFree`: Excel llama de
  vuelta a `xlAutoFree12` (`engine_excel.cpp`) para liberarlo, que delega en
  `xlbridge::free_xloper` (recorre `xltypeMulti` recursivamente liberando también las
  cadenas internas).

**Handles memoizados (`handles.hpp`/`.cpp`)** — una celda de Excel no puede contener un
`IModel`/`IProduct`/`IMeasure` opaco (a diferencia de `engine.Model`/`Product`/`Measure` en
Python, §7.7): `HandleRegistry` envuelve exactamente el mismo `engine::Registries`/
`register_builtins`/`Registry<T>::create`/`IMeasure::evaluate` que ya consumen
`cpp/engine/tests` y `clients/python`, exponiendo cada instancia creada como un **handle**
(cadena) memoizado por una clave canónica (nombre + parámetros ordenados por clave, formato
por `std::to_chars` para precisión exacta de round-trip). Mismos parámetros ⇒ mismo handle ⇒
fórmula determinista, sin necesitar un mecanismo de liberación ligado al ciclo de vida de la
celda que lo creó (invalidación en recálculo, `xlfGetCaller`, etc. — se evaluó y se descartó
por su complejidad, no verificable sin Excel instalado en este entorno de desarrollo,
ver "Alcance y limitaciones" en `clients/excel/README.md`): las instancias viven hasta
`xlAutoClose`, no por-celda.

**`engine_excel.cpp`** — las UDFs (`ENGINE.LIST_MODELS`/`LIST_PRODUCTS`/`LIST_MEASURES`/
`CREATE_MODEL`/`CREATE_PRODUCT`/`CREATE_MEASURE`/`EVALUATE`) más los 3 puntos de entrada que
Excel exige de todo XLL (`xlAutoOpen`, `xlAutoClose`, `xlAutoFree12`). **Rediseñado tras la
primera versión** (revisión post-Fase 4) para minimizar cuántos sitios hay que tocar al
añadir una UDF nueva — el problema original: el nombre de cada función se escribía a mano
*tres veces* (la propia definición, una fila de la tabla de registro, y una línea en
`engine_excel.def`), sin ninguna comprobación que detectara un desajuste entre las tres.
Ahora son dos sitios, no tres:

1. La definición de la función (`extern "C" __declspec(dllexport) LPXLOPER12 WINAPI
   xlEngineFoo(...)`), con el cuerpo envuelto en `xlbridge::guarded(...)` (plantilla en
   `xloper.hpp`) en vez de repetir `try/catch (...) { return xlbridge::new_error(xlerrValue); }`
   en cada una — ninguna excepción de C++ puede cruzar la frontera hacia Excel sin
   desencadenar comportamiento indefinido, y ahora ese contrato vive en un solo sitio.
2. Una fila en la tabla `kFunctions` (registro explícito centralizado, mismo principio que
   §5.4), construida con la macro `ENGINE_XLL_ENTRY(fn, type_text, nombre_excel,
   argument_text, help)`: deriva el nombre exportado directamente del identificador C++ de
   `fn` (macro `ENGINE_XLL_WSTRINGIZE`, el truco estándar de "stringize + token-paste con
   `L`" para obtener un `L"..."` a partir de un nombre de función) en vez de que alguien lo
   vuelva a teclear a mano, y además fuerza `void(&fn)` (descartado vía el operador coma) —
   si `fn` no existe con ese nombre exacto, la fila no compila; un typo ya no puede llegar a
   producir un `#NAME?` silencioso dentro de Excel real.

**Sin fichero `.def`**: se comprobó (build local, `dumpbin /exports`) que en x64 `__stdcall`
no decora nombres de símbolo (a diferencia de x86, donde se sufija `"@N"` con el tamaño de
los argumentos) — como este proyecto solo compila para x64 (`x86_64-pc-windows-msvc`,
`windows-latest` en CI), `__declspec(dllexport)` en cada función basta para que Excel la
resuelva por `GetProcAddress` con el nombre exacto, sin necesitar un `.def` que repita la
lista de exports por tercera vez.

`xlAutoOpen` sigue registrando cada fila de `kFunctions` vía `Excel12(xlfRegister, ...)`
(tipo `"U"` de retorno y `"Q"` por argumento según la tabla de tipos de `xlfRegister`);
`xlAutoClose` llama a `HandleRegistry::clear()`.

**CMake** (`clients/excel/CMakeLists.txt`): `engine_excel_bridge` (STATIC, la parte
testeable) y `engine_excel_ext` (`MODULE` — un plugin que Excel carga con `LoadLibrary`,
nada más enlaza contra él, mismo motivo por el que `clients/python` usa
`nanobind_add_module` en vez de `SHARED`), con `SUFFIX ".xll"` explícito (Excel identifica
un add-in por esa extensión, no basta con que el contenido sea un DLL válido). Guardado tras
`if(WIN32)` en el `CMakeLists.txt` raíz: un XLL es un artefacto específico de Windows.

**Verificado localmente** (build Release vía CMake+Ninja+MSVC, sin Excel instalado en este
entorno de desarrollo): `engine_excel_bridge_tests` (15/15, incluye dos casos que reproducen
los mismos parámetros/semillas que `test_registry.cpp`/`test_registry.py` para confirmar que
el bridge invoca el mismo código, PLAN.md §5.6 capa 4 parcial) pasa; `engine_excel_ext`
enlaza y produce `engine_excel.xll`, con `dumpbin /exports` confirmando las 10 entradas
esperadas sin decorar. **Añadido tras esta fase** (§7.9): `engine_excel_harness_test` carga
el `.xll` con `LoadLibrary` y ejerce `xlAutoOpen`/las UDFs exactamente como lo haría Excel
(exportando su propio stub de `MdCallBack12`), cerrando gran parte del hueco de "¿esto
funciona con Excel de verdad?" sin necesitar Excel instalado. **Sigue pendiente de
verificación manual con Excel real** solo la parte que ese harness no cubre (Excel cargando
el `.xll` y evaluando una fórmula real en una hoja) — `clients/excel/README.md` documenta los
pasos y los valores de referencia exactos: `CVA = 426.76182440931836` para el caso
`UnilateralCVA` de `test_registry.py`.

### 7.9 Empaquetado y distribución — wheel Python autocontenida + XLL + release en CI

Motivación: hasta esta fase, usar `clients/python` o `clients/excel` exigía clonar el repo y
compilar (CMake + Corrosion + Rust + MSVC) — nada distribuible con un simple `pip install` o
"descarga y ejecuta". Objetivo de esta fase: que alguien sin el toolchain instalado pueda (a)
`pip install` una rueda de `engine-quant` y usar `import engine`, y (b) descargar un zip,
ejecutar un script, y tener el complemento funcionando en su Excel — sin arrastrar consigo
Visual Studio, Rust, ni preocuparse de dependencias que falten.

**`pyproject.toml` (raíz) + `scikit-build-core`** — build backend que sabe invocar el
`CMakeLists.txt` ya existente (no un `setup.py` paralelo con su propia lógica de build):
`build.targets = ["engine_py_ext"]` compila solo lo que hace falta para el cliente Python;
`cmake.define` fija `ENGINE_QUANT_BUILD_TESTS=OFF`/`ENGINE_QUANT_BUILD_EXCEL=OFF` (opciones
nuevas de `CMakeLists.txt` raíz, `option(... ON)` por defecto para no tocar el build normal)
para no traerse GoogleTest ni compilar el XLL en un build de rueda. `wheel.packages = []` +
`wheel.install-dir = "."` porque no hay paquete Python envoltorio: el único artefacto es
`engine.pyd`, instalado en la raíz del árbol vía el nuevo `install(TARGETS engine_py_ext
LIBRARY DESTINATION . RUNTIME DESTINATION .)` de `clients/python/CMakeLists.txt` (RUNTIME
porque en Windows un `MODULE` se instala como los ejecutables/DLLs, no como `LIBRARY` —
gotcha clásico de `install(TARGETS)` multiplataforma). El paquete pip se llama
`engine-quant`; el módulo importable sigue siendo `engine` (no se renombra código existente).

**Dependencias en tiempo de ejecución de la rueda** — comprobado con `dumpbin /dependents`
sobre el `.pyd` ya compilado: además de `python3XX.dll` (la proporciona el intérprete del
usuario) y DLL que ya trae cualquier Windows 10/11 (`kernel32`, `ntdll`,
`api-ms-win-crt-*.dll` — Universal CRT, servida vía Windows Update), depende de
`MSVCP140.dll`/`VCRUNTIME140.dll`/`VCRUNTIME140_1.dll` (runtime de C++, el motivo del
mismatch de CRT ya documentado en §7.1: todo se enlaza `/MD`, no hay forma sencilla de
enlazar estático sin tocar cómo `cxx-build`/Cargo compilan su propio shim). En vez de exigir
al usuario instalar el redistribuible de Visual C++ por separado, la rueda se repara con
[`delvewheel`](https://github.com/adang1345/delvewheel) (equivalente Windows de
`auditwheel`/`delocate`): embebe las DLL que de verdad hacen falta, con *name-mangling* para
evitar colisiones si el usuario tiene instaladas varias ruedas que también embeben su propia
copia del runtime de C++. Verificado en local: `delvewheel repair` sobre la rueda solo copia
`msvcp140.dll` — decide que `vcruntime140[_1].dll` no hace falta copiarlas porque las
considera parte del CRT universal que ya trae Windows 10/11 actualizado (mismo criterio que
se reutiliza sin reinventar para el `.xll`, ver más abajo). Instalada la rueda reparada en un
entorno virtual limpio (sin Visual Studio/Rust/`cmake` en el `PATH`), `import engine` más un
cálculo real (`UnilateralCVA`, mismos parámetros/semilla que `test_registry.py`) reproduce
exactamente `CVA = 426.76182440931836` — la rueda es indistinguible en resultado del build
directo.

**Complemento de Excel — instalador sin pasos manuales** (`clients/excel/install/`):
`Install-EngineExcelAddin.ps1`/`Uninstall-EngineExcelAddin.ps1` copian `engine_excel.xll` (y
sus DLL) a `%LOCALAPPDATA%\engine_quant\excel` y lo registran. Decisión de diseño no trivial,
con un giro durante el desarrollo:

- **Primer intento: automatizar Excel por COM** (`New-Object -ComObject Excel.Application`,
  `Application.AddIns.Add(...).Installed = $true` — el equivalente scriptable de abrir
  Archivo > Opciones > Complementos > Ir... a mano). Descartado tras probarlo en la máquina
  de desarrollo: incluso operaciones tan básicas como `Workbooks.Add()` sobre una instancia
  de Excel recién creada (visible o no) fallan con `RPC_E_CALL_REJECTED` ("Call was rejected
  by callee") de forma consistente y no transitoria (reintentos con espera no lo resuelven),
  pese a que el proceso `EXCEL.EXE` arranca y responde a nivel de mensajes de ventana — un
  síntoma característico de una política de seguridad/DCOM del equipo bloqueando llamadas COM
  entrantes a Office, no un fallo del complemento en sí.
- **Solución adoptada: escribir directamente la clave del registro que Excel usa para
  complementos persistentes por usuario** — `HKCU\Software\Microsoft\Office\<versión>\Excel\
  Options`, valores `OPEN`/`OPEN1`/`OPEN2`/... con formato `/R "ruta\al\complemento"`. No es
  una ingeniería inversa a ciegas: se confirmó leyendo el valor `OPEN` ya existente en la
  cuenta de desarrollo, puesto ahí por otro complemento XLL de terceros instalado
  previamente por esa misma vía (Archivo > Opciones > Complementos) — mismo formato exacto.
  El script busca el primer slot `OPEN`/`OPEN1`/... libre, comprueba que no esté ya
  registrado (idempotente), y opera sobre todas las claves `Excel\Options` que existan bajo
  `HKCU\Software\Microsoft\Office\*` (una por versión de Office que se haya ejecutado alguna
  vez en esa cuenta). Sin lanzar Excel en ningún momento: más rápido y sin la fragilidad de
  la automatización COM.
- Verificado end-to-end en la máquina de desarrollo: el script de instalación escribe el
  valor `OPEN1` con el formato correcto (comprobado con `reg query`); el de desinstalación lo
  quita y borra la carpeta de instalación (`-RemoveFiles`), dejando el equipo exactamente
  como estaba antes de la prueba.

**`engine_excel_harness_test`** (`clients/excel/tests/xll_harness_test.cpp`, nuevo desde esta
fase) — cierra el hueco que ni `engine_excel_bridge_tests` (lógica pura, nunca llama a
`Excel12`/`Excel12v`) ni la automatización COM (bloqueada en el entorno de desarrollo, ver
arriba) cubrían: carga con `LoadLibrary` el `engine_excel.xll` ya compilado — el mismo
artefacto que se empaqueta y distribuye — y ejerce `xlAutoOpen`/las UDFs exactamente como lo
haría Excel. Lo consigue exportando desde el propio ejecutable de test un stub de
`MdCallBack12`, el símbolo que `XLCALL.CPP` (vendorizado, §7.8) resuelve en tiempo de
ejecución vía `GetProcAddress(GetModuleHandle(NULL), "MdCallBack12")` — `Excel.exe` exporta
su propia implementación real de ese mismo símbolo por el mismo motivo. El stub responde a
`xlGetName`/`xlFree`/`xlfRegister` (las tres llamadas que hace `xlAutoOpen`) y comprueba: que
`xlAutoOpen` devuelve `1`, que registra exactamente las 7 UDFs con nombres `ENGINE.*`, y que
`xlEngineListModels()` devuelve un `xltypeMulti` que incluye `"HullWhite1F"`. Añadido a
`ctest` (22/22 en el build completo, antes 21/22) vía `add_test` directo (no `gtest_discover_
tests`: no usa GoogleTest, es un `main()` con comprobaciones propias, más simple para un
único caso de integración de bajo nivel). La ruta del `.xll` a cargar la resuelve el propio
CMake (`$<TARGET_FILE:engine_excel_ext>` como *compile definition*), no una ruta adivinada a
mano.

**`.github/workflows/release.yml`** — dispara con un `push` de tag `vX.Y.Z` (o
`workflow_dispatch` para probar el pipeline sin publicar nada). Tres jobs:

1. `build-wheels` (matriz `cp3.10`–`cp3.13`, `windows-latest`): fija la versión del paquete
   desde el tag (sustituye el `"0.0.0"` de `pyproject.toml`, que solo sirve para builds
   locales), construye la rueda (`pip wheel .`) y la repara con `delvewheel`.
2. `build-xll` (`windows-latest`): build completo + `ctest` (incluye
   `engine_excel_harness_test` contra el `.xll` real que se va a distribuir — no se publica
   nada que no haya pasado los tests) y empaqueta `engine_excel.xll` + `MSVCP140.dll`
   (localizada vía la variable de entorno `VCToolsRedistDir` que ya pone `ilammy/msvc-dev-
   cmd`, el mismo mecanismo de CI que usa `build-and-smoke-test`, §7.4) + los scripts de
   `clients/excel/install` en un único zip versionado.
3. `publish-release` (solo si el disparo fue un tag, no en `workflow_dispatch`): descarga los
   artefactos de los dos jobs anteriores y los adjunta a la release de GitHub
   (`softprops/action-gh-release`, que crea la release si no existe).

Todo el pipeline (sustitución de versión, `pip wheel` + `delvewheel repair`, y el
empaquetado en zip del `.xll`) se verificó localmente paso a paso con una versión de prueba
antes de confiar en que funcionaría sin poder ejecutar GitHub Actions desde este entorno.

**Confirmado en la práctica** (a diferencia del resto de esta fase, verificada solo en
local): una release real ejecutada desde GitHub Actions construyó las ruedas de Python
correctamente a la primera. El empaquetado del `.xll` falló la primera vez con un error real
de CI que no se había visto en local — el runner tenía un toolset de Visual Studio más nuevo
(14.51) cuya carpeta de `Redist` no se llama `Microsoft.VC143.CRT` como en el VS2022 14.44
usado para probar esto en desarrollo (la ruta estaba fija en vez de buscarse); corregido
sustituyendo la ruta fija por una búsqueda con `Get-ChildItem -Recurse` dentro de
`VCToolsRedistDir` (con `VCToolsInstallDir` como segundo intento) que no depende de ese
nombre de carpeta. Lección: por bien que se verifique cada paso en local, el entorno real de
CI puede diferir de formas que solo aparecen al ejecutarlo de verdad — de ahí que el "próxima
iteración" de más abajo siga recomendando confirmar en la práctica lo que no se pudo probar
así.

También se detectó (uso real, no en el desarrollo de esta fase) que `publish-release` se
saltaba siempre al lanzar el workflow a mano (`workflow_dispatch`): esa condición solo miraba
si el disparo era un `push` de un tag. Se añadió un job `determine-version` (calcula
versión/tag/si-se-publica una única vez, consumido por el resto de jobs) y dos inputs nuevos
de `workflow_dispatch` (`publish`, `version`) que permiten publicar una release real
directamente desde la pestaña Actions sin tocar git en local — `softprops/action-gh-release`
crea el tag `vX.Y.Z` correspondiente en el mismo paso. Python 3.14 añadido a la matriz de
`build-wheels` en la misma revisión.

### 7.10 Instalador Windows todo-en-uno (wizard `.exe`, Inno Setup)

Motivación: incluso con wheel y `.xll` autocontenidos (§7.9), seguía haciendo falta saber
qué hacer con cada uno (descomprimir un zip, ejecutar un script de PowerShell, saber en qué
`python.exe` instalar la rueda si hay varios). Objetivo de esta fase: un único `.exe` de
"siguiente, siguiente, instalar" que resuelva las dos cosas, con una casilla por componente
(complemento de Excel / paquete de Python), y que una desinstalación o una actualización
posterior deshaga o sustituya exactamente lo que instaló, sin dejar nada a medias.

**Inno Setup** (no WiX/NSIS) — elegido por su sencillez para este caso (wizard con
`[Components]` casi gratis, `[Run]`/`[UninstallRun]` para invocar los scripts de PowerShell
ya existentes sin reimplementar su lógica en otro lenguaje) y por integrarse bien en CI:
Chocolatey (preinstalado en los runners `windows-latest`) lo instala con una línea
(`choco install innosetup`), y se compila con un compilador de línea de comandos
(`ISCC.exe`).

**Reutiliza los scripts de PowerShell existentes, no los reimplementa**: el `.iss`
(`installer/EngineQuantSetup.iss`) es sobre todo empaquetado + un puñado de líneas `[Run]`/
`[UninstallRun]` que invocan `clients/excel/install/Install-EngineExcelAddin.ps1` y el
`Install-EngineWheels.ps1` nuevo de esta fase (ver más abajo) — la lógica de detección de
Excel/Python vive en un único sitio cada una, testeable y usable también sin el instalador
(zips manuales de §7.9). Retoque necesario en `Install-EngineExcelAddin.ps1`: nuevo
parámetro `-Optional` para que, si la máquina no tiene Excel instalado, termine con éxito en
vez de lanzar un error — antes solo se contemplaba el caso "Excel instalado y ya ejecutado
alguna vez" (única situación probada manualmente en §7.8); el instalador no debe fallar
entero solo porque no haya Excel. De paso, si Excel está instalado pero nunca se ha ejecutado
en esa cuenta (la clave `HKCU\...\Excel\Options` aún no existe, solo se crea la primera vez
que arranca), ahora se detecta la versión instalada vía `HKLM\...\Excel\InstallRoot`
(nativo y `WOW6432Node`) y se crea esa clave para poder registrar el complemento igual —
antes este caso también lanzaba un error.

**`Install-EngineWheels.ps1`/`Uninstall-EngineWheels.ps1`** (`clients/python/install/`,
nuevos en esta fase) — instalan/desinstalan la rueda en *todos* los intérpretes de Python de
64 bits detectados, sin una lista fija de versiones que mantener:

- Descubrimiento vía PEP 514 (`HKLM`/`HKCU\SOFTWARE\Python\PythonCore`, incluida la vista
  `WOW6432Node` de 32 bits en un Windows de 64) — comprobado en una máquina real que
  Miniconda también se registra ahí, no es exclusivo de los instaladores de python.org — más
  el lanzador `py` (`py -0p`) como fuente complementaria si está presente, deduplicado por
  ruta resuelta de `python.exe`.
- Para cada candidato, se le pregunta *directamente* su versión y arquitectura
  (`sys.version_info`, `struct.calcsize('P') * 8`) en vez de fiarse del nombre de la clave
  del registro: una entrada obsoleta (detectada de verdad en la máquina de desarrollo, una
  clave PEP 514 de un Python 3.7 ya desinstalado sin subclave `InstallPath`) se descarta sola
  en vez de romper el resto.
- Selección de rueda por nombre de fichero (`*-cp<major><minor>-cp<major><minor>-*.whl`): una
  versión de Python nueva que aún no exista hoy funciona en cuanto la matriz de
  `build-wheels` (§7.9) incluya su rueda, sin tocar este script.
- `pip install --force-reinstall --no-deps` (instala) / `pip uninstall -y` (desinstala) sobre
  cada intérprete, con un manifiesto (`installed_pythons.txt`, un `python.exe` por línea) que
  recuerda en cuáles se instaló para que la desinstalación no tenga que repetir todo el
  descubrimiento ni arriesgarse a tocar un intérprete que nunca lo tuvo.

**`installer/EngineQuantSetup.iss`** — `AppId` fijo (generado una vez,
`{C3C5CE8D-CE38-461E-A633-81B65EE77AE3}`, no cambia nunca entre releases): con el mismo
`AppId`, instalar una versión más nueva sobre una ya instalada actualiza en el mismo sitio
(Windows la reconoce como "la misma app", no crea una segunda entrada en Programas y
características) en vez de necesitar desinstalar la anterior a mano primero — los propios
pasos de instalación (idempotentes por diseño: `Install-EngineExcelAddin.ps1` ya lo era desde
§7.8, `pip install --force-reinstall` también) sobrescriben correctamente lo anterior al
volver a ejecutarse sobre los ficheros nuevos.

**Bug real encontrado probando la actualización** (no algo evitado por diseño a priori): el
nombre de fichero de la rueda incluye la versión (`engine_quant-1.2.3-...whl`), así que sin
más, Inno Setup *añade* la rueda nueva junto a la antigua en vez de sustituirla (no borra
ficheros que ya no aparecen en `[Files]` solo porque cambien de nombre) — y
`Install-EngineWheels.ps1`, al encontrar dos ficheros que casan con el mismo `cp3XX`, se
quedaba con el primero por orden alfabético, que resultó ser el *más antiguo*. Reproducido y
corregido en desarrollo: sección `[InstallDelete]` que vacía `{app}\wheels`/`{app}\xll` antes
de copiar los ficheros de la versión nueva. Verificado con un ciclo completo real (instalar
9.9.9 → actualizar a 9.9.10 → `pip show engine-quant` daba `9.9.10`, un único fichero `.whl`
en `{app}\wheels`, una única entrada en el registro de Programas y características con
`DisplayVersion 9.9.10`) antes de dar el fix por bueno — sin este ciclo de prueba real el bug
habría pasado desapercibido (compila y "funciona" en una instalación limpia igualmente).

`PrivilegesRequired=admin` (hace falta para poder instalar en cualquier intérprete detectado,
algunos solo escribibles como administrador) combinado con `Flags: runascurrentuser` en los
pasos `[Run]`/`[UninstallRun]` (para que el registro de Excel en `HKCU` y los intérpretes
"solo para mí" del usuario operen sobre el perfil del usuario real, no sobre el token elevado
de administrador). `[Components]` (Excel / Python, ambos marcados por defecto) da la casilla
de selección casi gratis, sin código adicional.

**`.github/workflows/release.yml`** — nuevo job `build-installer` (tras `build-wheels`/
`build-xll`): descarga las N ruedas (`merge-multiple`, todas a una misma carpeta —
`Install-EngineWheels.ps1` elige la que corresponda en tiempo de instalación, no hace falta
elegir aquí) y el zip del `.xll` (descomprimido a `installer\payload\xll`), copia los scripts
de `clients/python/install/`, instala Inno Setup vía Chocolatey y compila con
`ISCC.exe /DMyAppVersion=<version>`. `publish-release` adjunta también este `.exe` a la
release.

**Verificado en local, ciclo completo real** (instalación limpia, actualización 9.9.9→9.9.10,
desinstalación completa — los tres verificados leyendo el registro/el disco antes y después,
no solo confiando en el código de salida): compila con `ISCC.exe`; una instalación limpia dejó
`engine-quant` importable en el intérprete de Miniconda detectado en la máquina de desarrollo
y el complemento de Excel correctamente registrado en `HKCU\...\Excel\Options`; la
actualización sustituyó la rueda y no duplicó la entrada de Programas y características tras
corregir el bug de `[InstallDelete]` descrito arriba; la desinstalación completa dejó la
máquina exactamente como al principio (paquete no importable, clave de Excel eliminada,
carpetas de `%LOCALAPPDATA%`/`Program Files` limpias, entrada de Programas y características
eliminada).

### 7.11 Fase 5 — backend GPU: benchmarks reales y decisión de feature por defecto

Objetivo (§6, §5.1): dejar de tratar `GpuBackend` (`burn-wgpu`) como un alias de tipo sin
ejercitar y decidir, con datos, si conviene compilarlo por defecto o mantenerlo tras la
feature opcional `gpu` de `engine-core`.

**Cambio previo necesario — `crate::exposure` genérico sobre `Backend`**: antes de esta fase
`expected_exposure_profile`/`unilateral_cva` estaban escritos directamente sobre `CpuBackend`
(el resto del motor — `HullWhite1F<B>`, `IrSwap<B>` — ya era genérico desde Fase 1). Se
generaliza ambas funciones a `B: Backend<FloatElem = f64>` (el `FloatElem = f64` es el mismo
que ya fija `backend.rs` para ambos backends concretos, §5.1 — evita tener que arrastrar
`ElementConversion` genérico solo para comparar/multiplicar el resultado de `into_scalar()`)
para poder instanciarlas también con `GpuBackend`. La API pública de `crate::api` (frontera
consumida por `engine-ffi`/C++) sigue fijada a `CpuBackend`, sin cambios de comportamiento.

**Sonda de disponibilidad** (`examples/gpu_probe.rs`, `cargo run -p engine-core --features gpu
--example gpu_probe`): confirma antes de fiarse de cualquier benchmark que `burn-wgpu`
encuentra un adaptador GPU real en la máquina y devuelve resultados correctos. En la máquina
de desarrollo usada: adaptador `DefaultDevice` disponible, resultado numérico correcto.

**Benchmark** (`examples/gpu_vs_cpu_bench.rs`, `cargo run -p engine-core --release --features
gpu --example gpu_vs_cpu_bench`): mide el tiempo de `expected_exposure_profile` sobre el caso
base de §5.2 (IRS 5y anual a la par bajo Hull-White 1F) para una serie de tamaños de
`n_paths`, en `CpuBackend` (`burn-ndarray`) y `GpuBackend` (`burn-wgpu`), descartando una
primera pasada de calentamiento en cada backend (el pool de threads en CPU, la compilación de
shaders/inicialización del adaptador en GPU) para medir solo el coste en estado estable.
Resultado real (build `--release`, misma máquina que la sonda anterior):

| `n_paths` | CPU (`ndarray`) | GPU (`wgpu`) |
|---:|---:|---:|
| 1.000 | 5,6 ms | 53,8 ms |
| 10.000 | 72,5 ms | 76,9 ms |
| 100.000 | 555,0 ms | 90,7 ms (**6,1x**) |
| 1.000.000 | 9,27 s | 2,17 s (**4,3x**) |

**Decisión: `gpu` sigue siendo una feature opcional, no se activa por defecto.** Motivos,
todos con datos de la propia tabla o de esta fase:

1. **El punto de cruce está lejos del caso de uso por defecto.** Por debajo de ~10.000 paths
   (el rango típico de un smoke test o de una consulta interactiva desde Python/Excel, §5.2)
   la GPU es más lenta que la CPU — el overhead de lanzar kernels/shaders no se amortiza. Solo
   a partir de ~100.000 paths (perfiles de exposición de alta precisión, cálculo de un
   portfolio grande) la GPU gana con claridad.
2. **Coste de compilación no trivial** (ya anticipado en §5.1): compilar `engine-core` con
   `--features gpu` en `--release` tardó ~4 min adicionales frente a la compilación sin la
   feature, por el árbol de dependencias de `burn-wgpu`/`wgpu`/`naga`. Pagar ese coste en todo
   build (CI, desarrollo local) sin necesitarlo no está justificado.
3. **No hay adaptador GPU garantizado en CI**: los runners de `ubuntu-latest`/`windows-latest`
   usados en `.github/workflows/ci.yml` no tienen GPU; `rust-tests` seguiría verificando el
   motor sobre `CpuBackend` sin cambios. `gpu` queda como feature que un desarrollador activa
   explícitamente en una máquina con GPU real, verificado aquí con `gpu_probe`.

Guía práctica resultante para quien necesite decidir qué backend usar en un caso concreto:
activar `gpu` (y usar `GpuBackend` en vez de `CpuBackend` al construir el modelo/producto)
cuando el número de paths de Monte Carlo del cálculo sea del orden de 10⁵ o superior; para
todo lo demás (incluida toda la superficie actual de `crate::api`/clientes), `CpuBackend`
sigue siendo la opción correcta y es la que se mantiene compilada por defecto.

### 7.12 Selección de backend desde los clientes (Python `with`, UDF global de Excel)

§7.11 dejó `GpuBackend` benchmarkado pero inalcanzable desde fuera de `cargo run --example`:
ningún cliente (Python, Excel) tenía forma de pedir "corre esto en GPU". Esta sección cierra
ese hueco extendiendo la Fase 5 en vez de abrir una fase nueva — sigue siendo "que los
clientes puedan elegir CPU o GPU" (§6), solo que ahora de verdad desde fuera de Rust.

**Estado global de proceso, no un parámetro más de cada llamada** — decisión deliberada, no
un atajo: `crate::backend::current()`/`set_current()` (`rust/crates/engine-core/src/
backend.rs`) son un `AtomicU8` de proceso que leen internamente `irs_hull_white_exposure_
profile`/`unilateral_cva_from_exposure` (`crate::api`) en cada llamada, en vez de recibir el
backend como argumento. Mismo enfoque que `decimal.localcontext()` (stdlib) o `torch.device`
(PyTorch): tratar el backend como un *contexto ambiente* ("¿con qué calculo a partir de
ahora?") en vez de forzar a colarlo en cada llamada — y es lo único que encaja con cómo cada
cliente quiere seleccionarlo de verdad:

- **Python**: `with engine.backend("gpu"): ...` — gestor de contexto que guarda el backend
  previo al entrar y lo restaura al salir (incluso si el bloque lanza una excepción).
- **Excel**: `=ENGINE.SET_BACKEND("gpu")` — una hoja de cálculo no tiene un "bloque"
  equivalente a un `with`, así que es una UDF que cambia el estado global, tal cual.

**Cadena completa, sin acortar ningún tramo**: `engine_core::backend` (enum `ComputeBackend`
+ `current()`/`set_current()`, con `ComputeBackend::is_available()` distinguiendo "Cpu"
—siempre— de "Gpu" —solo si este build tiene la feature `gpu`, §7.11—) → `crate::api`
(`set_compute_backend`/`compute_backend_name`/`is_gpu_backend_available`, más el despacho en
tiempo de ejecución dentro de `irs_hull_white_exposure_profile`/`unilateral_cva_from_
exposure`: un `match current() { Cpu => ..., Gpu => ... }` que instancia `CpuBackend` o
`GpuBackend` según toque, ya que ambas funciones eran genéricas sobre `B: Backend` desde
§7.11) → `engine-ffi` (tres funciones más en el bridge `cxx`, sin `#[cfg(feature = "gpu")]`
propio: el despacho ya vive en `engine-core`) → `engine::set_compute_backend`/
`compute_backend_name`/`is_gpu_backend_available` en `cpp/engine/include/engine/engine.hpp`
→ nanobind (`clients/python/src/engine_py_ext.cpp`) y el bridge de Excel (`clients/excel/src/
engine_excel.cpp`).

`set_compute_backend("gpu")` devuelve `false` (sin cambiar nada) si el nombre no se reconoce
o si se pide un backend no compilado en este build — nunca cae en silencio a CPU sin que el
cliente se entere: Python lo convierte en una `ValueError` desde `engine.backend(...)`, Excel
en `#VALUE!` desde `ENGINE.SET_BACKEND`.

**`GpuBackend` compilable en toda la pila, no solo en `engine-core`** (necesario para que
"pedir gpu" tenga efecto real más allá de Rust): nueva feature `gpu` en `engine-ffi/Cargo.toml`
(reenvía a `engine-core/gpu`) y nueva opción de CMake `ENGINE_QUANT_ENABLE_GPU` (por defecto
`OFF`, coherente con la decisión de §7.11 de no compilarlo por defecto) que llama a
`corrosion_set_features(engine_ffi FEATURES gpu)`. **Verificado en local** (no en CI, ver
razones de §7.11 — sin adaptador GPU garantizado en el runner): con
`-DENGINE_QUANT_ENABLE_GPU=ON` compila el árbol completo (Rust + registry C++ + `engine.pyd` +
`.xll`) y, desde Python, `engine.is_gpu_backend_available()` da `True` y `with engine.backend
("gpu"):` ejecuta de verdad sobre `burn-wgpu` (perfil de exposición con valores del mismo
orden que CPU, ver limitación de reproducibilidad más abajo). Sin la opción (el `.xll`/wheel
que se publican hoy, PLAN.md §7.9), `is_gpu_backend_available()` da `False` y pedir "gpu"
falla con un mensaje claro en vez de silencio.

**Descubrimiento no anticipado — el mismo `seed` no da el mismo Monte Carlo en los dos
backends**: `examples/backend_dispatch_probe.rs` (nuevo, ejercita el despacho de `crate::api`
de punta a punta, a diferencia de `examples/gpu_vs_cpu_bench.rs` que instancia los tipos de
Burn a mano) reveló que `Tensor::random` con la misma semilla produce secuencias de shocks
distintas en `burn-ndarray` y `burn-wgpu` — cada backend trae su propio generador. El perfil
de exposición difiere en ruido estadístico (variación observada <2% en el caso de §5.2), no
en lógica de valoración; documentado en `clients/excel/README.md` para que nadie lo confunda
con un bug al comparar CPU vs GPU con el mismo `seed`.

**Gotcha de nanobind que costó diagnosticar** (dejar constancia para no repetir la
investigación): un método `__exit__(self, exc_type, exc_value, traceback)` expuesto con
argumentos `nb::object` falla con "incompatible function arguments" en **cualquier** llamada
donde alguno de los tres sea `None` — que es exactamente como Python invoca `__exit__` en una
salida normal del `with` — a menos que cada argumento se anote explícitamente con `.none()`
(`nb::arg("exc_type").none()`, PLAN.md `clients/python/src/engine_py_ext.cpp`): nanobind
rechaza `None` por defecto para *cualquier* tipo de argumento, incluido el genérico
`nb::object`, salvo que se pida explícitamente lo contrario. Sin este `.none()` el gestor de
contexto nativo lanzaba `TypeError` en cuanto el bloque `with` terminaba sin excepción — el
caso más común, así que habría fallado inmediatamente en cuanto alguien lo probara.

### 7.13 Fase 6 — API universal en C ABI (`engine/abi.h`)

Objetivo (§5.5): exponer la misma superficie que ya consumen Python/Excel (`Registries`/
`register_builtins`/`Registry<T>::create`/`IMeasure::evaluate`, más la selección de backend de
§7.12) como una interfaz `extern "C"` plana, para que un lenguaje sin binding dedicado (Julia
vía `ccall`, .NET vía P/Invoke, Go vía `cgo`) pueda consumir el motor sin pasar por `cxx` ni
por nanobind — nueva puerta de entrada, no una reimplementación: `engine/abi.h` es una
traducción de la capa C++ ya existente, igual que lo son `clients/python/src/engine_py_ext.cpp`
y `clients/excel/src/engine_excel.cpp`.

**`engine/abi.h` + `engine/src/abi.cpp`, nuevo target `engine_abi` (SHARED)**: el resto de
`cpp/engine` (target `engine`) es una `STATIC` library pensada para enlazarse dentro del mismo
árbol de CMake — no exporta símbolos ni tiene sentido como `.dll`/`.so` suelto. La C ABI sí
necesita serlo (un consumidor externo no compila este repo, enlaza contra un binario ya
compilado), así que es un target nuevo, no una opción del existente:

- **Handles opacos con ownership explícito** (`EngineModel*`/`EngineProduct*`/`EngineMeasure*`,
  cada uno envolviendo el mismo `std::unique_ptr<IModel/IProduct/IMeasure>` que ya devuelve
  `Registry<T>::create`): un `engine_abi_create_*` por cada `engine_abi_free_*`, sin recolector
  de basura ni memoización por parámetros al otro lado (a diferencia del `HandleRegistry` de
  Excel, §7.8 — ahí la memoización era un workaround específico al modelo de recálculo de
  Excel; un consumidor de C ABI gestiona su propio ciclo de vida, como con cualquier librería
  C: `sqlite3_close`, `curl_easy_cleanup`, etc.).
- **Parámetros como struct plano + longitud** (`EngineParam { key, kind, scalar, values,
  count }`, un array de estos en vez de `engine::Params`): mismo bag de datos (double / vector
  de double / bool) que ya consumen `dict_to_params` (Python) y `table_to_params` (Excel), sin
  `std::variant`/`std::unordered_map` en la frontera — PLAN.md §5.5 ("structs planos / punteros
  + longitud, evitando dependencias de serialización de terceros").
- **Ninguna excepción de C++ cruza la frontera** (comportamiento indefinido en C): cada función
  que puede fallar atrapa `std::exception` y devuelve un centinela (`NULL`, o `!= 0` para
  `engine_abi_evaluate`), dejando el detalle en `engine_abi_last_error()` (mensaje del último
  fallo de ese hilo, `thread_local` — mismo propósito que el `#VALUE!` de Excel o la excepción
  Python que traduce nanobind, pero explícito en vez de solo un código de error opaco).
- **Versionado explícito** (§5.5): `engine_abi_version()` devuelve un entero que solo sube
  cuando cambia el layout de un struct ya publicado o la firma de una función ya publicada —
  nunca al añadir algo nuevo al final. Empieza en `1`.
- **`ENGINE_ABI_API`** (`__declspec(dllexport)`/`dllimport` en Windows vía la macro
  `ENGINE_ABI_BUILD`, visibilidad por defecto en GCC/Clang): mismo header sirve para compilar
  la librería y para que un consumidor la incluya, sin macro propia que definir en el lado del
  consumidor.

**Verificado con dos niveles de test, ninguno delegado a "confiar en que compila"**:

1. `cpp/engine/tests/test_abi.cpp` (GoogleTest, como el resto de `cpp/engine/tests`) — pero
   llamando *solo* a la superficie `extern "C"` de `abi.h`, no a los tipos C++ del registry
   directamente, para probar exactamente lo que vería un consumidor externo. Reutiliza el
   mismo caso dorado que documenta `clients/excel/README.md` (IRS 5y+Hull-White,
   `seed=13`/`hazard_rate=0.02`/`recovery_rate=0.4` → `CVA=426.7618244093184`,
   `seed=7` → `EE≈[0, 12862.62, 13673.53]`) con tolerancia estrecha en vez de solo "no
   negativo": extiende la capa 4 de test de §5.6 ("equivalencia entre clientes") a la C ABI
   además de Python/Excel/C++ directo — los cuatro dan el mismo número.
2. `cpp/engine/examples/abi_c_smoke.c`, compilado como **C puro** (`project(engine_quant
   LANGUAGES CXX C)` en la raíz, nuevo — antes solo `CXX`): la única forma de comprobar de
   verdad que el header no es solo "C++ que se parece a C" es compilarlo con un compilador de
   C. Ejercita el mismo caso dorado y confirma en tiempo de ejecución que
   `engine_abi_c_smoke.exe` imprime el mismo `426.7618244093184`.

Ninguno de los dos se distribuye (ni el `.dll`, ni el `.exe` de la sonda): `pyproject.toml`
sigue construyendo solo `engine_py_ext` (wheel) y `release.yml` solo empaqueta
`engine_excel.xll` (§7.9) — `engine_abi`/`engine_abi_c_smoke` existen únicamente dentro del
árbol de build de desarrollo/CI, sin afectar a ningún artefacto publicado hoy.

**Ejemplos multi-lenguaje** (`examples/abi/`, además de `cpp/engine/examples/abi_c_smoke.c`):
cinco versiones del mismo recorrido (listar modelos, IRS 5y+Hull-White, `ExposureProfile`/
`UnilateralCVA`, backend de §7.12, un error controlado) consumiendo *solo* `engine/abi.h`, para
documentar cómo se ve de verdad consumir el motor desde fuera de este repo:

- **C++** (`examples/abi/cpp/main.cpp`, target `engine_abi_cpp_example`): envuelve los handles
  opacos con `std::unique_ptr` + deleters y traduce `NULL`/`!= 0` + `engine_abi_last_error()`
  a excepciones — la ABI en sí no puede permitirse eso (C puro), pero un consumidor en C++ sí.
- **Rust** (`examples/abi/rust/`): crate independiente, deliberadamente **no** miembro de
  `rust/Cargo.toml` ni dependiente de `engine-core`/`engine-ffi` (ese workspace habla con la
  capa C++ vía `cxx`, un mecanismo interno distinto) — declara a mano las firmas `extern "C"`
  de `abi.h` (lo que generaría `bindgen`) para demostrar que incluso Rust podría consumir el
  motor como cualquier lenguaje externo. Cero dependencias: `cargo build` no toca la red.
- **Python, dos versiones** (`examples/abi/python/abi_example_ctypes.py` y
  `abi_example_cffi.py`): ninguna pasa por el `.pyd` de nanobind de `clients/python` — sin
  compilar nada específico de Python, la demostración más directa de "universal" (§5.5).
  `ctypes` (solo librería estándar) declara cada struct campo a campo
  (`ctypes.Structure`/`ctypes.POINTER(...)`); `cffi` en modo ABI (`ffi.dlopen`, sin compilar
  una extensión propia) acepta en `ffi.cdef(...)` una traducción de `abi.h` en sintaxis C casi
  literal, menos código repetido a cambio de una dependencia externa (`pip install cffi`) —
  comparar ambos ficheros lado a lado documenta la diferencia entre las dos librerías de FFI
  más comunes de Python. Gotcha de `cffi` que costó diagnosticar: inicializar un campo `enum`
  de un struct vía diccionario (`ffi.new("EngineParam[]", [{"kind": "ENGINE_PARAM_DOUBLE",
  ...}])`) con el *nombre* de la constante como string falla con `TypeError: an integer is
  required` — hace falta el valor entero subyacente (`0`, no `"ENGINE_PARAM_DOUBLE"`).

Las cinco (C incluido) dieron exactamente los mismos números en verificación manual —
`ExposureProfile EE ≈ [0, 12862.62, 13673.53]`, `UnilateralCVA = 426.7618244093184` (Rust y
Python, con más decimales de precisión de imprenta, dieron `426.76182440931836`, el mismo
valor que ya documentaba `clients/excel/README.md` bit a bit) — confirmando que la traducción
C ABI ↔ `engine::Registries`/`IMeasure` es correcta independientemente del lenguaje/librería de
FFI que la consuma. CI (`ci.yml`) construye y ejecuta las cinco en cada push (el C++ y el C
comparten el build de CMake; Rust y ambas versiones de Python se compilan/ejecutan aparte, con
el mismo `engine_abi.dll` ya generado).

### 7.14 Fase 7 — `MarketSnapshot` y calibración de modelos (`ICalibrator`)

**Motivación**: hasta esta fase, `HullWhite1F` se instanciaba siempre con `a`/`b`/`sigma`
elegidos a mano (PLAN.md §5.2 lo documentaba como simplificación deliberada de la primera
versión). Para que el motor sirva para valorar un swap de verdad —no solo el caso dorado de
prueba— hace falta poder decir "estos son los parámetros que mejor reproducen *esta* curva de
mercado", no solo los que alguien tecleó. Esta fase añade esa infraestructura, en las cinco
capas del árbol, no solo en Rust.

**`MarketSnapshot` es datos, no una jerarquía polimórfica** — misma filosofía que `Params`
(§5.4): `pillars` (años desde hoy, estrictamente creciente) + `zero_rates` (tipos cero de
capitalización continua), dos vectores paralelos con interpolación lineal entre pillars y
extrapolación plana fuera de rango. Que el mercado sea "falso" (fabricado,
`synthetic_from_hull_white` — lee la propia fórmula cerrada de `HullWhite1F` en los pillars
dados, para poder probar/demostrar calibración sin depender de datos reales) o "real" (números
observados, de donde sea que vengan — no hay todavía un bootstrapping desde instrumentos de
mercado crudos: eso queda fuera de esta fase, ver "Próxima iteración" más abajo) es una
cuestión de *de dónde salen los números*, no de un tipo C++/Rust/Python distinto.

**Solo `a`/`b` se calibran; `sigma`/`r0` se toman como datos de entrada — decisión tomada tras
comprobarlo empíricamente, no una simplificación *a priori***: la primera versión intentó
calibrar `a`/`b`/`sigma` los tres a la vez contra el factor de descuento, y el optimizador
colapsaba `sigma` hacia 0 casi sin mover el error, porque `sigma` solo entra en el precio del
bono cero-cupón vía el término de convexidad `-sigma²/2` de `a_factor` — un efecto de segundo
orden, casi invisible frente al efecto de primer orden de `a`/`b` sobre la forma/nivel de la
curva. No es un defecto del optimizador: en la práctica de mercado la volatilidad de un modelo
de tipo corto se calibra contra instrumentos de volatilidad (swaptions, caps), no contra la
curva de descuento — no hay ese tipo de instrumento en `MarketSnapshot` todavía. `a`/`b` sí
están bien identificados por la curva (determinan directamente su forma y su nivel de largo
plazo) y calibran de forma robusta.

**Gauss-Newton amortiguado (Levenberg-Marquardt) con jacobiana vía AAD, no diferencias
finitas** (`rust/crates/engine-core/src/calibration.rs`): reutiliza el mismo autodiff en modo
reverse de Burn que ya usa `crate::smoke`/`tests/aad_vs_bump_reval.rs` (§5.3) — una
`backward()` por pillar y por iteración calcula la jacobiana de los residuos de precio
respecto a `(ln(a), b)` (`a` reparametrizado sobre su logaritmo para que el optimizador no
necesite restricciones: la fórmula afín de `HullWhite1F` exige `a > 0`, y el propio autodiff
se encarga de la regla de la cadena a través de `exp()`). La amortiguación de
Levenberg-Marquardt (subir/bajar un factor `lambda` en la diagonal de `JᵀJ` según si el paso
propuesto de verdad mejora el error) evita que Gauss-Newton diverja desde una estimación
inicial lejana — verificado con una estimación inicial deliberadamente alejada de los
parámetros "verdaderos" en los tests de las cinco capas.

**Registro explícito centralizado, cuarto registry** (§5.4, mismo patrón que
`IModel`/`IProduct`/`IMeasure`): `engine::ICalibrator` (`calibrate(MarketSnapshot, Params
inicial) -> CalibrationResult{Params óptimo, rmse, iterations, converged}`) +
`Registry<ICalibrator>` + `Registries::calibrators`, con `HullWhite1FCalibrator` registrado en
`bootstrap.cpp` bajo `"HullWhite1F"`. `CalibrationResult::optimal_params` es un `Params`
—igual que ya consume `Registry<IModel>::create`— no un struct de campos fijos: cierra el
círculo **Mercado → calibrar → Modelo calibrado** pasando el resultado directamente a
`create_model` sin ningún paso intermedio (verificado en las cinco capas: Rust, GoogleTest de
C++, GoogleTest de la C ABI, `pytest`-style de Python, y el propio rango "derramado" de Excel
referenciado con `#` en `ENGINE.CREATE_MODEL`).

**Las cinco capas, de dentro hacia fuera**:

1. **Rust** (`engine_core::market::MarketSnapshot`, `engine_core::calibration::
   calibrate_hull_white`, expuesto en `f64` puro vía `crate::api::calibrate_hull_white`).
2. **`engine-ffi`** (`cxx`): struct plano `HullWhiteCalibrationResult` + función
   `calibrate_hull_white`, sin lógica propia (frontera aislada, igual que el resto de este
   crate).
3. **Registry C++** (`engine::MarketSnapshot`, `engine::ICalibrator`/`HullWhite1FCalibrator`,
   `Registries::calibrators`) — la pieza que pedía explícitamente esta fase: "especialmente en
   C++", porque es donde vive el concepto genérico (`ICalibrator`), no solo su única
   implementación de hoy.
4. **C ABI** (`engine/abi.h`/`abi.cpp`): `EngineMarketSnapshot` (pillars/zero_rates/count, el
   mismo par de arrays plano) + `engine_abi_calibrate_hull_white`. **Deliberadamente sin
   handle de calibrador ni `Registry<ICalibrator>` genérico a este nivel** (a diferencia de
   model/product/measure, que sí lo tienen): con un solo calibrador implementado no hay
   genericidad real que ganar todavía en la ABI pública — se añadirá cuando exista un segundo
   calibrador, sin tener que rediseñar nada (la capa C++ ya es genérica).
5. **Python** (nanobind): `engine.MarketSnapshot` (constructor + `synthetic_from_hull_white`
   estático + `zero_rate`/`discount_factor`), `engine.Calibrator` (`Engine.list_calibrators`/
   `create_calibrator`), `engine.CalibrationResult` (`optimal_params` como `dict`, igual
   filosofía de "se siente de Python" que `dict_to_params` ya aplicaba a la entrada — nueva
   función inversa `params_to_dict`).
6. **Excel**: `ENGINE.LIST_CALIBRATORS`/`ENGINE.CREATE_CALIBRATOR`/`ENGINE.CALIBRATE`.
   `ENGINE.CALIBRATE` no toma el mercado como handle (`MarketSnapshot` no tiene estado que
   memoizar, se consume una sola vez por llamada): se pasa directamente como rango de **2
   columnas sin clave por fila** (`xlbridge::table_to_market`, distinto del rango
   clave/valor de `params`) — el resultado (`xlbridge::new_calibration_result`) "derrama" una
   tabla clave/valor con los parámetros óptimos más `rmse`/`iterations`/`converged`, pensada
   para pasarse tal cual a `ENGINE.CREATE_MODEL` (que ignora las claves que no reconoce).

**Verificado con el mismo caso en las cinco capas** (`true_a=0.15`, `true_b=0.025`,
`sigma=0.008`, `r0=0.02`, pillars de 0.5 a 30 años, estimación inicial `a=0.3, b=0.01`
deliberadamente lejana): todas recuperan `a`/`b` dentro de `1e-4` del valor verdadero,
`rmse < 1e-9`, `converged = true` — Rust (`cargo test`), C++ (`cpp/engine/tests/
test_calibration.cpp`, incluido el *round-trip* hasta `Registry<IModel>::create`), la C ABI
(`cpp/engine/tests/test_abi.cpp`), Python (`clients/python/tests/test_calibration.py`,
incluido el mismo *round-trip* hasta `Engine.create_model`) y Excel (`clients/excel/tests/
test_xloper.cpp`, vía `HandleRegistry::calibrate`).

---
*Próxima iteración: confirmar en la práctica (no se pudo ejecutar GitHub Actions desde este
entorno de desarrollo) que el job `build-installer` de `.github/workflows/release.yml` (§7.10)
compila y publica `engine_quant_setup.exe` en una release real, con la misma cautela que en
§7.9: la primera ejecución real del pipeline de wheel+xll ya reveló un problema (carpeta de
Redist con nombre distinto en un toolset de CI más nuevo) que no había aparecido en ninguna
verificación local. En cuanto exista verificación manual de Fase 4 con Excel real
(`clients/excel/README.md`), completar la capa 4 de test de §5.6 marcándola como verificada de
punta a punta, no solo parcial — aprovechar esa sesión para probar también `ENGINE.SET_BACKEND`/
`ENGINE.GET_BACKEND` (§7.12) y `ENGINE.CALIBRATE` (§7.14) con Excel real, incluida la
advertencia de recálculo manual. Con Fase 6 (§7.13) cerrada en su mecanismo pero no en su
distribución: `engine/abi.h`/`engine_abi` no se publican todavía en ninguna release (§7.9 solo
empaqueta la wheel y el `.xll`) — decidir si hace falta un artefacto propio (zip con `abi.h` +
`engine_abi.dll` + `.lib` de import) antes de considerar la interoperabilidad externa "usable"
por alguien fuera de este repo, no solo "implementada y testeada" dentro de él. Sobre la Fase 7
(§7.14): sigue pendiente todo lo que quedó fuera de alcance al cerrarla — bootstrapping de
`MarketSnapshot` desde instrumentos de mercado crudos (depósitos, futuros, swaps) en vez de
zero rates ya construidos; calibrar `sigma` contra instrumentos de volatilidad (swaptions,
caps) en vez de dejarlo fijo; generalizar `engine_abi_calibrate_hull_white` a un
`engine_abi_calibrate` genérico en cuanto exista un segundo `ICalibrator`; y, más en general,
el resto de la lista de "qué faltaría para valorar un swap de verdad" (day count/calendarios/
generación de calendario de pagos, multi-curva descuento vs. proyección, valoración a media
vida de un swap que ya fijó su cupón actual). CUDA (`burn-cuda`, §5.1) sigue abierto como
backend adicional si algún día hiciera falta más rendimiento que `wgpu`.*
