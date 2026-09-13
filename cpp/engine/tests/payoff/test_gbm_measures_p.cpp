// Medidas bajo P via Monte Carlo (GBM fisico) de un PayoffProgram (PLAN_PRODUCTS.md §12
// Fase 7): engine::payoff::forecast_gbm_p/hit_probability_gbm(GbmPModel)/pnl_distribution_gbm_p,
// preflight de ModelCapabilities y el criterio de aceptacion explicito de esta fase --
// "el motor rechaza combinaciones Q/P invalidas y muestra diferencias esperadas de hit/P&L en un
// fixture con drift P distinto de r-q" -- ver test_gbm_measures.cpp para el equivalente bajo Q,
// del que este archivo es deliberadamente paralelo.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "engine/model.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/payoff/tp_sl.hpp"

namespace {

namespace pf = engine::payoff;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

engine::GbmModel make_gbm_q(double s0, double r, double q, double sigma, const std::string& observable) {
    return engine::GbmModel(engine::Params{
        {"s0", s0}, {"r", r}, {"q", q}, {"sigma", sigma}, {"observable", observable}
    });
}

engine::GbmPModel make_gbm_p(double s0, double mu, double sigma, const std::string& observable) {
    return engine::GbmPModel(engine::Params{{"s0", s0}, {"mu", mu}, {"sigma", sigma}, {"observable", observable}});
}

std::vector<pf::TimePoint> monitoring_grid(int n) {
    std::vector<pf::TimePoint> times;
    times.reserve(n);
    for (int i = 1; i <= n; ++i) times.push_back(tp(static_cast<double>(i) / n));
    return times;
}

pf::templates::TpSlSpec tp_sl_fixture(const pf::ObservableId& spot, const std::vector<pf::TimePoint>& monitoring_times) {
    return pf::templates::TpSlSpec{
        spot,
        pf::templates::TpSlMetric::ReturnFromEntry,
        /*entry_price=*/100.0,
        /*quantity=*/1.0,
        /*take_profit_level=*/0.20,
        /*stop_loss_level=*/-0.10,
        monitoring_times,
        pf::Currency{"USD"},
        pf::EventId{"TAKE_PROFIT"},
        pf::EventId{"STOP_LOSS"},
    };
}

// PLAN_PRODUCTS.md §12 Fase 7, criterio de aceptacion: "el motor rechaza combinaciones Q/P
// invalidas". A diferencia de una medida generica despachada dinamicamente (eso es Fase 8+,
// Registry<IMeasure>), las medidas GBM de Fase 5-7 son funciones tipadas por modelo concreto
// (GbmModel para Q, GbmPModel para P): la combinacion invalida "pedir RiskNeutralQ a un modelo
// que solo declara PhysicalP" ya es un error de COMPILACION (no existe overload de
// risk_neutral_price_gbm que acepte GbmPModel, ni de forecast_gbm_p/hit_probability_gbm(P) que
// acepte GbmModel) -- mas fuerte que un rechazo en tiempo de ejecucion. Este test fija la
// garantia en el unico punto donde es observable en tiempo de ejecucion: las `capabilities()`
// que alimentan el preflight (measures.cpp::preflight_gbm_capabilities) son disjuntas.
TEST(ModelCapabilitiesTest, GbmQAndGbmPDeclareDisjointSupportedMeasures) {
    engine::GbmModel q_model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.XYZ");
    engine::GbmPModel p_model = make_gbm_p(100.0, 0.05, 0.2, "EQ.SPOT.XYZ");

    auto q_caps = q_model.capabilities();
    auto p_caps = p_model.capabilities();
    ASSERT_TRUE(q_caps.has_value());
    ASSERT_TRUE(p_caps.has_value());

    EXPECT_TRUE(q_caps->supported_measures.count(pf::ProbabilityMeasure::RiskNeutralQ));
    EXPECT_FALSE(q_caps->supported_measures.count(pf::ProbabilityMeasure::PhysicalP));
    EXPECT_TRUE(p_caps->supported_measures.count(pf::ProbabilityMeasure::PhysicalP));
    EXPECT_FALSE(p_caps->supported_measures.count(pf::ProbabilityMeasure::RiskNeutralQ));
}

TEST(ForecastGbmPMeasureTest, MatchesTheAnalyticExpectedTerminalSpotUnderThePhysicalDrift) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, mu = 0.30, sigma = 0.2, maturity = 1.0;

    // Contrato que paga el spot terminal sin transformar: E_P[cashflow] = E_P[S_T] = S0*exp(mu*T)
    // en forma cerrada (identico razonamiento que black_scholes_call en test_gbm_measures.cpp,
    // pero sin descuento -- Forecast bajo P nunca descuenta, ver el doc-comment de measures.hpp).
    pf::ContractPtr pays_terminal_spot = pf::when(tp(maturity), pf::cashflow(pf::Currency{"USD"}, pf::fixing(spot, tp(maturity))));
    pf::PayoffProduct product("TERMINAL_SPOT", pays_terminal_spot);
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);

    pf::ForecastResult result = pf::forecast_gbm_p(*product.payoff_program(), model, 200'000, 7);

    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::PhysicalP);
    EXPECT_EQ(result.n_paths, 200'000u);
    EXPECT_GT(result.std_error, 0.0);

    double analytic = s0 * std::exp(mu * maturity);
    double tolerance = 8.0 * result.std_error;
    EXPECT_NEAR(result.mean, analytic, tolerance)
        << "MC=" << result.mean << " analytic=" << analytic << " std_error=" << result.std_error;
}

TEST(ForecastGbmPMeasureTest, PreflightRejectsObservableTheModelDoesNotGenerate) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::ContractPtr contract = pf::when(tp(1.0), pf::cashflow(pf::Currency{"USD"}, pf::fixing(spot, tp(1.0))));
    pf::PayoffProduct product("TERMINAL_SPOT", contract);
    // El modelo genera un observable DISTINTO al que referencia el contrato.
    engine::GbmPModel model = make_gbm_p(100.0, 0.05, 0.2, "EQ.SPOT.OTHER_TICKER");

    EXPECT_THROW(pf::forecast_gbm_p(*product.payoff_program(), model, 1'000, 7), pf::ValidationError);
}

// PLAN_PRODUCTS.md §12 Fase 7, criterio de aceptacion: "muestra diferencias esperadas de hit ...
// en un fixture con drift P distinto de r-q". Mismo grupo TP/SL (§4.3, take profit +20%/stop
// loss -10% sobre el retorno desde entrada), dos drifts fisicos: uno apenas por encima de cero
// (comparable a un `r-q` tipico de Q) y otro claramente alcista -- con el mismo sigma, el drift
// mas alcista debe alcanzar el take-profit con mas frecuencia.
TEST(HitProbabilityGbmPMeasureTest, AMoreBullishPhysicalDriftIncreasesTheTakeProfitHitProbability) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, sigma = 0.2;
    std::vector<pf::TimePoint> monitoring_times = monitoring_grid(50);

    pf::templates::TpSlSpec spec = tp_sl_fixture(spot, monitoring_times);
    pf::ContractPtr tp_sl_contract = pf::templates::first_of_take_profit_stop_loss(spec);
    pf::PayoffProduct product("TP_SL", tp_sl_contract);

    const std::uint64_t n_paths = 200'000, seed = 7;
    // r-q comparable de una configuracion Q tipica (0.05): mu_low apenas por encima, mu_high
    // claramente distinto de ese r-q -- el propio criterio de aceptacion pide un drift P
    // "distinto de r-q", no solo "positivo".
    engine::GbmPModel low_drift_model = make_gbm_p(s0, 0.02, sigma, spot.value);
    engine::GbmPModel high_drift_model = make_gbm_p(s0, 0.40, sigma, spot.value);

    pf::HitProbabilityResult low_drift =
        pf::hit_probability_gbm(*product.payoff_program(), low_drift_model, spec.take_profit_id, n_paths, seed);
    pf::HitProbabilityResult high_drift =
        pf::hit_probability_gbm(*product.payoff_program(), high_drift_model, spec.take_profit_id, n_paths, seed);

    EXPECT_EQ(low_drift.measure, pf::ProbabilityMeasure::PhysicalP);
    double tolerance = 8.0 * (low_drift.std_error + high_drift.std_error);
    EXPECT_GT(high_drift.probability, low_drift.probability + tolerance)
        << "low_drift=" << low_drift.probability << " high_drift=" << high_drift.probability;
}

TEST(HitProbabilityGbmPMeasureTest, RejectsAnEventThatDoesNotExistInTheContract) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    std::vector<pf::TimePoint> monitoring_times = monitoring_grid(10);
    pf::templates::TpSlSpec spec = tp_sl_fixture(spot, monitoring_times);
    pf::PayoffProduct product("TP_SL", pf::templates::first_of_take_profit_stop_loss(spec));
    engine::GbmPModel model = make_gbm_p(100.0, 0.05, 0.2, spot.value);

    EXPECT_THROW(
        pf::hit_probability_gbm(*product.payoff_program(), model, pf::EventId{"NOT_A_REAL_EVENT"}, 1'000, 7),
        pf::EvaluationError
    );
}

// PLAN_PRODUCTS.md §12 Fase 7, criterio de aceptacion: "... y P&L en un fixture con drift P
// distinto de r-q". Mismo grupo TP/SL: el P&L esperado de la estrategia crece con un drift mas
// alcista (mas rutas cierran en take-profit en vez de stop-loss o sin cerrar).
TEST(PnlDistributionGbmPMeasureTest, ForecastPnlIsHigherUnderAMoreBullishPhysicalDrift) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, sigma = 0.2;
    std::vector<pf::TimePoint> monitoring_times = monitoring_grid(50);
    pf::templates::TpSlSpec spec = tp_sl_fixture(spot, monitoring_times);
    pf::PayoffProduct product("TP_SL", pf::templates::first_of_take_profit_stop_loss(spec));

    const std::uint64_t n_paths = 200'000, seed = 11;
    engine::GbmPModel low_drift_model = make_gbm_p(s0, 0.02, sigma, spot.value);
    engine::GbmPModel high_drift_model = make_gbm_p(s0, 0.40, sigma, spot.value);

    pf::ForecastResult low_drift = pf::forecast_gbm_p(*product.payoff_program(), low_drift_model, n_paths, seed);
    pf::ForecastResult high_drift = pf::forecast_gbm_p(*product.payoff_program(), high_drift_model, n_paths, seed);

    double tolerance = 8.0 * (low_drift.std_error + high_drift.std_error);
    EXPECT_GT(high_drift.mean, low_drift.mean + tolerance)
        << "low_drift=" << low_drift.mean << " high_drift=" << high_drift.mean;
}

TEST(PnlDistributionGbmPMeasureTest, ExpectedShortfallIsAtLeastAsSevereAsValueAtRisk) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    std::vector<pf::TimePoint> monitoring_times = monitoring_grid(50);
    pf::templates::TpSlSpec spec = tp_sl_fixture(spot, monitoring_times);
    pf::PayoffProduct product("TP_SL", pf::templates::first_of_take_profit_stop_loss(spec));
    engine::GbmPModel model = make_gbm_p(100.0, 0.05, 0.2, spot.value);

    pf::PnlDistributionResult dist =
        pf::pnl_distribution_gbm_p(*product.payoff_program(), model, 100'000, 13, 0.95);

    EXPECT_EQ(dist.measure, pf::ProbabilityMeasure::PhysicalP);
    EXPECT_GE(dist.es, dist.var) << "es=" << dist.es << " deberia ser >= var=" << dist.var;
}

} // namespace
