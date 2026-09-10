cmake_minimum_required(VERSION 3.24)
# C además de CXX: engine/abi.h (PLAN.md Fase 6, §5.5) se compila también como C puro en
# cpp/engine/examples/abi_c_smoke.c, para demostrar/verificar que es una C ABI de verdad y no
# solo "C++ que se parece a C" -- ningún otro target de este árbol necesita un compilador de C.
project(engine_quant LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
enable_testing()

# Permiten acotar el build a lo que hace falta en cada contexto (PLAN.md §7.9): un build de
# paquete/wheel (pyproject.toml, scikit-build-core) solo necesita cpp/engine + clients/python,
# sin traerse GoogleTest (cpp/engine/tests, clients/excel/tests) ni compilar el XLL.
option(ENGINE_QUANT_BUILD_TESTS "Compilar los tests de C++ (ctest: registry + bridge Excel)" ON)
option(ENGINE_QUANT_BUILD_EXCEL "Compilar el cliente Excel (XLL)" ON)
# PLAN.md §5.1/§7.11: GpuBackend (burn-wgpu) no se compila por defecto (árbol de dependencias
# y tiempo de compilación considerables, sin GPU garantizada en CI). Activar esta opción
# compila engine-ffi con la feature `gpu` de engine-core, dejando GpuBackend realmente
# disponible al pasar backend="gpu"/"auto" a un ExecutionContext en C++/Python/Excel
# (PLAN.md §7.15, sustituye el estado global de §7.12).
option(ENGINE_QUANT_ENABLE_GPU "Compila el backend GPU (burn-wgpu) en el core Rust" OFF)

# Corrosion: importa el workspace de Cargo (rust/) como targets CMake nativos (PLAN.md §5, §7.3).
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/rust/Cargo.toml)

# Corrosion nombra el target CMake "engine_ffi" (guiones -> guiones bajos, ver comentario de
# corrosion_add_cxxbridge más abajo). corrosion_set_features debe llamarse después de que el
# target exista (corrosion_import_crate lo crea arriba).
if(ENGINE_QUANT_ENABLE_GPU)
    corrosion_set_features(engine_ffi FEATURES gpu)
endif()

# Genera el header/fuente C++ del bridge cxx de engine-ffi (PLAN.md §7, §7.1).
# Nota: Corrosion reemplaza guiones por guiones bajos en el nombre del target CMake
# generado para crates staticlib/cdylib (el paquete Cargo sigue llamándose "engine-ffi").
corrosion_add_cxxbridge(engine-ffi-cxx
    CRATE engine_ffi
    FILES lib.rs
)

add_subdirectory(cpp/engine)
add_subdirectory(clients/python)

if(ENGINE_QUANT_BUILD_TESTS)
    add_subdirectory(cpp/engine/tests)
endif()

# Cliente Excel (PLAN.md Fase 4, §7.8): artefacto especifico de Windows (XLL, API de Excel
# via windows.h). El resto del arbol de CMake ya solo se construye/testea en windows-latest
# en CI (§7.4); este guard hace explicito que clients/excel en particular no tiene sentido
# fuera de Windows.
if(WIN32 AND ENGINE_QUANT_BUILD_EXCEL)
    add_subdirectory(clients/excel)
endif()
