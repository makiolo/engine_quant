cmake_minimum_required(VERSION 3.24)
project(engine_quant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
enable_testing()

# Corrosion: importa el workspace de Cargo (rust/) como targets CMake nativos (PLAN.md §5, §7.3).
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/rust/Cargo.toml)

# Genera el header/fuente C++ del bridge cxx de engine-ffi (PLAN.md §7, §7.1).
# Nota: Corrosion reemplaza guiones por guiones bajos en el nombre del target CMake
# generado para crates staticlib/cdylib (el paquete Cargo sigue llamándose "engine-ffi").
corrosion_add_cxxbridge(engine-ffi-cxx
    CRATE engine_ffi
    FILES lib.rs
)

add_subdirectory(cpp/engine)
add_subdirectory(clients/python)
add_subdirectory(cpp/engine/tests)

# Cliente Excel (PLAN.md Fase 4, §7.8): artefacto especifico de Windows (XLL = DLL con tabla
# de exports xlAuto*/UDFs via .def, API de Excel via windows.h). El resto del arbol de CMake
# ya solo se construye/testea en windows-latest en CI (§7.4); este guard hace explicito que
# clients/excel en particular no tiene sentido fuera de Windows.
if(WIN32)
    add_subdirectory(clients/excel)
endif()
