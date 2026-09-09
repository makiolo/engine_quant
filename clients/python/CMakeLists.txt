find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)

include(FetchContent)

FetchContent_Declare(
    nanobind
    GIT_REPOSITORY https://github.com/wjakob/nanobind.git
    GIT_TAG v3.0.1
)
FetchContent_MakeAvailable(nanobind)

nanobind_add_module(engine_py_ext src/engine_py_ext.cpp)
target_link_libraries(engine_py_ext PRIVATE engine)
set_target_properties(engine_py_ext PROPERTIES OUTPUT_NAME "engine")

# Empaquetado (PLAN.md §7.9, pyproject.toml + scikit-build-core): instala engine.pyd en la
# raiz del arbol de instalacion (wheel.install-dir = "." en pyproject.toml lo mapea a la raiz
# del site-packages de la wheel) -- sin paquete Python envoltorio, "import engine" importa
# directamente la extension nativa. RUNTIME (no LIBRARY) porque en Windows un MODULE se
# instala como los ejecutables/DLLs; LIBRARY cubre el .so equivalente en Linux/macOS.
install(TARGETS engine_py_ext LIBRARY DESTINATION . RUNTIME DESTINATION .)
