// market_snapshot_bridge (PLAN_PRODUCTS.md §9.5, version minima de Fase 8): puente
// MarketSnapshot -> motor de payoff para contratos deterministas de una unica moneda.

#include <gtest/gtest.h>

#include "engine/market.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/fxforward_templates.hpp"
#include "engine/payoff/irs_templates.hpp"
#include "engine/payoff/market_snapshot_bridge.hpp"

namespace {

namespace pf = engine::payoff;
namespace tmpl = engine::payoff::templates;

using engine::MarketSnapshot;

TEST(MarketSnapshotBridgeTest, PresentValueOfASimpleFixedCashflowMatchesClosedForm) {
    pf::Currency usd{"USD"};
    pf::ContractPtr bond = pf::when(pf::TimePoint{5.0}, pf::cashflow(usd, pf::constant(1'000'000.0)));

    MarketSnapshot market({1.0, 5.0}, {0.02, 0.025});

    pf::ValuationResult result = pf::present_value_from_market_snapshot(bond, market);

    EXPECT_NEAR(result.present_value, 1'000'000.0 * market.discount_factor(5.0), 1e-6);
    EXPECT_EQ(result.reporting_currency, usd);
}

TEST(MarketSnapshotBridgeTest, PresentValueOfIrsSwapTemplateMatchesLegacyFormula) {
    pf::Currency usd{"USD"};
    double notional = 1'000'000.0;
    double fixed_rate = 0.03;
    std::vector<pf::TimePoint> payment_times{pf::TimePoint{1.0}, pf::TimePoint{2.0}, pf::TimePoint{3.0}};
    std::vector<double> accruals{1.0, 1.0, 1.0};

    tmpl::IrsSwapSpec spec{usd, notional, fixed_rate, pf::TimePoint{0.0}, payment_times, accruals};
    pf::ContractPtr swap = tmpl::irs_swap(spec);

    MarketSnapshot market({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022});

    pf::ValuationResult result = pf::present_value_from_market_snapshot(swap, market);

    double floating_leg = notional * (market.discount_factor(0.0) - market.discount_factor(3.0));
    double fixed_leg = 0.0;
    for (std::size_t i = 0; i < payment_times.size(); ++i) {
        fixed_leg += notional * fixed_rate * accruals[i] * market.discount_factor(payment_times[i].year_fraction);
    }
    EXPECT_NEAR(result.present_value, floating_leg - fixed_leg, 1e-6);
}

TEST(MarketSnapshotBridgeTest, Dv01MatchesBumpAndRevalOfTheClosedForm) {
    pf::Currency usd{"USD"};
    pf::ContractPtr bond = pf::when(pf::TimePoint{5.0}, pf::cashflow(usd, pf::constant(1'000'000.0)));
    MarketSnapshot market({1.0, 5.0}, {0.02, 0.025});
    const double bump = 0.0001;

    double dv01 = pf::bump_and_reval_from_market_snapshot(bond, market, bump);

    MarketSnapshot bumped_market({1.0, 5.0}, {0.02 + bump, 0.025 + bump});
    double expected = 1'000'000.0 * (bumped_market.discount_factor(5.0) - market.discount_factor(5.0));
    EXPECT_NEAR(dv01, expected, 1e-6);
}

TEST(MarketSnapshotBridgeTest, RejectsAContractWithMoreThanOneCurrency) {
    // templates::fx_forward: exactamente el caso multi-moneda que este puente no cubre (ver
    // PLAN_FXFORWARD.md §3.2 para el caso completo, no implementado en Fase 8).
    tmpl::FxForwardSpec spec{
        pf::Currency{"EUR"}, pf::Currency{"USD"}, 1'000'000.0, 1.10, pf::TimePoint{1.0}, /*buy_foreign=*/true
    };
    pf::ContractPtr forward = tmpl::fx_forward(spec);

    MarketSnapshot market({1.0}, {0.02});

    EXPECT_THROW(pf::present_value_from_market_snapshot(forward, market), std::invalid_argument);
}

TEST(MarketSnapshotBridgeTest, RejectsAContractReferencingAnObservableMarketSnapshotDoesNotModel) {
    pf::ObservableId spot{"EQ.SPOT.AAPL"};
    pf::Currency usd{"USD"};
    pf::ContractPtr call = pf::when(
        pf::TimePoint{1.0}, pf::cashflow(usd, pf::maximum(pf::sub(pf::fixing(spot, pf::TimePoint{1.0}), pf::constant(100.0)), pf::constant(0.0)))
    );

    MarketSnapshot market({1.0}, {0.02});

    EXPECT_THROW(pf::present_value_from_market_snapshot(call, market), pf::EvaluationError);
}

} // namespace
