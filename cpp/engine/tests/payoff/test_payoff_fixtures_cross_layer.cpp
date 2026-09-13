// PLAN_PRODUCTS.md Fase 10 (§12, Aceptacion: "los mismos JSON fixtures producen mismo hash y
// resultados en las cinco capas"): confirma que los fixtures reales de
// docs/schema/engine.payoff/examples/*.json producen el mismo explain()/hash agregado en la
// capa nucleo C++ y en la C ABI -- las otras dos capas alcanzables sin Excel real se cubren
// con estos mismos fixtures via ctypes en clients/python/tests/test_engine_typed_payoff.py
// (nanobind + engine_abi.dll) y via HandleRegistry en clients/excel/tests/test_xloper.cpp
// (bridge de Excel, mismos casos ad-hoc). No repite la validacion de FORMA contra el JSON
// Schema (eso ya lo hace test_payoff_schema.py, sin compilar C++) -- aqui se confirma la
// validacion SEMANTICA (ValidationVisitor) de cada fixture, la primera vez que se ejercen con
// el motor real end-to-end.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "engine/abi.h"
#include "engine/payoff/payoff_product.hpp"

namespace {

#ifndef ENGINE_PAYOFF_EXAMPLES_DIR
#error "ENGINE_PAYOFF_EXAMPLES_DIR no definido (ver cpp/engine/tests/CMakeLists.txt)"
#endif

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("no se pudo abrir " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct ProductHandle {
    EngineProduct* ptr;
    ~ProductHandle() { engine_abi_free_product(ptr); }
};

std::string last_error() {
    std::size_t len = engine_abi_last_error(nullptr, 0);
    std::string buffer(len, '\0');
    engine_abi_last_error(buffer.data(), len + 1);
    return buffer;
}

class PayoffFixturesCrossLayerTest : public ::testing::TestWithParam<std::string> {};

TEST_P(PayoffFixturesCrossLayerTest, CoreAndAbiAgreeOnValidationAndExplain) {
    std::string path = std::string(ENGINE_PAYOFF_EXAMPLES_DIR) + "/" + GetParam();
    std::string spec = read_file(path);

    // Capa nucleo C++ (misma superficie que engine_typed.payoff SS7.3 / HandleRegistry de
    // clients/excel/src/handles.cpp).
    auto core_errors = engine::payoff::validate_payoff_spec(spec);
    ASSERT_TRUE(core_errors.empty()) << GetParam() << ": " << (core_errors.empty() ? "" : core_errors[0]);
    std::string core_explain = engine::payoff::explain_payoff_spec(spec);
    ASSERT_FALSE(core_explain.empty());

    // Capa C ABI (misma superficie que consumiria Julia/.NET/Go, PLAN.md §5.5, o Python via
    // ctypes/nanobind).
    EXPECT_EQ(engine_abi_validate_payoff_spec(spec.c_str()), 0) << last_error();

    EngineParam params[] = {EngineParam{"spec", ENGINE_PARAM_STRING, 0.0, nullptr, 0, spec.c_str()}};
    ProductHandle product{engine_abi_create_product("Payoff", params, 1)};
    ASSERT_NE(product.ptr, nullptr) << last_error();

    std::size_t len = engine_abi_explain_product(product.ptr, nullptr, 0);
    std::string abi_explain(len, '\0');
    engine_abi_explain_product(product.ptr, abi_explain.data(), len + 1);

    // Mismo fixture -> mismo arbol/hash agregado en ambas capas (el hash canonico forma parte
    // del texto de explain(), ver PayoffProduct::explain()).
    EXPECT_EQ(core_explain, abi_explain) << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Fixtures, PayoffFixturesCrossLayerTest,
    ::testing::Values(
        std::string("call.json"), std::string("forward.json"), std::string("swap.json"),
        std::string("barrier.json"), std::string("tp_sl.json"), std::string("exercise.json"),
        std::string("asian.json")
    )
);

} // namespace
