// Medidas deterministas genericas (PLAN_PRODUCTS.md §12 Fase 4): Cashflows, ScenarioPayoff, PV
// por ledger, DependencyReport y bump-and-reval genericos sobre call/forward/FXForward.

#include <gtest/gtest.h>

#include <cmath>

#include "engine/payoff/contract.hpp"
#include "engine/payoff/event_state.hpp"
#include "engine/payoff/evaluation_context.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/market_path.hpp"
#include "engine/payoff/measures.hpp"

namespace {

namespace pf = engine::payoff;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

TEST(GenericMeasuresTest, EuropeanCallCashflowsAndPresentValue) {
    const pf::Currency usd{"USD"};
    const pf::CurveId curve{"IR.DF.USD.OIS"};
    const pf::ObservableId spot_id{"EQ.SPOT.AAPL"};
    const double strike = 100.0;
    const double spot_value = 120.0;
    const double df = 0.95;

    pf::ContractPtr call = pf::when(
        tp(1.0),
        pf::cashflow(usd, pf::maximum(pf::sub(pf::fixing(spot_id, tp(1.0)), pf::constant(strike)), pf::constant(0.0)))
    );

    pf::MarketPath path;
    path.set_fixing(spot_id, tp(1.0), spot_value);
    path.set_discount_factor(curve, tp(0.0), tp(1.0), df);

    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::CashflowLedger ledger = pf::evaluate_cashflows(call, context);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, spot_value - strike);
    EXPECT_EQ(ledger[0].currency, usd);

    pf::DiscountingPolicy discounting(usd, curve);
    pf::ValuationResult result = pf::present_value(call, context, discounting, usd);
    EXPECT_DOUBLE_EQ(result.present_value, (spot_value - strike) * df);
    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::DeterministicScenario);
    EXPECT_EQ(result.reporting_currency, usd);
    EXPECT_EQ(result.dependencies.observables.count(spot_id), 1u);
    EXPECT_EQ(result.dependencies.currencies.count(usd), 1u);

    // Bump-and-reval: subir el spot en una call ITM sube la PV en df (pendiente 1 del payoff
    // intrinseco); subir el tipo cero de la curva de descuento baja la PV.
    double delta = pf::bump_and_reval_fixing(call, path, historical, discounting, usd, spot_id, 1.0);
    EXPECT_NEAR(delta, df, 1e-9);

    double dv01 = pf::bump_and_reval_curve(call, path, historical, discounting, usd, curve, 0.0001);
    EXPECT_LT(dv01, 0.0);
}

TEST(GenericMeasuresTest, ForwardMatchesClosedForm) {
    const pf::Currency usd{"USD"};
    const pf::CurveId curve{"IR.DF.USD.OIS"};
    const pf::ObservableId spot_id{"EQ.SPOT.AAPL"};
    const double strike = 105.0;
    const double spot_value = 110.0;
    const double df = 0.98;

    pf::ContractPtr forward = pf::when(tp(2.0), pf::cashflow(usd, pf::sub(pf::fixing(spot_id, tp(2.0)), pf::constant(strike))));

    pf::MarketPath path;
    path.set_fixing(spot_id, tp(2.0), spot_value);
    path.set_discount_factor(curve, tp(0.0), tp(2.0), df);

    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::DiscountingPolicy discounting(usd, curve);
    pf::ValuationResult result = pf::present_value(forward, context, discounting, usd);
    EXPECT_DOUBLE_EQ(result.present_value, (spot_value - strike) * df);
}

// FXForward multi-moneda (PLAN_PRODUCTS.md §9.4): pata extranjera + pata domestica, reportado
// en la moneda domestica -- ejercita fx_rate() y dos curvas de descuento distintas dentro de
// discount_and_convert().
TEST(GenericMeasuresTest, FxForwardMultiCurrency) {
    const pf::Currency eur{"EUR"};
    const pf::Currency usd{"USD"};
    const pf::CurveId eur_curve{"IR.DF.EUR.OIS"};
    const pf::CurveId usd_curve{"IR.DF.USD.OIS"};
    const double notional_for = 1'000'000.0;
    const double strike = 1.10; // USD por EUR
    const double sign = 1.0; // largo EUR
    const double eur_usd_fx = 1.12;
    const double df_eur = 0.99;
    const double df_usd = 0.97;

    pf::ContractPtr fx_forward = pf::when(
        tp(1.0),
        pf::both({
            pf::cashflow(eur, pf::constant(sign * notional_for)),
            pf::cashflow(usd, pf::constant(-sign * notional_for * strike)),
        })
    );

    pf::MarketPath path;
    path.set_discount_factor(eur_curve, tp(0.0), tp(1.0), df_eur);
    path.set_discount_factor(usd_curve, tp(0.0), tp(1.0), df_usd);
    path.set_fx_rate(eur, usd, tp(1.0), eur_usd_fx);

    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::DiscountingPolicy discounting;
    discounting.set_curve(eur, eur_curve);
    discounting.set_curve(usd, usd_curve);

    pf::ValuationResult result = pf::present_value(fx_forward, context, discounting, usd);

    double expected = sign * notional_for * eur_usd_fx * df_eur - sign * notional_for * strike * df_usd;
    EXPECT_NEAR(result.present_value, expected, 1e-6);
    EXPECT_EQ(result.dependencies.currencies.count(eur), 1u);
    EXPECT_EQ(result.dependencies.currencies.count(usd), 1u);
}

TEST(GenericMeasuresTest, DiscountingPolicyRejectsUnknownCurrency) {
    pf::DiscountingPolicy discounting(pf::Currency{"USD"}, pf::CurveId{"IR.DF.USD.OIS"});
    EXPECT_THROW(discounting.curve_for(pf::Currency{"EUR"}), std::invalid_argument);
}

} // namespace
