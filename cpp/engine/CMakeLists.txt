add_library(engine STATIC
    src/engine.cpp
    src/params.cpp
    src/model.cpp
    src/product.cpp
    src/measure.cpp
    src/market.cpp
    src/pricing_context.cpp
    src/execution_context.cpp
    src/price.cpp
    src/calibrator.cpp
    src/bootstrap.cpp
    src/payoff/ids.cpp
    src/payoff/expression.cpp
    src/payoff/predicate.cpp
    src/payoff/contract.cpp
)

target_include_directories(engine PUBLIC include)

target_link_libraries(engine PUBLIC engine-ffi-cxx)

# API universal en C ABI (PLAN.md Fase 6, §5.5): shared library (.dll/.so) que expone
# engine/abi.h -- consumible desde cualquier lenguaje con FFI a C (Julia, .NET, Go, ...) sin
# pasar por cxx/nanobind/el bridge de Excel. ENGINE_ABI_BUILD activa dllexport en Windows (ver
# abi.h); un consumidor externo que incluya abi.h sin definirlo obtiene dllimport.
add_library(engine_abi SHARED
    src/abi.cpp
)
target_include_directories(engine_abi PUBLIC include)
target_link_libraries(engine_abi PRIVATE engine)
target_compile_definitions(engine_abi PRIVATE ENGINE_ABI_BUILD)

# Sonda en C puro de engine/abi.h (PLAN.md §5.5): demuestra que la ABI de arriba se puede
# consumir sin compilador de C++, lo mas parecido a un consumidor externo real (Julia/.NET/Go)
# que se puede verificar en este árbol.
add_executable(engine_abi_c_smoke examples/abi_c_smoke.c)
target_link_libraries(engine_abi_c_smoke PRIVATE engine_abi)
add_custom_command(
    TARGET engine_abi_c_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:engine_abi> $<TARGET_FILE_DIR:engine_abi_c_smoke>
)

# Ejemplo de C++ idiomático (RAII + excepciones) sobre la misma C ABI, vive en examples/abi/
# (PLAN.md §7.13) porque -- a diferencia de engine_abi_c_smoke, que verifica el mecanismo -- su
# propósito es documentar cómo se consume, junto a sus equivalentes de Rust/Python.
add_executable(engine_abi_cpp_example ${CMAKE_SOURCE_DIR}/examples/abi/cpp/main.cpp)
target_link_libraries(engine_abi_cpp_example PRIVATE engine_abi)
add_custom_command(
    TARGET engine_abi_cpp_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:engine_abi> $<TARGET_FILE_DIR:engine_abi_cpp_example>
)
