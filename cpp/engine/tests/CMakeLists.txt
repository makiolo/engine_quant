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

add_executable(engine_tests test_registry.cpp test_abi.cpp test_calibration.cpp payoff/test_ids.cpp payoff/test_expression.cpp payoff/test_predicate.cpp payoff/test_contract_basic.cpp payoff/test_validation.cpp payoff/test_dependency.cpp payoff/test_scenario_evaluator_vanilla.cpp payoff/test_explain.cpp payoff/test_trigger_state.cpp payoff/test_trigger_barrier.cpp payoff/test_tp_sl.cpp payoff/test_measure_cross_check.cpp payoff/test_json_parser.cpp payoff/test_canonical_visitor.cpp payoff/test_payoff_product.cpp payoff/test_measures.cpp payoff/test_gbm_measures.cpp payoff/test_gbm_measures_p.cpp payoff/test_gbm_exercise_measures.cpp payoff/test_irs_templates.cpp payoff/test_fxforward_templates.cpp payoff/test_specialization_visitor.cpp payoff/test_market_snapshot_bridge.cpp payoff/test_price_many_mixed_products.cpp payoff/test_payoff_fixtures_cross_layer.cpp)
target_link_libraries(engine_tests PRIVATE engine engine_abi GTest::gtest_main)

# PLAN_PRODUCTS.md Fase 10 (§12, Aceptacion: "los mismos JSON fixtures producen mismo hash y
# resultados en las cinco capas"): test_payoff_fixtures_cross_layer.cpp lee los fixtures reales
# de docs/schema/engine.payoff/examples/*.json desde disco -- necesita saber donde esta esa
# carpeta sin adivinar el directorio de trabajo de ctest (mismo patron que
# ENGINE_EXCEL_XLL_PATH en clients/excel/tests/CMakeLists.txt).
target_compile_definitions(engine_tests PRIVATE
    ENGINE_PAYOFF_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/docs/schema/engine.payoff/examples"
)

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
