// Precio bajo Q via Monte Carlo (GBM) de un PayoffProgram (PLAN_PRODUCTS.md §12 Fase 5):
// engine::payoff::risk_neutral_price_gbm, preflight de ModelCapabilities y convergencia contra
// Black-Scholes calculado de forma independiente en este archivo (mismo criterio que
// test_measures.cpp: el oraculo del test nunca reutiliza la formula de produccion, ver
// rust/crates/engine-core/src/models/gbm.rs::black_scholes_call, que usa una aproximacion
// racional distinta de std::erfc).

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "engine/model.hpp"
#include "engine/payoff/barrier_templates.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/payoff/predicate.hpp"

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

// PLAN_PRODUCTS.md §12 Fase 6 ("barrera discreta vectorizada bajo Q"): un AST de barrera
// construido en C++ (plantilla `templates::up_and_in` + un `up-and-out` armado a mano con el
// builder de bajo nivel `trigger`, ver §4.2) cruza a Rust como JSON, se compila/evalua alli
// (ver rust/crates/engine-core/src/payoff/{compile,eval}.rs) y vuelve como `QValuationResult` --
// confirma el camino completo C++ AST -> JSON -> Rust IR -> Monte Carlo para `Trigger`, no solo
// el lado Rust (ya cubierto en rust/.../payoff/api.rs). §13.2: "un knock-in + knock-out
// complementarios reproducen el underlying" -- UI + UO reproduce la vanilla dentro del error
// estandar combinado (ambas simulaciones son GBM exacto, no hay sesgo de discretizacion).
TEST(RiskNeutralGbmBarrierMeasureTest, UpAndInPlusUpAndOutReproducesVanillaCallUnderQ) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, barrier = 120.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const std::vector<pf::TimePoint> monitoring_times{tp(0.25), tp(0.5), tp(0.75), tp(1.0)};

    pf::ContractPtr vanilla = european_call(spot, strike, maturity);

    pf::ContractPtr up_and_in =
        pf::templates::up_and_in(pf::EventId{"UI"}, spot, barrier, monitoring_times, vanilla);

    // No hay plantilla `up_and_out` en barrier_templates.hpp (solo down_and_out) -- se arma con
    // el builder de bajo nivel `trigger` (§7.1): mismo predicado que `up_and_in`, on_hit/on_miss
    // intercambiados (se desactiva -- paga Zero -- si toca la barrera; paga la vanilla si nunca
    // la toca).
    pf::PredicatePtr up_and_out_condition = pf::greater_equal(pf::current(spot), pf::constant(barrier));
    pf::TriggerSpec up_and_out_spec{
        pf::EventId{"UO"}, monitoring_times, up_and_out_condition, pf::Monitoring::Discrete, pf::Settlement::AtHit,
        0, true
    };
    pf::ContractPtr up_and_out = pf::trigger(up_and_out_spec, pf::zero(), vanilla);

    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);
    const std::uint64_t n_paths = 300'000;

    pf::PayoffProduct ui_product("UI", up_and_in);
    pf::PayoffProduct uo_product("UO", up_and_out);
    pf::PayoffProduct vanilla_product("VANILLA", vanilla);

    pf::QValuationResult ui_result = pf::risk_neutral_price_gbm(*ui_product.payoff_program(), model, n_paths, 7);
    pf::QValuationResult uo_result = pf::risk_neutral_price_gbm(*uo_product.payoff_program(), model, n_paths, 8);
    pf::QValuationResult vanilla_result =
        pf::risk_neutral_price_gbm(*vanilla_product.payoff_program(), model, n_paths, 9);

    double combined_std_error = std::sqrt(
        ui_result.std_error * ui_result.std_error + uo_result.std_error * uo_result.std_error +
        vanilla_result.std_error * vanilla_result.std_error
    );
    double tolerance = 8.0 * combined_std_error;
    EXPECT_NEAR(ui_result.mean + uo_result.mean, vanilla_result.mean, tolerance)
        << "ui=" << ui_result.mean << " uo=" << uo_result.mean << " vanilla=" << vanilla_result.mean;
}

} // namespace
