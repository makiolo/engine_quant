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
