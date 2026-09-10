# Ejemplos de la C ABI (`engine/abi.h`, PLAN.md Fase 6, §5.5/§7.13)

Cuatro versiones del mismo recorrido -- listar modelos registrados, construir un IRS a 5 años
bajo Hull-White 1F, evaluar `ExposureProfile` y `UnilateralCVA`, consultar/seleccionar el
backend de cómputo (PLAN.md §7.12) y provocar un error controlado -- consumiendo únicamente la
interfaz `extern "C"` de [`cpp/engine/include/engine/abi.h`](../../cpp/engine/include/engine/abi.h),
nunca el registry C++ interno, `cxx` ni nanobind. El objetivo es documentar cómo se ve "de
verdad" consumir el motor desde fuera de este repo, en cuatro lenguajes distintos, no repetir
la validación numérica fina (eso ya lo cubren `rust/crates/engine-core` y
`cpp/engine/tests/test_abi.cpp`).

Los cuatro deben imprimir los mismos números -- el mismo caso/semillas que documenta
[`clients/excel/README.md`](../../clients/excel/README.md) ("Verificación manual"):
`ExposureProfile EE ≈ [0, 12862.62, 13673.53]` (`seed=7`) y `UnilateralCVA = 426.7618244093184`
(`seed=13`, `hazard_rate=0.02`, `recovery_rate=0.4`). Si alguno da un número distinto, algo se
rompió en la traducción C ABI ↔ `engine::Registries`/`IMeasure` para ese lenguaje concreto, no
en el motor (que ya validan las otras tres).

## Requisito común: compilar `engine_abi` primero

Los cuatro ejemplos enlazan/cargan `engine_abi.dll` (Windows) o `libengine_abi.so`/`.dylib`
(Linux/macOS), generado al compilar el árbol CMake de la raíz del repo (ver `PLAN.md`):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target engine_abi
```

Por defecto, los tres ejemplos que no viven dentro del propio árbol de CMake (Rust, Python)
buscan la librería en `<repo>/build/cpp/engine/`; una variable de entorno permite apuntar a
otra ubicación (por ejemplo, un `engine_abi` instalado fuera de este repo).

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

## Python

[`python/abi_example.py`](python/abi_example.py) -- usa `ctypes` (solo librería estándar): a
diferencia de `import engine` (`clients/python`, un `.pyd` de nanobind compilado para una
versión exacta de CPython), esta ruta funciona con cualquier Python que tenga `ctypes` --
ninguna compilación específica de Python de por medio, la demostración más directa de qué
significa "universal" en PLAN.md §5.5.

```
python examples/abi/python/abi_example.py
```

`ENGINE_ABI_LIB_DIR` (carpeta) o `ENGINE_ABI_LIB_PATH` (ruta completa al `.dll`/`.so`, tiene
prioridad) como variables de entorno para apuntar a una ubicación distinta de
`<repo>/build/cpp/engine/`.

## Otros lenguajes con FFI a C

Julia (`ccall`), .NET (`P/Invoke`/`DllImport`) o Go (`cgo`) seguirían exactamente la misma
forma que estos cuatro: cargar `engine_abi.dll`/`.so`/`.dylib`, declarar los mismos structs
planos (`EngineParam`, `EngineMeasureResult`) y las mismas firmas de `abi.h` en su propia
sintaxis de FFI. No hay un ejemplo de cada uno aquí por acotar el alcance, no porque la ABI no
los soporte -- PLAN.md §5.5 los menciona explícitamente como motivación original de esta fase.
