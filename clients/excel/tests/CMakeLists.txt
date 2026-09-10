# Mismo patron que cpp/engine/tests (PLAN.md §7.6): GoogleTest via FetchContent, pinneado al
# mismo tag exacto. FetchContent_Declare es idempotente si los argumentos coinciden con una
# declaracion anterior, asi que redeclarar aqui no vuelve a descargar nada.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

add_executable(engine_excel_bridge_tests test_xloper.cpp)
target_link_libraries(engine_excel_bridge_tests PRIVATE engine_excel_bridge GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(engine_excel_bridge_tests)

# Test de integracion del .xll real (PLAN.md §7.8): carga engine_excel.xll con LoadLibrary y
# ejerce xlAutoOpen/las UDFs exactamente como lo haria Excel, exportando su propio stub de
# MdCallBack12 -- no depende de GoogleTest ni de Excel instalado. ENGINE_EXCEL_XLL_PATH usa
# un generator expression para no adivinar la ruta de salida (Debug/Release, generador, ...).
add_executable(engine_excel_harness_test xll_harness_test.cpp)
target_include_directories(engine_excel_harness_test PRIVATE ${CMAKE_SOURCE_DIR}/clients/excel/thirdparty/xlcall/include)
target_compile_definitions(engine_excel_harness_test PRIVATE
    ENGINE_EXCEL_XLL_PATH="$<TARGET_FILE:engine_excel_ext>"
)
add_dependencies(engine_excel_harness_test engine_excel_ext)
add_test(NAME EngineExcelHarness COMMAND engine_excel_harness_test)
