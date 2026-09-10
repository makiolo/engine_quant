# Framework de test C++ (PLAN.md §7.3, §7.4): GoogleTest vía FetchContent, mismo patrón que
# Corrosion/nanobind, pinneado a un tag exacto.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

add_executable(engine_tests test_registry.cpp test_abi.cpp test_calibration.cpp)
target_link_libraries(engine_tests PRIVATE engine engine_abi GTest::gtest_main)

# engine_abi es SHARED (PLAN.md §5.5, Fase 6): Windows no busca el .dll en el directorio del
# target que lo enlaza (a diferencia de un .lib estático), solo en el suyo propio o en el del
# .exe -- sin esta copia, engine_tests.exe falla en tiempo de carga (STATUS_DLL_NOT_FOUND)
# antes de ejecutar un solo test.
add_custom_command(
    TARGET engine_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:engine_abi> $<TARGET_FILE_DIR:engine_tests>
)

include(GoogleTest)
gtest_discover_tests(engine_tests)
