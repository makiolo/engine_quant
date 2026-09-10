# Ejemplos de la C ABI (`engine/abi.h`, PLAN.md Fase 6, §5.5/§7.13; ENGINE.CALC en PLAN.md §7.15)

Cinco versiones del mismo recorrido -- listar modelos registrados, construir un IRS a 5 años
bajo Hull-White 1F, calcular un lote de medidas (`PV`, `DV01`, `ExpectedExposure`, `PFE95`,
`UnilateralCVA`) con `engine_abi_calc` y provocar dos errores controlados (medida y modelo
desconocidos) -- consumiendo únicamente la interfaz `extern "C"` de
[`cpp/engine/include/engine/abi.h`](../../cpp/engine/include/engine/abi.h), nunca el registry
C++ interno, `cxx` ni nanobind. El objetivo es documentar cómo se ve "de verdad" consumir el
motor desde fuera de este repo, en distintos lenguajes/librerías de FFI, no repetir la
validación numérica fina (eso ya lo cubren `rust/crates/engine-core` y
`cpp/engine/tests/test_registry.cpp`/`test_abi.cpp`) -- por eso estos cinco ejemplos verifican
solo invariantes cualitativos (PV~0, DV01>0, PFE95>=ExpectedExposure>=0, CVA>0), no un valor
exacto: el valor de referencia exacto para el mismo caso vive en
`cpp/engine/tests/test_registry.cpp` (`Registry.UnilateralCvaMatchesGoldenValue`/
`ExposureProfileMatchesGoldenValue`) y en
[`clients/excel/README.md`](../../clients/excel/README.md) ("Verificación manual"). Si alguno
de estos cinco falla sus invariantes, algo se rompió en la traducción C ABI ↔
`engine::Registries`/`engine::calc` para ese lenguaje/librería concreto, no en el motor (que ya
validan los demás).

El ejemplo en C (único de los cinco actualizado en PLAN.md §7.18/§7.19, ver "## C" más abajo)
añade además la calibración genérica de los dos modelos del motor
(`engine_abi_create_calibrator`/`engine_abi_calibrate`, uno por `HullWhite1F` y `HullWhite2F`)
hasta pasar el resultado a `engine_abi_create_model` -- cerrando el círculo Mercado → calibrar
→ Modelo calibrado también desde esta ABI -- y un lote homogéneo de 3 swaps con
`engine_abi_calc_batch` (PLAN.md §7.19: mismo `EngineProduct**` genérico que `calc_many`/
`calc_grid`, no repetido aquí por acotar el alcance). El resto de lenguajes seguirían exactamente el
mismo patrón (mismo `EngineCalibrator` opaco que `EngineModel`/`EngineProduct`), no repetido
aquí por acotar el alcance.

## Requisito común: compilar `engine_abi` primero

Los cuatro ejemplos enlazan/cargan `engine_abi.dll` (Windows) o `libengine_abi.so`/`.dylib`
(Linux/macOS), generado al compilar el árbol CMake de la raíz del repo (ver `PLAN.md`):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target engine_abi
```

Por defecto, los ejemplos que no viven dentro del propio árbol de CMake (Rust, Python) buscan
la librería en `<repo>/build/cpp/engine/`; una variable de entorno permite apuntar a otra
ubicación (por ejemplo, un `engine_abi` instalado fuera de este repo).

## C

**`cpp/engine/examples/abi_c_smoke.c`** (no vive en esta carpeta: es también el mecanismo de
verificación de la C ABI en CI, PLAN.md §7.13) -- se compila como parte del árbol de CMake
principal, como C puro (`project(engine_quant LANGUAGES CXX C)` en la raíz):

```
cmake --build build --target engine_abi_c_smoke
build/cpp/engine/engine_abi_c_smoke.exe
```

## C++

[`cpp/main.cpp`](cpp/main.cpp) -- envuelve los handles opacos de la ABI con RAII
(`std::unique_ptr` + deleters) y traduce el patrón `NULL`/`!= 0` +
`engine_abi_last_error()` a excepciones, para que el resto del código se lea como C++
normal. Target `engine_abi_cpp_example` del mismo `cpp/engine/CMakeLists.txt`:

```
cmake --build build --target engine_abi_cpp_example
build/cpp/engine/engine_abi_cpp_example.exe
```

## Rust

[`rust/`](rust) -- crate independiente, **no** es miembro de `rust/Cargo.toml` (ese workspace
habla con la capa C++ vía `cxx`/`engine-ffi`, un mecanismo interno distinto de esta ABI
pública) ni depende de `engine-core`/`engine-ffi`: declara a mano en
[`src/main.rs`](rust/src/main.rs) las mismas firmas `extern "C"` de `abi.h` -- la traducción
que generaría `bindgen` automáticamente -- y no tiene ninguna dependencia externa (`cargo
build` no necesita red).

```
cd examples/abi/rust
cargo build
# Windows: el .dll debe ser localizable en tiempo de ejecución (no solo el .lib en tiempo de
# enlazado) -- cópialo junto al .exe generado, o añade su carpeta al PATH:
cp ../../../build/cpp/engine/engine_abi.dll target/debug/
./target/debug/engine_abi_example.exe
```

`ENGINE_ABI_LIB_DIR` (variable de entorno, leída por `build.rs`) sobreescribe dónde buscar
`engine_abi.lib`/`.dll` en tiempo de compilación si no está en `../../../build/cpp/engine`
relativo a este crate.

## Python (dos versiones: `ctypes` y `cffi`)

Ambas siguen el mismo recorrido y aceptan las mismas variables de entorno
(`ENGINE_ABI_LIB_DIR`, una carpeta; o `ENGINE_ABI_LIB_PATH`, la ruta completa al `.dll`/`.so`,
con prioridad sobre la anterior) para apuntar a una ubicación de `engine_abi` distinta de
`<repo>/build/cpp/engine/`. Comparar ambos ficheros lado a lado es la forma más directa de ver
la diferencia entre las dos librerías de FFI más comunes de Python.

- [`python/abi_example_ctypes.py`](python/abi_example_ctypes.py) -- `ctypes`, **solo librería
  estándar**: a diferencia de `import engine` (`clients/python`, un `.pyd` de nanobind
  compilado para una versión exacta de CPython), esta ruta funciona con cualquier Python que
  tenga `ctypes` -- ninguna compilación específica de Python de por medio, la demostración más
  directa de qué significa "universal" en PLAN.md §5.5.

  ```
  python examples/abi/python/abi_example_ctypes.py
  ```

- [`python/abi_example_cffi.py`](python/abi_example_cffi.py) -- [`cffi`](https://cffi.readthedocs.io/),
  en modo ABI (`ffi.dlopen`, sin compilar una extensión propia): `ffi.cdef(...)` acepta
  declaraciones en sintaxis C casi literal (la misma forma que `abi.h`, sin macros de
  exportación ni directivas de preprocesador) en vez de traducir cada struct campo a campo a
  `ctypes.Structure`, menos código repetido a cambio de una dependencia externa
  (`pip install cffi`).

  ```
  pip install cffi
  python examples/abi/python/abi_example_cffi.py
  ```

## Otros lenguajes con FFI a C

Julia (`ccall`), .NET (`P/Invoke`/`DllImport`) o Go (`cgo`) seguirían exactamente la misma
forma que estos cuatro: cargar `engine_abi.dll`/`.so`/`.dylib`, declarar los mismos structs
planos (`EngineParam`, `EngineMeasureResult`) y las mismas firmas de `abi.h` en su propia
sintaxis de FFI. No hay un ejemplo de cada uno aquí por acotar el alcance, no porque la ABI no
los soporte -- PLAN.md §5.5 los menciona explícitamente como motivación original de esta fase.
