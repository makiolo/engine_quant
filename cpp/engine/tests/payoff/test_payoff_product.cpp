// Tests de `PayoffProduct` (PLAN_PRODUCTS.md §7.2, §9.1, §12 Fase 3): dos constructores,
// mensaje agregado de validación, integración con el registry, `payoff_program()`/`explain()`.
// Cierra el núcleo C++ de Fase 3.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/payoff/canonical_visitor.hpp"
#include "engine/payoff/payoff_product.hpp"

namespace {

using namespace engine::payoff;

ContractPtr call_100() {
    return when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            mul(constant(1000.0), maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0)))
        )
    );
}

TEST(PayoffProductTest, DirectConstructorPopulatesProgramAndHash) {
    ContractPtr contract = call_100();
    PayoffProduct product("AAPL_CALL_100", contract);

    EXPECT_EQ(product.type_name(), "Payoff");
    ASSERT_NE(product.payoff_program(), nullptr);
    EXPECT_EQ(product.payoff_program()->id, "AAPL_CALL_100");
    EXPECT_EQ(product.payoff_program()->contract, contract);
    EXPECT_EQ(product.payoff_program()->canonical_hash, CanonicalVisitor::hash("AAPL_CALL_100", contract));
}

TEST(PayoffProductTest, ParamsConstructorParsesSpecJson) {
    std::string json = CanonicalVisitor::to_json("AAPL_CALL_100", call_100());
    engine::Params params{{"spec", json}};
    PayoffProduct product(params);

    EXPECT_EQ(product.type_name(), "Payoff");
    ASSERT_NE(product.payoff_program(), nullptr);
    EXPECT_EQ(product.payoff_program()->id, "AAPL_CALL_100");
}

TEST(PayoffProductTest, ExplainIncludesIdHashAndTreeStructure) {
    ContractPtr contract = call_100();
    PayoffProduct product("AAPL_CALL_100", contract);
    std::string text = product.explain();
    EXPECT_NE(text.find("AAPL_CALL_100"), std::string::npos);
    EXPECT_NE(text.find(product.payoff_program()->canonical_hash), std::string::npos);
    EXPECT_NE(text.find("When"), std::string::npos);
}

TEST(PayoffProductTest, AggregatesMultipleValidationErrorsIntoOneException) {
    // Dos Cashflow sin instante activo (sin When/Trigger envolvente), cada uno un error
    // independiente de ValidationVisitor -- confirma que el mensaje agregado los contiene a
    // AMBOS, no solo el primero (ADR-P0-05).
    ContractPtr invalid = both({
        cashflow(Currency{"USD"}, constant(1.0)),
        cashflow(Currency{"USD"}, constant(2.0)),
    });

    try {
        PayoffProduct product("BAD", invalid);
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        std::string message = e.what();
        // Cada Cashflow sin cursor genera su propio error "... sin instante activo ..." con un
        // NodePath distinto (children[0]/children[1]) -- confirma que ambos aparecen.
        EXPECT_NE(message.find("children[0]"), std::string::npos);
        EXPECT_NE(message.find("children[1]"), std::string::npos);
    }
}

TEST(PayoffProductTest, RegistryCreatesPayoffProductFromSpecParams) {
    engine::Registries registries;
    engine::register_builtins(registries);

    std::string json = CanonicalVisitor::to_json("AAPL_CALL_100", call_100());
    auto product = registries.products.create("Payoff", engine::Params{{"spec", json}});

    ASSERT_NE(product, nullptr);
    EXPECT_EQ(product->type_name(), "Payoff");
    ASSERT_NE(product->payoff_program(), nullptr);
    EXPECT_EQ(product->payoff_program()->id, "AAPL_CALL_100");
}

TEST(PayoffProductTest, LegacyProductsStillReturnNullPayoffProgram) {
    engine::Registries registries;
    engine::register_builtins(registries);
    auto irs = registries.products.create(
        "IRSwap", engine::Params{
                      {"notional", 1'000'000.0},
                      {"payment_times", std::vector<double>{1.0}},
                      {"accruals", std::vector<double>{1.0}},
                      {"fixed_rate", 0.03},
                  }
    );
    EXPECT_EQ(irs->payoff_program(), nullptr);
}

} // namespace
