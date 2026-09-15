// Fase 11 (items pendientes del cierre documentado en PLAN_PRODUCTS.md §12): cablear
// payoff::api::payoff_sensitivity_gbm_q y payoff::hedge::synthesize_hedge_gbm_q al bridge cxx --
// tests directos (llamada libre de C++, sin pasar por Registry<IMeasure>) de
// `engine::payoff::payoff_sensitivity_gbm`/`engine::payoff::synthesize_hedge_gbm`, mismo estilo
// que test_gbm_measures.cpp (oraculo Black-Scholes independiente de la formula de produccion).

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "engine/model.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"

namespace {

namespace pf = engine::payoff;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

double black_scholes_call(double s0, double strike, double r, double q, double sigma, double maturity) {
    double sqrt_t = std::sqrt(maturity);
    double d1 = (std::log(s0 / strike) + (r - q + 0.5 * sigma * sigma) * maturity) / (sigma * sqrt_t);
    double d2 = d1 - sigma * sqrt_t;
    return s0 * std::exp(-q * maturity) * norm_cdf(d1) - strike * std::exp(-r * maturity) * norm_cdf(d2);
}

pf::ContractPtr european_call(const pf::ObservableId& spot, double strike, double maturity) {
    return pf::when(
        tp(maturity),
        pf::cashflow(
            pf::Currency{"USD"},
            pf::maximum(pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(strike)), pf::constant(0.0))
        )
    );
}

engine::GbmModel make_gbm(double s0, double r, double q, double sigma, const std::string& observable) {
    return engine::GbmModel(engine::Params{
        {"s0", s0}, {"r", r}, {"q", q}, {"sigma", sigma}, {"observable", observable}
    });
}

// --- payoff_sensitivity_gbm ---------------------------------------------------------------

TEST(PayoffSensitivityGbmTest, SpotDeltaOfACallMatchesFiniteDifferenceOfBlackScholes) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::SensitivityResult result =
        pf::payoff_sensitivity_gbm(*product.payoff_program(), model, "spot", 400'000, 7);

    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::RiskNeutralQ);
    const double bump = 1.0;
    double analytic_delta =
        (black_scholes_call(s0 + bump, strike, r, q, sigma, maturity) -
         black_scholes_call(s0 - bump, strike, r, q, sigma, maturity)) /
        (2.0 * bump);
    double tolerance = 8.0 * result.std_error + 0.02;
    EXPECT_NEAR(result.value, analytic_delta, tolerance)
        << "MC=" << result.value << " analytic=" << analytic_delta << " std_error=" << result.std_error;
}

TEST(PayoffSensitivityGbmTest, RejectsAnUnknownGreekName) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm(100.0, 0.05, 0.0, 0.2, spot.value);

    EXPECT_THROW(
        pf::payoff_sensitivity_gbm(*product.payoff_program(), model, "theta", 1'000, 7), pf::EvaluationError
    );
}

TEST(PayoffSensitivityGbmTest, PreflightRejectsObservableTheModelDoesNotGenerate) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.OTHER_TICKER");

    EXPECT_THROW(
        pf::payoff_sensitivity_gbm(*product.payoff_program(), model, "spot", 1'000, 7), pf::ValidationError
    );
}

// --- synthesize_hedge_gbm ------------------------------------------------------------------

TEST(SynthesizeHedgeGbmTest, HedgingACallWithItselfGivesWeightMinusOne) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    pf::PayoffProduct target("CALL", european_call(spot, strike, maturity));
    pf::PayoffProduct instrument("CALL_COPY", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::HedgeConstraints constraints;
    pf::HedgeResult result = pf::synthesize_hedge_gbm(
        *target.payoff_program(), {instrument.payoff_program()}, model, std::nullopt, 0.0, constraints, false,
        5'000, 7
    );

    ASSERT_EQ(result.weights.size(), 1u);
    EXPECT_NEAR(result.weights[0], -1.0, 1e-6);
    EXPECT_LT(result.residual_std, 1e-6);
    EXPECT_FALSE(result.cost.has_value());
    EXPECT_FALSE(result.residual_greeks.has_value());
}

TEST(SynthesizeHedgeGbmTest, ResidualGreeksOfHedgingACallWithItselfAreAllNearZero) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    pf::PayoffProduct target("CALL", european_call(spot, strike, maturity));
    pf::PayoffProduct instrument("CALL_COPY", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::HedgeConstraints constraints;
    pf::HedgeResult result = pf::synthesize_hedge_gbm(
        *target.payoff_program(), {instrument.payoff_program()}, model, std::nullopt, 0.0, constraints, true, 5'000,
        7
    );

    ASSERT_TRUE(result.residual_greeks.has_value());
    EXPECT_NEAR(result.residual_greeks->delta, 0.0, 1e-6);
    EXPECT_NEAR(result.residual_greeks->rho, 0.0, 1e-6);
    EXPECT_NEAR(result.residual_greeks->dividend_yield, 0.0, 1e-6);
    EXPECT_NEAR(result.residual_greeks->vega, 0.0, 1e-6);
}

TEST(SynthesizeHedgeGbmTest, BoundsPinTheWeightOfAnInstrumentToAFixedPosition) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    pf::PayoffProduct target("CALL_100", european_call(spot, 100.0, maturity));
    pf::PayoffProduct instrument("CALL_90", european_call(spot, 90.0, maturity));
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::HedgeConstraints constraints;
    constraints.bounds = {{-0.1, -0.1}};
    pf::HedgeResult result = pf::synthesize_hedge_gbm(
        *target.payoff_program(), {instrument.payoff_program()}, model, std::nullopt, 0.0, constraints, false,
        2'000, 5
    );

    ASSERT_EQ(result.weights.size(), 1u);
    EXPECT_NEAR(result.weights[0], -0.1, 1e-6);
}

TEST(SynthesizeHedgeGbmTest, MaxGrossNotionalScalesWeightsDownAndReportsCost) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    pf::PayoffProduct target("CALL_100", european_call(spot, 100.0, maturity));
    pf::PayoffProduct instrument("CALL_90", european_call(spot, 90.0, maturity));
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);
    std::vector<double> prices{12.0};

    pf::HedgeConstraints unconstrained;
    pf::HedgeResult base = pf::synthesize_hedge_gbm(
        *target.payoff_program(), {instrument.payoff_program()}, model, prices, 0.01, unconstrained, false, 5'000, 13
    );
    ASSERT_TRUE(base.gross_notional.has_value());

    pf::HedgeConstraints capped;
    capped.max_gross_notional = *base.gross_notional / 2.0;
    pf::HedgeResult scaled = pf::synthesize_hedge_gbm(
        *target.payoff_program(), {instrument.payoff_program()}, model, prices, 0.01, capped, false, 5'000, 13
    );

    ASSERT_TRUE(scaled.gross_notional.has_value());
    EXPECT_NEAR(*scaled.gross_notional, *capped.max_gross_notional, 1e-6);
    ASSERT_EQ(scaled.weights.size(), base.weights.size());
    EXPECT_NEAR(scaled.weights[0], base.weights[0] * 0.5, 1e-6);
}

TEST(SynthesizeHedgeGbmTest, PreflightRejectsAnInstrumentReferencingAnObservableTheModelDoesNotGenerate) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const pf::ObservableId other{"EQ.SPOT.OTHER"};
    pf::PayoffProduct target("CALL", european_call(spot, 100.0, 1.0));
    pf::PayoffProduct bad_instrument("BAD", european_call(other, 100.0, 1.0));
    engine::GbmModel model = make_gbm(100.0, 0.05, 0.0, 0.2, spot.value);

    pf::HedgeConstraints constraints;
    EXPECT_THROW(
        pf::synthesize_hedge_gbm(
            *target.payoff_program(), {bad_instrument.payoff_program()}, model, std::nullopt, 0.0, constraints,
            false, 1'000, 7
        ),
        pf::ValidationError
    );
}

} // namespace
