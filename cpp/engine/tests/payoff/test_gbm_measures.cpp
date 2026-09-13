// Precio bajo Q via Monte Carlo (GBM) de un PayoffProgram (PLAN_PRODUCTS.md §12 Fase 5):
// engine::payoff::risk_neutral_price_gbm, preflight de ModelCapabilities y convergencia contra
// Black-Scholes calculado de forma independiente en este archivo (mismo criterio que
// test_measures.cpp: el oraculo del test nunca reutiliza la formula de produccion, ver
// rust/crates/engine-core/src/models/gbm.rs::black_scholes_call, que usa una aproximacion
// racional distinta de std::erfc).

#include <gtest/gtest.h>

#include <cmath>

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

TEST(RiskNeutralGbmMeasureTest, MonteCarloConvergesToBlackScholes) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("TEST_CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::QValuationResult result = pf::risk_neutral_price_gbm(*product.payoff_program(), model, 200'000, 7);

    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::RiskNeutralQ);
    EXPECT_EQ(result.n_paths, 200'000u);
    EXPECT_GT(result.std_error, 0.0);

    double analytic = black_scholes_call(s0, strike, r, q, sigma, maturity);
    double tolerance = 8.0 * result.std_error;
    EXPECT_NEAR(result.mean, analytic, tolerance)
        << "MC=" << result.mean << " analytic=" << analytic << " std_error=" << result.std_error;
}

TEST(RiskNeutralGbmMeasureTest, PreflightRejectsObservableTheModelDoesNotGenerate) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("TEST_CALL", european_call(spot, 100.0, 1.0));
    // El modelo genera un observable DISTINTO al que referencia el contrato -- preflight debe
    // fallar sin llegar a serializar el JSON ni cruzar la frontera hacia Rust.
    engine::GbmModel model = make_gbm(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.OTHER_TICKER");

    EXPECT_THROW(pf::risk_neutral_price_gbm(*product.payoff_program(), model, 1'000, 7), pf::ValidationError);
}

} // namespace
