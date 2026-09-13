// Criterio de aceptacion explicito de PLAN_PRODUCTS.md §12 Fase 8: "price_many mezcla IRS,
// FXForward y payoff custom sin ramas de producto nuevas". Este test mezcla en un unico
// price_many() un IrSwapProduct legacy, un PayoffProduct que envuelve exactamente el mismo AST
// que templates::irs_swap generaria, y un PayoffProduct con un payoff custom (bono cupon cero
// sintetico) que nunca ha existido como clase nominal -- las tres via la misma medida "PV"/
// "DV01" registrada, sin ningun dynamic_cast nuevo por tipo de producto mas alla de la unica
// rama generica anadida a PresentValueMeasure/Dv01Measure (measure.cpp).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/payoff/irs_templates.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/price.hpp"

namespace {

namespace pf = engine::payoff;
namespace tmpl = engine::payoff::templates;

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::PriceBatchResult;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

struct MixedBatchFixture {
    pf::Currency ccy{"USD"};
    double notional = 1'000'000.0;
    double fixed_rate = 0.03;
    std::vector<double> payment_times{1.0, 2.0, 3.0};
    std::vector<double> accruals{1.0, 1.0, 1.0};

    double bond_amount = 500'000.0;
    double bond_maturity = 3.0;

    Registries registries;
    MixedBatchFixture() { register_builtins(registries); }

    std::vector<pf::TimePoint> payment_time_points() const {
        std::vector<pf::TimePoint> out;
        out.reserve(payment_times.size());
        for (double t : payment_times) out.push_back(pf::TimePoint{t});
        return out;
    }

    pf::ContractPtr irs_ast() const {
        tmpl::IrsSwapSpec spec{ccy, notional, fixed_rate, pf::TimePoint{0.0}, payment_time_points(), accruals};
        return tmpl::irs_swap(spec);
    }

    pf::ContractPtr bond_ast() const {
        return pf::when(pf::TimePoint{bond_maturity}, pf::cashflow(ccy, pf::constant(bond_amount)));
    }
};

TEST(PriceManyMixedProductsTest, PresentValueMatchesAcrossLegacyPayoffAstAndCustomPayoff) {
    MixedBatchFixture fx;
    MarketSnapshot market({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022});

    auto legacy_irs = fx.registries.products.create(
        "IRSwap", Params{
                      {"notional", fx.notional},
                      {"fixed_rate", fx.fixed_rate},
                      {"start", 0.0},
                      {"payment_times", fx.payment_times},
                      {"accruals", fx.accruals},
                  }
    );
    pf::PayoffProduct irs_as_payoff("IRS_AS_AST", fx.irs_ast());
    pf::PayoffProduct custom_bond("CUSTOM_BOND", fx.bond_ast());

    std::vector<const engine::IProduct*> products{&*legacy_irs, &irs_as_payoff, &custom_bond};

    auto model = fx.registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});

    PriceBatchResult results =
        price_many(fx.registries, products, std::vector<std::string>{"PV"}, *model, market, pricing, execution);

    ASSERT_EQ(results.size(), 3u);

    // Fila 0: IRS legacy. Fila 1: mismo swap como AST -- deben coincidir exactamente (misma
    // formula, PLAN_PRODUCTS.md §9.3).
    double legacy_pv = results[0].measures[0].result.scalar;
    double ast_pv = results[1].measures[0].result.scalar;
    EXPECT_NEAR(ast_pv, legacy_pv, 1e-6);

    // Fila 2: bono custom, nunca existio como clase nominal -- PV = amount * DF(maturity).
    double bond_pv = results[2].measures[0].result.scalar;
    EXPECT_NEAR(bond_pv, fx.bond_amount * market.discount_factor(fx.bond_maturity), 1e-6);

    // El orden de entrada se preserva (trade_index).
    EXPECT_EQ(results[0].trade_index, 0u);
    EXPECT_EQ(results[1].trade_index, 1u);
    EXPECT_EQ(results[2].trade_index, 2u);
}

TEST(PriceManyMixedProductsTest, Dv01MatchesAcrossLegacyAndPayoffAst) {
    MixedBatchFixture fx;
    MarketSnapshot market({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022});
    const double bump = 0.0001;

    auto legacy_irs = fx.registries.products.create(
        "IRSwap", Params{
                      {"notional", fx.notional},
                      {"fixed_rate", fx.fixed_rate},
                      {"start", 0.0},
                      {"payment_times", fx.payment_times},
                      {"accruals", fx.accruals},
                  }
    );
    pf::PayoffProduct irs_as_payoff("IRS_AS_AST", fx.irs_ast());

    std::vector<const engine::IProduct*> products{&*legacy_irs, &irs_as_payoff};

    auto model = fx.registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});

    PriceBatchResult results = price_many(
        fx.registries, products, std::vector<engine::MeasureSpec>{{"DV01", Params{{"bump", bump}}}}, *model, market,
        pricing, execution
    );

    ASSERT_EQ(results.size(), 2u);
    EXPECT_NEAR(results[1].measures[0].result.scalar, results[0].measures[0].result.scalar, 1e-6);
}

} // namespace
