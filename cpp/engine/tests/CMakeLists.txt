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

add_executable(engine_tests test_registry.cpp)
target_link_libraries(engine_tests PRIVATE engine GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(engine_tests)
