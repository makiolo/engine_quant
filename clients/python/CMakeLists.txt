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
