// Equivalencia legacy vs AST para IRS via template reutilizable (PLAN_PRODUCTS.md §12 Fase 8):
// generaliza `test_measure_cross_check.cpp` (AST construido a mano, solo PV) usando
// `templates::irs_swap` y añade la comparación de DV01 (bump-and-reval generico del AST vs.
// `Dv01Measure` legacy sobre el mismo mercado/bump).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/payoff/irs_templates.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/price.hpp"

namespace {

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

namespace pf = engine::payoff;
namespace tmpl = engine::payoff::templates;

struct IrsTemplateFixture {
    double notional = 1'000'000.0;
    double fixed_rate = 0.03;
    double start = 0.0;
    std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};

    pf::CurveId curve{"IR.DF.OIS"};
    pf::Currency ccy{"USD"};

    Registries registries;
    Params market_params{{"pillars", std::vector<double>{1.0}}, {"zero_rates", std::vector<double>{0.02}}};

    IrsTemplateFixture() { register_builtins(registries); }

    MarketSnapshot market() const { return MarketSnapshot({1.0}, {0.02}); }

    pf::MarketPath ast_path(const MarketSnapshot& mkt) const {
        pf::MarketPath path;
        path.set_discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{start}, mkt.discount_factor(start));
        for (double t : payment_times) {
            path.set_discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{t}, mkt.discount_factor(t));
        }
        return path;
    }

    std::vector<pf::TimePoint> payment_time_points() const {
        std::vector<pf::TimePoint> out;
        out.reserve(payment_times.size());
        for (double t : payment_times) out.push_back(pf::TimePoint{t});
        return out;
    }

    pf::ContractPtr ast() const {
        tmpl::IrsSwapSpec spec{ccy, notional, fixed_rate, pf::TimePoint{start}, payment_time_points(), accruals};
        return tmpl::irs_swap(spec);
    }
};

TEST(IrsTemplateTest, PresentValueMatchesLegacy) {
    IrsTemplateFixture fx;
    MarketSnapshot market = fx.market();

    auto model = fx.registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
    auto product = fx.registries.products.create(
        "IRSwap", Params{
                      {"notional", fx.notional},
                      {"fixed_rate", fx.fixed_rate},
                      {"start", fx.start},
                      {"payment_times", fx.payment_times},
                      {"accruals", fx.accruals},
                  }
    );
    auto pv_measure = fx.registries.measures.create("PV");
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
    engine::MeasureResult legacy_pv = pv_measure->evaluate(*model, *product, market, pricing, execution);
    ASSERT_TRUE(legacy_pv.has_scalar);

    pf::MarketPath path = fx.ast_path(market);
    path.set_discount_factor(fx.curve, pf::TimePoint{0.0}, pf::TimePoint{0.0}, 1.0);
    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};
    pf::DiscountingPolicy discounting(fx.ccy, fx.curve);
    pf::ValuationResult ast_pv = pf::present_value(fx.ast(), context, discounting, fx.ccy);

    EXPECT_NEAR(ast_pv.present_value, legacy_pv.scalar, 1e-6);
}

TEST(IrsTemplateTest, Dv01MatchesLegacy) {
    IrsTemplateFixture fx;
    MarketSnapshot market = fx.market();
    const double bump = 0.0001;

    auto model = fx.registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
    auto product = fx.registries.products.create(
        "IRSwap", Params{
                      {"notional", fx.notional},
                      {"fixed_rate", fx.fixed_rate},
                      {"start", fx.start},
                      {"payment_times", fx.payment_times},
                      {"accruals", fx.accruals},
                  }
    );
    auto dv01_measure = fx.registries.measures.create("DV01", Params{{"bump", bump}});
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
    engine::MeasureResult legacy_dv01 = dv01_measure->evaluate(*model, *product, market, pricing, execution);
    ASSERT_TRUE(legacy_dv01.has_scalar);

    pf::MarketPath path = fx.ast_path(market);
    path.set_discount_factor(fx.curve, pf::TimePoint{0.0}, pf::TimePoint{0.0}, 1.0);
    pf::FixingStore historical;
    pf::DiscountingPolicy discounting(fx.ccy, fx.curve);
    double ast_dv01 = pf::bump_and_reval_curve(fx.ast(), path, historical, discounting, fx.ccy, fx.curve, bump);

    EXPECT_NEAR(ast_dv01, legacy_dv01.scalar, 1e-6);
}

} // namespace
