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

add_subdirectory(tests)
