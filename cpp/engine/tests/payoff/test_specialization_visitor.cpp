// Equivalencia a tres bandas para el IRS (PLAN_PRODUCTS.md §9.3, Fase 8, criterio de aceptacion
// explicito de §12 Fase 8: "tests comparan AST generico, kernel especializado y resultados
// legacy"): kernel legacy (IrSwapProduct + PresentValueMeasure/Dv01Measure sobre HullWhite1F),
// AST generico (templates::irs_swap + present_value/bump_and_reval_curve, ya cubierto en
// test_irs_templates.cpp) y el reconocedor especializado (specialization::recognize_irs_swap +
// specialized_present_value/specialized_dv01) deben coincidir.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/payoff/irs_templates.hpp"
#include "engine/payoff/specialization_visitor.hpp"
#include "engine/price.hpp"

namespace {

namespace pf = engine::payoff;
namespace tmpl = engine::payoff::templates;
namespace spec = engine::payoff::specialization;

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

struct SpecializationFixture {
    double notional = 1'000'000.0;
    double fixed_rate = 0.03;
    double start = 0.0;
    std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};
    pf::Currency ccy{"USD"};

    Registries registries;

    SpecializationFixture() { register_builtins(registries); }

    MarketSnapshot market() const { return MarketSnapshot({1.0}, {0.02}); }

    std::vector<pf::TimePoint> payment_time_points() const {
        std::vector<pf::TimePoint> out;
        out.reserve(payment_times.size());
        for (double t : payment_times) out.push_back(pf::TimePoint{t});
        return out;
    }

    pf::ContractPtr ast() const {
        tmpl::IrsSwapSpec irs_spec{ccy, notional, fixed_rate, pf::TimePoint{start}, payment_time_points(), accruals};
        return tmpl::irs_swap(irs_spec);
    }

    double legacy_pv(const MarketSnapshot& mkt) const {
        auto model = registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
        auto product = registries.products.create(
            "IRSwap", Params{
                          {"notional", notional},
                          {"fixed_rate", fixed_rate},
                          {"start", start},
                          {"payment_times", payment_times},
                          {"accruals", accruals},
                      }
        );
        auto pv_measure = registries.measures.create("PV");
        PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
        ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
        engine::MeasureResult result = pv_measure->evaluate(*model, *product, mkt, pricing, execution);
        return result.scalar;
    }

    double legacy_dv01(const MarketSnapshot& mkt, double bump) const {
        auto model = registries.models.create("HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
        auto product = registries.products.create(
            "IRSwap", Params{
                          {"notional", notional},
                          {"fixed_rate", fixed_rate},
                          {"start", start},
                          {"payment_times", payment_times},
                          {"accruals", accruals},
                      }
        );
        auto dv01_measure = registries.measures.create("DV01", Params{{"bump", bump}});
        PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
        ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
        engine::MeasureResult result = dv01_measure->evaluate(*model, *product, mkt, pricing, execution);
        return result.scalar;
    }
};

TEST(SpecializationVisitorTest, RecognizesTheShapeProducedByIrsSwapTemplate) {
    SpecializationFixture fx;
    std::optional<spec::RecognizedIrsSwap> recognized = spec::recognize_irs_swap(fx.ast());

    ASSERT_TRUE(recognized.has_value());
    EXPECT_EQ(recognized->currency, fx.ccy);
    EXPECT_DOUBLE_EQ(recognized->notional, fx.notional);
    EXPECT_DOUBLE_EQ(recognized->start.year_fraction, fx.start);
    EXPECT_DOUBLE_EQ(recognized->end.year_fraction, fx.payment_times.back());
    ASSERT_EQ(recognized->fixed_payments.size(), fx.payment_times.size());
    for (std::size_t i = 0; i < fx.payment_times.size(); ++i) {
        EXPECT_DOUBLE_EQ(recognized->fixed_payments[i].first.year_fraction, fx.payment_times[i]);
        EXPECT_DOUBLE_EQ(recognized->fixed_payments[i].second, fx.notional * fx.fixed_rate * fx.accruals[i]);
    }
}

TEST(SpecializationVisitorTest, DoesNotRecognizeAnUnrelatedContract) {
    pf::ContractPtr not_an_irs = pf::when(pf::TimePoint{1.0}, pf::cashflow(pf::Currency{"USD"}, pf::constant(100.0)));
    EXPECT_FALSE(spec::recognize_irs_swap(not_an_irs).has_value());
}

TEST(SpecializationVisitorTest, PresentValueMatchesLegacyAndGenericAst) {
    SpecializationFixture fx;
    MarketSnapshot market = fx.market();

    std::optional<spec::RecognizedIrsSwap> recognized = spec::recognize_irs_swap(fx.ast());
    ASSERT_TRUE(recognized.has_value());
    double specialized_pv = spec::specialized_present_value(*recognized, market);

    EXPECT_NEAR(specialized_pv, fx.legacy_pv(market), 1e-6);
}

TEST(SpecializationVisitorTest, Dv01MatchesLegacyAndGenericAst) {
    SpecializationFixture fx;
    MarketSnapshot market = fx.market();
    const double bump = 0.0001;

    std::optional<spec::RecognizedIrsSwap> recognized = spec::recognize_irs_swap(fx.ast());
    ASSERT_TRUE(recognized.has_value());
    double specialized = spec::specialized_dv01(*recognized, market, bump);

    EXPECT_NEAR(specialized, fx.legacy_dv01(market, bump), 1e-6);
}

} // namespace
