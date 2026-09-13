// PLAN_PRODUCTS.md §12 Fase 11 ("grouping por fingerprint... y modelo"; §9.2 "agrupa por
// fingerprint de IR, modelo y configuración de medida"): price_batch_generic (price.cpp) debe
// deduplicar evaluate() entre productos DISTINTOS del lote que comparten canonical_hash (mismo
// id, mismo AST byte a byte) -- no solo dentro de un mismo producto, como ya hacía `computed`
// antes de esta fase. Este test demuestra el dedup DE VERDAD (una CountingPVMeasure que cuenta
// evaluate() reales), no solo que el resultado final sea correcto -- la corrección ya estaba
// garantizada por price_many/price_batch_generic antes de Fase 11.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/measure.hpp"
#include "engine/model.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/price.hpp"
#include "engine/product.hpp"
#include "engine/registry.hpp"

namespace {

namespace pf = engine::payoff;

using engine::ExecutionContext;
using engine::IMeasure;
using engine::IModel;
using engine::IProduct;
using engine::MarketSnapshot;
using engine::MeasureResult;
using engine::Params;
using engine::PriceBatchResult;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

// Contador de evaluaciones reales, a nivel de translation unit del test -- reseteado al
// principio de cada TEST (no hay estado compartido entre tests de este archivo más allá de
// este contador, que cada TEST pone a cero explícitamente).
std::atomic<int> g_eval_count{0};

// Envuelve la medida real "PV" ya registrada: cuenta cada evaluate() real y delega el cálculo
// en la medida de verdad, para que el resultado numérico siga siendo el PV correcto mientras el
// test observa cuántas veces se evaluó de verdad.
class CountingPVMeasure : public IMeasure {
public:
    CountingPVMeasure(const Params& params, const Registries& registries)
        : delegate_(registries.measures.create("PV", params)) {}

    std::string type_name() const override { return "CountingPV"; }

    MeasureResult evaluate(
        const IModel& model, const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
        const ExecutionContext& execution
    ) const override {
        g_eval_count.fetch_add(1, std::memory_order_relaxed);
        return delegate_->evaluate(model, product, market, pricing, execution);
    }

private:
    std::unique_ptr<IMeasure> delegate_;
};

struct FingerprintDedupFixture {
    pf::Currency ccy{"USD"};
    Registries registries;

    FingerprintDedupFixture() {
        register_builtins(registries);
        // register_factory (no register_type) porque el delegado real ("PV") se construye
        // dentro de la fábrica, capturando `registries` -- vive tanto como el fixture, que a su
        // vez vive durante toda la llamada a price_many() del test (registry.hpp,
        // Registry<Interface>::register_factory).
        registries.measures.register_factory("CountingPV", [this](const Params& p) {
            return std::make_unique<CountingPVMeasure>(p, registries);
        });
    }

    pf::ContractPtr bond_ast(double amount, double maturity) const {
        return pf::when(pf::TimePoint{maturity}, pf::cashflow(ccy, pf::constant(amount)));
    }

    std::unique_ptr<IModel> hull_white_model() const {
        return registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
    }
};

TEST(PriceBatchFingerprintDedupTest, SameFingerprintAcrossDistinctProductsEvaluatesOnce) {
    g_eval_count = 0;
    FingerprintDedupFixture fx;
    MarketSnapshot market({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022});

    const double amount = 500'000.0;
    const double maturity = 3.0;

    // 5 PayoffProduct DISTINTOS en memoria (5 objetos, no el mismo puntero 5 veces) con el
    // mismo id y el mismo contrato -- honesto sobre qué se deduplica: el fingerprint del AST
    // (canonical_hash), no la identidad del puntero C++.
    std::vector<std::unique_ptr<pf::PayoffProduct>> owned;
    for (int i = 0; i < 5; ++i) {
        owned.push_back(std::make_unique<pf::PayoffProduct>("ZERO_COUPON_BOND", fx.bond_ast(amount, maturity)));
    }
    std::vector<const IProduct*> products;
    for (const auto& p : owned) products.push_back(p.get());

    auto model = fx.hull_white_model();
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});

    PriceBatchResult results =
        price_many(fx.registries, products, std::vector<std::string>{"CountingPV"}, *model, market, pricing, execution);

    ASSERT_EQ(results.size(), 5u);
    EXPECT_EQ(g_eval_count.load(), 1) << "5 productos con el mismo fingerprint deben compartir una única evaluate() real";

    const double expected = amount * market.discount_factor(maturity);
    for (std::size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i].trade_index, i);
        ASSERT_TRUE(results[i].measures[0].result.has_scalar);
        EXPECT_NEAR(results[i].measures[0].result.scalar, expected, 1e-6);
    }
}

TEST(PriceBatchFingerprintDedupTest, DifferentFingerprintsEvaluateSeparately) {
    g_eval_count = 0;
    FingerprintDedupFixture fx;
    MarketSnapshot market({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022});

    // 3 bonos con distinto amount/maturity -- distinto canonical_hash cada uno -- no debe haber
    // dedup espurio entre contratos que en realidad son diferentes.
    std::vector<std::unique_ptr<pf::PayoffProduct>> owned;
    owned.push_back(std::make_unique<pf::PayoffProduct>("BOND_A", fx.bond_ast(100'000.0, 1.0)));
    owned.push_back(std::make_unique<pf::PayoffProduct>("BOND_B", fx.bond_ast(200'000.0, 2.0)));
    owned.push_back(std::make_unique<pf::PayoffProduct>("BOND_C", fx.bond_ast(300'000.0, 3.0)));
    std::vector<const IProduct*> products;
    for (const auto& p : owned) products.push_back(p.get());

    auto model = fx.hull_white_model();
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});

    PriceBatchResult results =
        price_many(fx.registries, products, std::vector<std::string>{"CountingPV"}, *model, market, pricing, execution);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(g_eval_count.load(), 3) << "3 contratos distintos no deben compartir evaluate()";
}

} // namespace
