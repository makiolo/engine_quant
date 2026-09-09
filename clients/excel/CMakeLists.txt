# Cliente Excel (XLL, PLAN.md §3.3/§6 Fase 4, §7.8). Especifico de Windows (windows.h, WINAPI,
# tabla de exports via .def): ver guard WIN32 alrededor de add_subdirectory(clients/excel) en
# el CMakeLists.txt raiz.
#
# engine_excel_bridge: logica de traduccion XLOPER12 <-> engine::Params/MeasureResult
# (xloper.hpp/.cpp) y el registry memoizado por handle (handles.hpp/.cpp). No depende de
# Excel12/Excel12v ni de XLCALL.CPP: es la parte testeable con GoogleTest sin Excel instalado
# (ver tests/).
add_library(engine_excel_bridge STATIC
    src/xloper.cpp
    src/handles.cpp
)
target_include_directories(engine_excel_bridge PUBLIC src thirdparty/xlcall/include)
target_link_libraries(engine_excel_bridge PUBLIC engine)

# engine_excel_ext: el XLL en si (PLAN.md §3.3, §7.8). MODULE (no SHARED): es un plugin que
# Excel carga via LoadLibrary, nada mas enlaza contra el -- mismo motivo por el que
# clients/python usa nanobind_add_module en vez de add_library(... SHARED ...).
# XLCALL.CPP (vendorizado, thirdparty/xlcall) resuelve Excel12/Excel12v en runtime via
# GetProcAddress; engine_excel.def fuerza los nombres de export sin decorar (xlAuto*/UDFs).
add_library(engine_excel_ext MODULE
    src/engine_excel.cpp
    thirdparty/xlcall/src/XLCALL.CPP
    src/engine_excel.def
)
target_link_libraries(engine_excel_ext PRIVATE engine_excel_bridge)
set_target_properties(engine_excel_ext PROPERTIES
    OUTPUT_NAME "engine_excel"
    SUFFIX ".xll"
)

add_subdirectory(tests)
