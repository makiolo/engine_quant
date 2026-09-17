// Fase 1 de PLAN_GREEKS.md: `IModel::to_params()` + motor de bump-and-reval genérico, limitado a
// `RiskFactorKind::ModelParameter`/orden 1/`GreekMethod::BumpAndReval`, sobre métricas sin
// parámetros propios ("PV"/"PayoffPriceQ"/"PayoffForecastP"). Criterios de aceptación explícitos
// de la fase: Delta/Vega/Rho de una call vía "Greek" coinciden (dentro de tolerancia MC) con
// `PayoffSensitivityQ` existente y con Black-Scholes cerrado; `bump_used`/`method_used` correctos
// en `GreekResult`; `compute_all_greeks("PayoffPriceQ", ..., Gbm)` devuelve exactamente
// {spot, rate, dividend_yield, volatility}, ninguno en `skipped`.
//
// Fase 2 de PLAN_GREEKS.md (`GreeksFase2Test` más abajo): extiende `compute_greek`/`GreekMeasure`
// a métricas CON parámetros propios reenviados vía el prefijo `metric.*` ("event" de
// `PayoffHitProbabilityQ`, "exposure_times" de `PayoffExposureProfileQ`, "confidence" de
// `PayoffPnlDistributionP`) y a métricas cuyo `MeasureResult` no es puramente escalar (perfil
// `times`/`primary`/`secondary`, o escalar+perfil a la vez como `PayoffPnlDistributionP`).
// Criterio de aceptación explícito: sensibilidad de una probabilidad de hit a la volatilidad, de
// un VaR/ES a `mu`, y de un perfil de exposición a `sigma`, las tres vía "Greek" sin código nuevo
// por combinación -- verificadas contra un oráculo de bump-and-reval manual construido con el
// mismo `bump_override` (números aleatorios comunes: mismo seed/n_paths en ambas evaluaciones).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/abi.h"
#include "engine/bootstrap.hpp"
#include "engine/greeks.hpp"
#include "engine/model.hpp"
#include "engine/payoff/barrier_templates.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/price.hpp"
#include "engine/product.hpp"

namespace {

namespace pf = engine::payoff;

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;
using engine::greeks::GreekMethod;
using engine::greeks::GreekOrder;
using engine::greeks::GreekRequest;
using engine::greeks::RiskFactor;
using engine::greeks::RiskFactorKind;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

double norm_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }
double norm_pdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::acos(-1.0)); }

double black_scholes_gamma(double s0, double strike, double r, double q, double sigma, double maturity) {
    double sqrt_t = std::sqrt(maturity);
    double d1 = (std::log(s0 / strike) + (r - q + 0.5 * sigma * sigma) * maturity) / (sigma * sqrt_t);
    return std::exp(-q * maturity) * norm_pdf(d1) / (s0 * sigma * sqrt_t);
}

double black_scholes_call(double s0, double strike, double r, double q, double sigma, double maturity) {
    double sqrt_t = std::sqrt(maturity);
    double d1 = (std::log(s0 / strike) + (r - q + 0.5 * sigma * sigma) * maturity) / (sigma * sqrt_t);
    double d2 = d1 - sigma * sqrt_t;
    return s0 * std::exp(-q * maturity) * norm_cdf(d1) - strike * std::exp(-r * maturity) * norm_cdf(d2);
}

pf::ContractPtr european_call(const pf::ObservableId& spot, double strike, double maturity) {
    return pf::when(
        tp(maturity),
        pf::cashflow(pf::Currency{"USD"}, pf::maximum(pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(strike)), pf::constant(0.0)))
    );
}

engine::GbmModel make_gbm_q(double s0, double r, double q, double sigma, const std::string& observable) {
    return engine::GbmModel(Params{{"s0", s0}, {"r", r}, {"q", q}, {"sigma", sigma}, {"observable", observable}});
}

engine::GbmPModel make_gbm_p(double s0, double mu, double sigma, const std::string& observable) {
    return engine::GbmPModel(Params{{"s0", s0}, {"mu", mu}, {"sigma", sigma}, {"observable", observable}});
}

engine::HullWhite1FModel make_hull_white() {
    return engine::HullWhite1FModel(Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
}

PricingContext pricing_context(std::uint64_t n_paths, std::uint64_t seed) {
    return PricingContext(Params{
        {"pricing_date", 0.0}, {"n_paths", static_cast<double>(n_paths)}, {"n_steps", 1.0}, {"seed", static_cast<double>(seed)}
    });
}

ExecutionContext cpu_execution() {
    return ExecutionContext(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
}

MarketSnapshot flat_market() { return MarketSnapshot({1.0}, {0.02}); }

GreekRequest payoff_price_q_request(const std::string& risk_factor_name) {
    GreekRequest request;
    request.metric_name = "PayoffPriceQ";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", risk_factor_name, std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    return request;
}

// Contrato up-and-in usado por los tests de Fase 2 de PayoffHitProbabilityQ (mismo patrón que
// test_registry_wiring_payoff_measures.cpp).
pf::ContractPtr up_and_in_call(
    const pf::ObservableId& spot, double barrier, double strike, double maturity, const std::vector<pf::TimePoint>& monitoring_times
) {
    return pf::templates::up_and_in(pf::EventId{"UI"}, spot, barrier, monitoring_times, european_call(spot, strike, maturity));
}

class FakeProduct : public engine::IProduct {
public:
    std::string type_name() const override { return "Fake"; }
};

#ifndef ENGINE_PAYOFF_EXAMPLES_DIR
#error "ENGINE_PAYOFF_EXAMPLES_DIR no definido (ver cpp/engine/tests/CMakeLists.txt)"
#endif

// Mismo helper que test_payoff_fixtures_cross_layer.cpp (no exportado desde alli, se repite
// aqui): lee un fixture de docs/schema/engine.payoff/examples/ para el test cruzado de Fase 9.
std::string read_fixture_file(const std::string& name) {
    std::string path = std::string(ENGINE_PAYOFF_EXAMPLES_DIR) + "/" + name;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("no se pudo abrir " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST(GreeksFase1Test, RegisterBuiltinsRegistersTheGreekMeasureName) {
    Registries registries;
    register_builtins(registries);

    EXPECT_TRUE(registries.measures.contains("Greek"));
}

TEST(GreeksFase1Test, DeltaOfACallMatchesPayoffSensitivityQAndBlackScholes) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, payoff_price_q_request("spot"), model, product, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );

    auto payoff_sensitivity = registries.measures.create("PayoffSensitivityQ", Params{{"greek", std::string("spot")}});
    engine::MeasureResult via_registry =
        payoff_sensitivity->evaluate(model, product, flat_market(), pricing_context(200'000, 7), cpu_execution());

    const double bump = 1.0;
    double analytic_delta =
        (black_scholes_call(s0 + bump, strike, r, q, sigma, maturity) - black_scholes_call(s0 - bump, strike, r, q, sigma, maturity)) /
        (2.0 * bump);

    EXPECT_NEAR(via_greek.value, via_registry.scalar, 0.02)
        << "Greek=" << via_greek.value << " PayoffSensitivityQ=" << via_registry.scalar;
    EXPECT_NEAR(via_greek.value, analytic_delta, 0.02) << "Greek=" << via_greek.value << " analytic=" << analytic_delta;
}

TEST(GreeksFase1Test, VegaOfACallMatchesPayoffSensitivityQ) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, payoff_price_q_request("volatility"), model, product, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );
    auto payoff_sensitivity = registries.measures.create("PayoffSensitivityQ", Params{{"greek", std::string("volatility")}});
    engine::MeasureResult via_registry =
        payoff_sensitivity->evaluate(model, product, flat_market(), pricing_context(200'000, 7), cpu_execution());

    EXPECT_NEAR(via_greek.value, via_registry.scalar, 0.5)
        << "Greek=" << via_greek.value << " PayoffSensitivityQ=" << via_registry.scalar;
}

TEST(GreeksFase1Test, RhoOfACallMatchesPayoffSensitivityQ) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, payoff_price_q_request("rate"), model, product, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );
    auto payoff_sensitivity = registries.measures.create("PayoffSensitivityQ", Params{{"greek", std::string("rate")}});
    engine::MeasureResult via_registry =
        payoff_sensitivity->evaluate(model, product, flat_market(), pricing_context(200'000, 7), cpu_execution());

    EXPECT_NEAR(via_greek.value, via_registry.scalar, 1.0)
        << "Greek=" << via_greek.value << " PayoffSensitivityQ=" << via_registry.scalar;
}

TEST(GreeksFase1Test, DividendYieldSensitivityMatchesPayoffSensitivityQ) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, payoff_price_q_request("dividend_yield"), model, product, flat_market(), pricing_context(200'000, 7),
        cpu_execution()
    );
    auto payoff_sensitivity = registries.measures.create("PayoffSensitivityQ", Params{{"greek", std::string("dividend_yield")}});
    engine::MeasureResult via_registry =
        payoff_sensitivity->evaluate(model, product, flat_market(), pricing_context(200'000, 7), cpu_execution());

    EXPECT_NEAR(via_greek.value, via_registry.scalar, 1.0)
        << "Greek=" << via_greek.value << " PayoffSensitivityQ=" << via_registry.scalar;
}

TEST(GreeksFase1Test, GreekResultReportsBumpUsedMethodUsedAndInferredMeasure) {
    // Actualizado en Fase 7: (GBM, "PayoffPriceQ") ya esta en la tabla de capacidades pathwise
    // (greeks.cpp), asi que `method=auto` sobre "spot" ya NO resuelve por BumpAndReval (ver
    // GreeksFase7Test.AutoSelectsPathwiseForPayoffPriceQOnGbmWhenTheContractHasNoExercise, mas
    // abajo, para ese caso). Este test sigue verificando el reporte de `bump_used`/`method_used`
    // para BumpAndReval, ahora pidiendolo EXPLICITO en vez de depender de que Auto no encuentre
    // una especializacion.
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.method = GreekMethod::BumpAndReval;
    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    EXPECT_EQ(result.method_used, GreekMethod::BumpAndReval);
    ASSERT_TRUE(result.bump_used.has_value());
    EXPECT_DOUBLE_EQ(*result.bump_used, 1.0); // max(1e-2*|100|, 1e-4) = 1.0, PLAN_GREEKS.md §4.3
    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::RiskNeutralQ); // "PayoffPriceQ" -> sufijo Q
    EXPECT_EQ(result.risk_factor.name, "spot");
    EXPECT_EQ(result.order.order, 1);
}

// Actualizado en Fase 5 (PLAN_GREEKS.md §8.5 puntos 2-4): "curve.parallel"/"credit.hazard_rate"/
// "credit.recovery_rate"/"time.theta" se enumeran SIEMPRE, además de los 4 parámetros de modelo
// -- 8 candidatos en vez de 4, ninguno en skipped ("PayoffPriceQ" SÍ honra
// `PricingContext::pricing_date()`, a diferencia de curve/credit que `PayoffPriceQMeasure`
// ignora por completo).
TEST(GreeksFase1Test, ComputeAllGreeksOnGbmReturnsTheFourModelParametersPlusCurveCreditAndTheta) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreeksReport report = engine::greeks::compute_all_greeks(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(100'000, 7), cpu_execution()
    );

    EXPECT_TRUE(report.skipped.empty());
    ASSERT_EQ(report.greeks.size(), 8u);
    std::vector<std::string> factors;
    for (const auto& greek : report.greeks) factors.push_back(engine::greeks::to_string(greek.risk_factor));
    std::sort(factors.begin(), factors.end());
    EXPECT_EQ(
        factors,
        (std::vector<std::string>{
            "credit.hazard_rate", "credit.recovery_rate", "curve.parallel", "model.dividend_yield", "model.rate",
            "model.spot", "model.volatility", "time.theta"
        })
    );

    // PayoffPriceQMeasure ignora `market` (measure.cpp) -- ambas evaluaciones +-h usan
    // exactamente el mismo modelo/paths, así que la derivada respecto de la curva/crédito es 0
    // exacto, no una aproximación de Monte Carlo. Theta SÍ depende de `pricing_date()`
    // (cableado en esta fase) y por tanto no es cero (una call pierde valor temporal).
    for (const auto& greek : report.greeks) {
        if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::CurveParallel ||
            greek.risk_factor.kind == engine::greeks::RiskFactorKind::CreditParameter) {
            EXPECT_DOUBLE_EQ(greek.value, 0.0);
        }
        if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::TimeShift) {
            EXPECT_LT(greek.value, 0.0);
        }
    }
}

// Actualizado en Fase 4/5: además de los 4 parámetros de Hull-White (derivada nula, sin cambios --
// PresentValueMeasure::evaluate para IrSwapProduct no usa el modelo en absoluto), "curve.parallel"
// se enumera SIEMPRE y su valor SÍ depende de la curva (PLAN_GREEKS.md §8.5: "una derivada nula es
// una respuesta valida, distinta de 'no aplica'" -- aquí, al revés, una derivada no nula es la
// respuesta correcta porque PV de un IRS es, por construcción, función de la curva de descuento).
// "credit.hazard_rate"/"credit.recovery_rate" también se enumeran SIEMPRE, con derivada nula (PV
// de un IRS no consume datos de crédito). "time.theta" (Fase 5) también se intenta SIEMPRE, pero
// este swap tiene `start()==0.0` (default) y el bump por defecto de Theta es `1/365 > 0`: cruza
// la guardia de `npv_from_market` (§7.4, "Theta no puede cruzar el primer reset sin fixing
// historico de la pata flotante") y cae en `skipped`, no en `greeks` -- ver
// `GreeksFase5Test.HullWhiteIrsPvThetaMatchesManualBumpAndRevalWhenStartIsInTheFuture` para el
// caso donde SÍ se computa.
TEST(GreeksFase1Test, ComputeAllGreeksOnIrsPvIsZeroForHullWhiteAndCreditButNonZeroForCurveParallel) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap(Params{
        {"notional", 1'000'000.0},
        {"fixed_rate", 0.02},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0}},
    });
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = flat_market();

    engine::greeks::GreeksReport report = engine::greeks::compute_all_greeks(
        registries, "PV", Params{}, model, swap, market, pricing_context(1'000, 7), cpu_execution()
    );

    ASSERT_EQ(report.skipped.size(), 1u);
    EXPECT_NE(report.skipped[0].find("time.theta"), std::string::npos) << report.skipped[0];
    ASSERT_EQ(report.greeks.size(), 7u);

    auto pv = registries.measures.create("PV", Params{});
    const double h = 0.0001; // default de curva (PLAN_GREEKS.md §4.3)
    MarketSnapshot market_up = engine::bump_market_parallel(market, h);
    MarketSnapshot market_down = engine::bump_market_parallel(market, -h);
    double manual_curve_parallel = (pv->evaluate(model, swap, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                                     pv->evaluate(model, swap, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                                    (2.0 * h);

    std::vector<std::string> model_param_names;
    bool saw_curve_parallel = false;
    int credit_factors_seen = 0;
    for (const auto& greek : report.greeks) {
        if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::ModelParameter) {
            model_param_names.push_back(greek.risk_factor.name);
            EXPECT_DOUBLE_EQ(greek.value, 0.0);
        } else if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::CreditParameter) {
            ++credit_factors_seen;
            EXPECT_DOUBLE_EQ(greek.value, 0.0);
        } else {
            ASSERT_EQ(greek.risk_factor.kind, engine::greeks::RiskFactorKind::CurveParallel);
            saw_curve_parallel = true;
            EXPECT_NEAR(greek.value, manual_curve_parallel, 1e-6);
        }
    }
    EXPECT_TRUE(saw_curve_parallel);
    EXPECT_EQ(credit_factors_seen, 2);
    std::sort(model_param_names.begin(), model_param_names.end());
    EXPECT_EQ(model_param_names, (std::vector<std::string>{"a", "b", "r0", "sigma"}));
}

TEST(GreeksFase1Test, ForecastPVegaOfDriftMatchesAnalyticDerivative) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, mu = 0.10, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product(
        "TERMINAL_SPOT", pf::when(tp(maturity), pf::cashflow(pf::Currency{"USD"}, pf::fixing(spot, tp(maturity))))
    );
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffForecastP";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "mu", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(300'000, 11), cpu_execution()
    );

    double analytic = s0 * maturity * std::exp(mu * maturity); // d/dmu [s0 * exp(mu*T)]
    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::PhysicalP); // "PayoffForecastP" -> sufijo P
    EXPECT_NEAR(result.value, analytic, 0.05 * analytic) << "Greek=" << result.value << " analytic=" << analytic;
}

TEST(GreeksFase1Test, GreekIsReachableThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::PriceResult result = engine::price(
        registries, product,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{{"metric", std::string("PayoffPriceQ")}, {"risk_factor", std::string("model.spot")}}}
        },
        model, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].measure_name, "Greek");
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
    EXPECT_LT(result[0].result.scalar, 1.0);
}

TEST(GreeksFase1Test, GreekMeasureRejectsAnUnknownRiskFactorString) {
    Registries registries;
    register_builtins(registries);

    EXPECT_THROW(
        registries.measures.create(
            "Greek", Params{{"metric", std::string("PayoffPriceQ")}, {"risk_factor", std::string("not_a_valid_factor")}}
        ),
        std::invalid_argument
    );
}

// Actualizado en Fase 6: order=2 sin cross_factor (Gamma) ya es soportado para
// RiskFactorKind::ModelParameter -- ver GreeksFase6Test.GammaOfACallMatchesClosedFormSecondDerivative
// más abajo. Lo que este test verificaba (order=2 rechazado sin excepción) ya no existe; el único
// rechazo que sigue vigente es order fuera de {1,2} y order=2 CON cross_factor (tercera derivada).
TEST(GreeksFase1Test, ComputeGreekRejectsAnOrderOutsideOneOrTwo) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{3, std::nullopt};

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

// Renombrado/actualizado en Fase 7 (PLAN_GREEKS.md §11): (GBM, "PayoffPriceQ") ya es una
// combinacion pathwise VERIFICADA (ver pathwise_capabilities() en greeks.cpp) -- pedir
// method=pathwise sobre ella ya NO lanza, la tabla de capacidades ahora la sirve. La forma "pedir
// method=pathwise sobre una combinacion no soportada lanza explicito" sigue vigente, solo que
// ahora se prueba sobre una combinacion que de verdad no esta en la tabla (ver
// GreeksFase7Test.ExplicitPathwiseRejectsAnUnverifiedCombination, mas abajo).
TEST(GreeksFase1Test, ComputeGreekAcceptsPathwiseMethodExplicitlyForAVerifiedCombination) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.method = GreekMethod::Pathwise;

    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );
    EXPECT_EQ(result.method_used, GreekMethod::Pathwise);
    EXPECT_FALSE(result.bump_used.has_value());
}

// Actualizado en Fase 5: los cinco RiskFactorKind del catálogo (§3.1) ya están soportados --
// "time.theta" (el último) queda cableado, pero solo para las métricas de la lista cerrada de
// `metric_supports_time_shift` ("PV"/"PayoffPriceQ"). Pedirlo sobre cualquier otra métrica sigue
// rechazándose explícito (ver GreeksFase5Test.ComputeGreekRejectsTimeShiftForAMetricNotYetWired
// más abajo) -- lo que este test verificaba (un RiskFactorKind entero pendiente) ya no existe.

TEST(GreeksFase1Test, ComputeGreekRejectsAnUnknownModelParameterName) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("not_a_real_parameter");

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(GreeksFase1Test, ComputeGreekRejectsAProductThatIsNotSupportedByTheInnerMetric) {
    Registries registries;
    register_builtins(registries);
    FakeProduct fake;
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.XYZ");

    EXPECT_THROW(
        engine::greeks::compute_greek(
            registries, payoff_price_q_request("spot"), model, fake, flat_market(), pricing_context(1'000, 7), cpu_execution()
        ),
        std::invalid_argument
    );
}

TEST(GreeksFase1Test, ParseRiskFactorRoundTripsThroughToString) {
    EXPECT_EQ(engine::greeks::to_string(engine::greeks::parse_risk_factor("model.spot")), "model.spot");
    EXPECT_EQ(engine::greeks::to_string(engine::greeks::parse_risk_factor("curve.parallel")), "curve.parallel");
    EXPECT_EQ(engine::greeks::to_string(engine::greeks::parse_risk_factor("curve.pillar:3")), "curve.pillar:3");
    EXPECT_EQ(engine::greeks::to_string(engine::greeks::parse_risk_factor("credit.hazard_rate")), "credit.hazard_rate");
    EXPECT_EQ(engine::greeks::to_string(engine::greeks::parse_risk_factor("time.theta")), "time.theta");
    EXPECT_THROW(engine::greeks::parse_risk_factor("bogus"), std::invalid_argument);
    EXPECT_THROW(engine::greeks::parse_risk_factor("model."), std::invalid_argument);
    EXPECT_THROW(engine::greeks::parse_risk_factor("curve.unknown"), std::invalid_argument);
}

// --- Fase 2: convención de prefijo `metric.*` para métricas con parámetros propios ------------

TEST(GreeksFase2Test, HitProbabilityQVegaMatchesManualBumpAndReval) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, barrier = 120.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const std::vector<pf::TimePoint> monitoring_times{tp(0.25), tp(0.5), tp(0.75), tp(maturity)};
    const double h = 0.02;

    pf::PayoffProduct product("UI", up_and_in_call(spot, barrier, strike, maturity, monitoring_times));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffHitProbabilityQ";
    request.metric_params = Params{{"event", std::string("UI")}};
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(50'000, 13), cpu_execution());

    auto hit_probability = registries.measures.create("PayoffHitProbabilityQ", Params{{"event", std::string("UI")}});
    engine::GbmModel model_up = make_gbm_q(s0, r, q, sigma + h, spot.value);
    engine::GbmModel model_down = make_gbm_q(s0, r, q, sigma - h, spot.value);
    engine::MeasureResult up = hit_probability->evaluate(model_up, product, flat_market(), pricing_context(50'000, 13), cpu_execution());
    engine::MeasureResult down =
        hit_probability->evaluate(model_down, product, flat_market(), pricing_context(50'000, 13), cpu_execution());
    double manual = (up.scalar - down.scalar) / (2.0 * h);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_TRUE(via_greek.times.empty());
    EXPECT_TRUE(via_greek.primary.empty());
    EXPECT_TRUE(via_greek.secondary.empty());
    EXPECT_NEAR(via_greek.value, manual, 1e-9) << "Greek=" << via_greek.value << " manual=" << manual;
    EXPECT_GT(via_greek.value, 0.0); // mas volatilidad -> mas probabilidad de tocar una barrera al alza
}

TEST(GreeksFase2Test, PnlDistributionPGreeksOfMeanVarEsToMuMatchManualBumpAndReval) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, mu = 0.05, sigma = 0.2, maturity = 1.0;
    const double h = 0.01;

    pf::PayoffProduct product(
        "LONG_SPOT", pf::when(tp(maturity), pf::cashflow(pf::Currency{"USD"}, pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(s0))))
    );
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffPnlDistributionP";
    request.metric_params = Params{{"confidence", 0.95}};
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "mu", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(50'000, 11), cpu_execution());

    auto pnl = registries.measures.create("PayoffPnlDistributionP", Params{{"confidence", 0.95}});
    engine::GbmPModel model_up = make_gbm_p(s0, mu + h, sigma, spot.value);
    engine::GbmPModel model_down = make_gbm_p(s0, mu - h, sigma, spot.value);
    engine::MeasureResult up = pnl->evaluate(model_up, product, flat_market(), pricing_context(50'000, 11), cpu_execution());
    engine::MeasureResult down = pnl->evaluate(model_down, product, flat_market(), pricing_context(50'000, 11), cpu_execution());

    ASSERT_TRUE(via_greek.has_scalar);
    ASSERT_EQ(via_greek.primary.size(), 1u);
    ASSERT_EQ(via_greek.secondary.size(), 1u);
    double manual_mean = (up.scalar - down.scalar) / (2.0 * h);
    double manual_var = (up.primary[0] - down.primary[0]) / (2.0 * h);
    double manual_es = (up.secondary[0] - down.secondary[0]) / (2.0 * h);
    EXPECT_NEAR(via_greek.value, manual_mean, 1e-9) << "mean: Greek=" << via_greek.value << " manual=" << manual_mean;
    EXPECT_NEAR(via_greek.primary[0], manual_var, 1e-9) << "var: Greek=" << via_greek.primary[0] << " manual=" << manual_var;
    EXPECT_NEAR(via_greek.secondary[0], manual_es, 1e-9) << "es: Greek=" << via_greek.secondary[0] << " manual=" << manual_es;
}

TEST(GreeksFase2Test, ExposureProfileQGreekIsAPerPointDeltaWithNoScalar) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const std::vector<double> exposure_times{0.5, maturity};
    const double h = 0.02;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffExposureProfileQ";
    request.metric_params = Params{{"exposure_times", exposure_times}};
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(50'000, 5), cpu_execution());

    auto exposure = registries.measures.create("PayoffExposureProfileQ", Params{{"exposure_times", exposure_times}});
    engine::GbmModel model_up = make_gbm_q(s0, r, q, sigma + h, spot.value);
    engine::GbmModel model_down = make_gbm_q(s0, r, q, sigma - h, spot.value);
    engine::MeasureResult up = exposure->evaluate(model_up, product, flat_market(), pricing_context(50'000, 5), cpu_execution());
    engine::MeasureResult down = exposure->evaluate(model_down, product, flat_market(), pricing_context(50'000, 5), cpu_execution());

    EXPECT_FALSE(via_greek.has_scalar);
    ASSERT_EQ(via_greek.times, exposure_times);
    ASSERT_EQ(via_greek.primary.size(), exposure_times.size());
    ASSERT_EQ(via_greek.secondary.size(), exposure_times.size());
    for (std::size_t i = 0; i < exposure_times.size(); ++i) {
        double manual_ee = (up.primary[i] - down.primary[i]) / (2.0 * h);
        double manual_pfe = (up.secondary[i] - down.secondary[i]) / (2.0 * h);
        EXPECT_NEAR(via_greek.primary[i], manual_ee, 1e-9) << "EE[" << i << "]";
        EXPECT_NEAR(via_greek.secondary[i], manual_pfe, 1e-9) << "PFE95[" << i << "]";
    }
}

TEST(GreeksFase2Test, GreekMeasureForwardsMetricEventThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, barrier = 120.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const std::vector<pf::TimePoint> monitoring_times{tp(0.25), tp(0.5), tp(0.75), tp(maturity)};

    pf::PayoffProduct product("UI", up_and_in_call(spot, barrier, strike, maturity, monitoring_times));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::PriceResult result = engine::price(
        registries, product,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{
                          {"metric", std::string("PayoffHitProbabilityQ")}, {"metric.event", std::string("UI")},
                          {"risk_factor", std::string("model.volatility")}
                      }}
        },
        model, flat_market(), pricing_context(30'000, 13), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_TRUE(std::isfinite(result[0].result.scalar));
}

TEST(GreeksFase2Test, GreekMeasureForwardsMetricExposureTimesThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::PriceResult result = engine::price(
        registries, product,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{
                          {"metric", std::string("PayoffExposureProfileQ")},
                          {"metric.exposure_times", std::vector<double>{0.5, maturity}},
                          {"risk_factor", std::string("model.spot")}
                      }}
        },
        model, flat_market(), pricing_context(30'000, 5), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    EXPECT_FALSE(result[0].result.has_scalar);
    ASSERT_EQ(result[0].result.times.size(), 2u);
    ASSERT_EQ(result[0].result.primary.size(), 2u);
    ASSERT_EQ(result[0].result.secondary.size(), 2u);
}

TEST(GreeksFase2Test, GreekMeasureRejectsWhenTheInnerMetricIsMissingAMandatoryParam) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, barrier = 120.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const std::vector<pf::TimePoint> monitoring_times{tp(0.25), tp(0.5), tp(0.75), tp(maturity)};

    pf::PayoffProduct product("UI", up_and_in_call(spot, barrier, strike, maturity, monitoring_times));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    // "event" no se reenvia (falta "metric.event") -- PayoffHitProbabilityQMeasure lo exige
    // (`get_string` sin default) y lanza `std::out_of_range` al construirse dentro de
    // compute_greek; nunca degrada a un resultado silencioso.
    auto measure = registries.measures.create(
        "Greek", Params{{"metric", std::string("PayoffHitProbabilityQ")}, {"risk_factor", std::string("model.volatility")}}
    );

    EXPECT_THROW(
        measure->evaluate(model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()), std::out_of_range
    );
}

// --- Fase 3: RiskFactorKind::CurveParallel/CurvePillar genéricos -------------------------------
//
// A diferencia de Fase 1-2 (RiskFactor de MODELO: bumpea `model`, `market` fijo), aquí se bumpea
// `market` y el modelo queda fijo -- aplicable a CUALQUIER producto que descuenta con
// `MarketSnapshot` (IrSwapProduct directamente, PayoffProduct vía `market_snapshot_bridge`),
// quitando la limitación previa de `Dv01Measure::evaluate` ("bucketed=true no soportado para
// PayoffProduct"). Los oráculos de estos tests son bump-and-reval MANUAL con
// `engine::bump_market_parallel`/`bump_market_pillar` -- las mismas dos funciones que ahora usa
// `compute_greek` internamente (engine/market.hpp).

MarketSnapshot upward_sloping_market() { return MarketSnapshot({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022}); }

engine::IrSwapProduct make_irs(double notional, double fixed_rate) {
    return engine::IrSwapProduct(Params{
        {"notional", notional},
        {"fixed_rate", fixed_rate},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0}},
    });
}

TEST(GreeksFase3Test, CurveParallelGreekOfIrsPvMatchesManualBumpAndReval) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market();
    const double h = 0.0002;

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::CurveParallel, "curve", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution());

    auto pv = registries.measures.create("PV", Params{});
    MarketSnapshot market_up = engine::bump_market_parallel(market, h);
    MarketSnapshot market_down = engine::bump_market_parallel(market, -h);
    double manual = (pv->evaluate(model, swap, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                      pv->evaluate(model, swap, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                     (2.0 * h);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_EQ(via_greek.bump_used, h);
    EXPECT_NEAR(via_greek.value, manual, 1e-9) << "Greek=" << via_greek.value << " manual=" << manual;
}

TEST(GreeksFase3Test, CurvePillarGreekOfIrsPvMatchesManualBumpAndReval) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market();
    const double h = 0.0002;

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::CurvePillar, "curve", "", std::size_t{1}};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution());

    auto pv = registries.measures.create("PV", Params{});
    MarketSnapshot market_up = engine::bump_market_pillar(market, 1, h);
    MarketSnapshot market_down = engine::bump_market_pillar(market, 1, -h);
    double manual = (pv->evaluate(model, swap, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                      pv->evaluate(model, swap, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                     (2.0 * h);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_EQ(via_greek.risk_factor.pillar_index, 1u);
    EXPECT_NEAR(via_greek.value, manual, 1e-9) << "Greek=" << via_greek.value << " manual=" << manual;
}

TEST(GreeksFase3Test, CurveParallelGreekOfPayoffProductPvMatchesManualBumpAndReval) {
    // "aplicable a CUALQUIER producto que descuenta con MarketSnapshot" (PLAN_GREEKS.md §11 Fase
    // 3): antes de esta fase, `Dv01Measure` era la ÚNICA forma de obtener una sensibilidad de
    // curva para un PayoffProduct, y solo en su variante paralela. Aquí se pide vía "Greek"
    // genérico, sin ningún código nuevo por producto.
    Registries registries;
    register_builtins(registries);
    pf::PayoffProduct bond("ZERO_COUPON_BOND", pf::when(tp(3.0), pf::cashflow(pf::Currency{"USD"}, pf::constant(500'000.0))));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.XYZ"); // ignorado por "PV" para PayoffProduct
    MarketSnapshot market = upward_sloping_market();
    const double h = 0.0002;

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::CurveParallel, "curve", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, bond, market, pricing_context(1'000, 7), cpu_execution());

    auto pv = registries.measures.create("PV", Params{});
    MarketSnapshot market_up = engine::bump_market_parallel(market, h);
    MarketSnapshot market_down = engine::bump_market_parallel(market, -h);
    double manual = (pv->evaluate(model, bond, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                      pv->evaluate(model, bond, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                     (2.0 * h);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_LT(via_greek.value, 0.0); // subir la curva de descuento baja el PV de un cashflow futuro
    EXPECT_NEAR(via_greek.value, manual, 1e-6) << "Greek=" << via_greek.value << " manual=" << manual;
}

TEST(GreeksFase3Test, ComputeGreekRejectsAnOutOfRangePillarIndex) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market(); // 3 pillars: indices 0..2

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::CurvePillar, "curve", "", std::size_t{5}};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(GreeksFase3Test, ComputeAllGreeksWithIncludeCurveBucketsAddsOnePillarCandidatePerPillar) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market(); // 3 pillars

    engine::greeks::GreeksReport without_buckets = engine::greeks::compute_all_greeks(
        registries, "PV", Params{}, model, swap, market, pricing_context(1'000, 7), cpu_execution(),
        /*include_curve_buckets=*/false
    );
    // "time.theta" cae en skipped: make_irs() usa start()==0.0 (default), y el bump por defecto
    // de Theta (1/365 > 0) cruza la guardia de npv_from_market (§7.4) -- ver GreeksFase1Test.
    // ComputeAllGreeksOnIrsPvIsZeroForHullWhiteAndCreditButNonZeroForCurveParallel.
    ASSERT_EQ(without_buckets.skipped.size(), 1u);
    ASSERT_EQ(without_buckets.greeks.size(), 7u); // 4 params HW1F + curve.parallel + 2 credit

    engine::greeks::GreeksReport with_buckets = engine::greeks::compute_all_greeks(
        registries, "PV", Params{}, model, swap, market, pricing_context(1'000, 7), cpu_execution(),
        /*include_curve_buckets=*/true
    );
    ASSERT_EQ(with_buckets.skipped.size(), 1u);
    ASSERT_EQ(with_buckets.greeks.size(), 10u); // + curve.pillar:0/1/2

    std::vector<std::string> pillar_factors;
    for (const auto& greek : with_buckets.greeks) {
        if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::CurvePillar) {
            pillar_factors.push_back(engine::greeks::to_string(greek.risk_factor));
        }
    }
    std::sort(pillar_factors.begin(), pillar_factors.end());
    EXPECT_EQ(pillar_factors, (std::vector<std::string>{"curve.pillar:0", "curve.pillar:1", "curve.pillar:2"}));
}

TEST(GreeksFase3Test, GreekMeasureReachesCurveParallelThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market();

    engine::PriceResult result = engine::price(
        registries, swap,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{{"metric", std::string("PV")}, {"risk_factor", std::string("curve.parallel")}}}
        },
        model, market, pricing_context(1'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_TRUE(std::isfinite(result[0].result.scalar));
}

// --- Fase 4: RiskFactorKind::CreditParameter (hazard_rate/recovery_rate de CVA) -----------------
//
// A diferencia de curve.*/model.* (bumpea market/model, respectivamente, pero afecta la curva de
// descuento o el modelo de tipos), aquí se bumpea `market.hazard_rate()`/`market.recovery_rate()`
// -- datos de crédito puros, consumidos hoy únicamente por `UnilateralCvaMeasure`
// (`ExposureProfileMeasure::evaluate` ignora `market` por completo, measure.cpp). El oráculo es
// bump-and-reval MANUAL con `engine::bump_market_credit`, la misma función que ahora usa
// `compute_greek` internamente. Como el perfil de exposición no depende de `market`, ambas
// evaluaciones +-h comparten exactamente los mismos paths Monte Carlo -- la derivada resultante es
// numéricamente exacta (sin ruido de Monte Carlo), igual que curve.parallel sobre PV de un IRS.

MarketSnapshot credit_market() {
    return MarketSnapshot({1.0, 2.0, 3.0}, {0.02, 0.021, 0.022}, /*hazard_rate=*/0.02, /*recovery_rate=*/0.4);
}

TEST(GreeksFase4Test, HazardRateGreekOfCvaMatchesManualBumpAndRevalAndHasExpectedSign) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = credit_market();
    const double h = 0.0001;

    GreekRequest request;
    request.metric_name = "UnilateralCVA";
    request.risk_factor = RiskFactor{RiskFactorKind::CreditParameter, "credit", "hazard_rate", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution());

    auto cva = registries.measures.create("UnilateralCVA", Params{});
    MarketSnapshot market_up = engine::bump_market_credit(market, "hazard_rate", h);
    MarketSnapshot market_down = engine::bump_market_credit(market, "hazard_rate", -h);
    double manual = (cva->evaluate(model, swap, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                      cva->evaluate(model, swap, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                     (2.0 * h);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_EQ(via_greek.bump_used, h);
    EXPECT_GT(via_greek.value, 0.0); // sube el hazard rate -> sube CVA (PLAN_GREEKS.md §11 Fase 4)
    EXPECT_NEAR(via_greek.value, manual, 1e-9) << "Greek=" << via_greek.value << " manual=" << manual;
}

TEST(GreeksFase4Test, RecoveryRateGreekOfCvaMatchesManualBumpAndRevalAndIsNegative) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = credit_market();
    const double h = 0.0001;

    GreekRequest request;
    request.metric_name = "UnilateralCVA";
    request.risk_factor = RiskFactor{RiskFactorKind::CreditParameter, "credit", "recovery_rate", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = h;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution());

    auto cva = registries.measures.create("UnilateralCVA", Params{});
    MarketSnapshot market_up = engine::bump_market_credit(market, "recovery_rate", h);
    MarketSnapshot market_down = engine::bump_market_credit(market, "recovery_rate", -h);
    double manual = (cva->evaluate(model, swap, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                      cva->evaluate(model, swap, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                     (2.0 * h);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_LT(via_greek.value, 0.0); // mas recovery -> menos perdida dado default -> menos CVA
    EXPECT_NEAR(via_greek.value, manual, 1e-9) << "Greek=" << via_greek.value << " manual=" << manual;
}

TEST(GreeksFase4Test, ComputeGreekRejectsAnUnknownCreditParameterNameAtParseTime) {
    // `parse_risk_factor` (Fase 0) ya rechaza cualquier nombre de credito que no sea
    // "hazard_rate"/"recovery_rate" antes de que compute_greek llegue a bumpear nada.
    EXPECT_THROW(engine::greeks::parse_risk_factor("credit.default_correlation"), std::invalid_argument);
}

TEST(GreeksFase4Test, GreekMeasureReachesCreditHazardRateThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = credit_market();

    engine::PriceResult result = engine::price(
        registries, swap,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{{"metric", std::string("UnilateralCVA")}, {"risk_factor", std::string("credit.hazard_rate")}}}
        },
        model, market, pricing_context(1'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
    EXPECT_TRUE(std::isfinite(result[0].result.scalar));
}

// --- Fase 5: RiskFactorKind::TimeShift (Theta) --------------------------------------------------
//
// A diferencia de model.*/curve.*/credit.* (bumpea `model`/`market`, diferencia CENTRAL), aquí se
// desplaza `PricingContext::pricing_date()` y se usa una diferencia UNIDIRECCIONAL
// `Metric(t+dt) - Metric(t)` (ADR §7.1, "theta puro": todo lo demás constante, solo avanza el
// reloj) -- mismo patrón no-derivado que `Dv01Measure`. Solo está cableado para `metric_name` en
// {"PV", "PayoffPriceQ"} (`metric_supports_time_shift`, greeks.cpp); cualquier otra métrica se
// rechaza explícito para no devolver un Theta silenciosamente nulo.

TEST(GreeksFase5Test, ThetaOfAEuropeanCallIsNegativeAndMatchesClosedFormFiniteDifference) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const double dt = 1.0 / 365.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffPriceQ";
    request.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(300'000, 7), cpu_execution()
    );

    double analytic_theta =
        black_scholes_call(s0, strike, r, q, sigma, maturity - dt) - black_scholes_call(s0, strike, r, q, sigma, maturity);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_EQ(via_greek.bump_used, dt);
    EXPECT_LT(via_greek.value, 0.0) << "una call sin dividendos pierde valor temporal";
    EXPECT_NEAR(via_greek.value, analytic_theta, 0.02)
        << "Greek=" << via_greek.value << " analytic=" << analytic_theta;
}

// Swap forward-starting (start > dt) valorado exactamente a la par en t=0: NPV(0) = 0 por
// construccion, y NPV(dt) = NPV(0) / discount_factor(dt) (reescalado lineal, ver el doc-comment
// de npv_from_market) -- Theta = NPV(dt) - NPV(0) = 0 EXACTO, no solo "aproximadamente cero".
TEST(GreeksFase5Test, HullWhiteIrsPvThetaIsExactlyZeroForAParSwapWhenStartIsInTheFuture) {
    Registries registries;
    register_builtins(registries);
    MarketSnapshot market = upward_sloping_market(); // pillars {1,2,3}, zero_rates {.02,.021,.022}
    const double start = 0.5;
    const std::vector<double> payment_times{1.5, 2.5, 3.5};
    const std::vector<double> accruals{1.0, 1.0, 1.0};

    double par_numerator = market.discount_factor(start) - market.discount_factor(payment_times.back());
    double par_denominator = 0.0;
    for (std::size_t i = 0; i < payment_times.size(); ++i) par_denominator += accruals[i] * market.discount_factor(payment_times[i]);
    double par_rate = par_numerator / par_denominator;

    engine::IrSwapProduct swap(Params{
        {"notional", 1'000'000.0},
        {"fixed_rate", par_rate},
        {"start", start},
        {"payment_times", payment_times},
        {"accruals", accruals},
    });
    engine::HullWhite1FModel model = make_hull_white();

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    engine::greeks::GreekResult via_greek =
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution());

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_NEAR(via_greek.value, 0.0, 1e-6) << "swap a la par, curva sin cambios -> Theta ~ 0";
}

TEST(GreeksFase5Test, ComputeGreekRejectsTimeShiftForAMetricNotYetWired) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market();

    GreekRequest request;
    request.metric_name = "DV01"; // no esta en metric_supports_time_shift (solo "PV"/"PayoffPriceQ")
    request.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(GreeksFase5Test, ComputeGreekRejectsTimeShiftThatCrossesARequiredTimeOfThePayoffContract) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0)); // maturity = 1.0
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffPriceQ";
    request.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;
    request.bump_override = 2.0; // > maturity: cruza el unico instante requerido por el contrato

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::exception
    );
}

TEST(GreeksFase5Test, ComputeGreekRejectsTimeShiftThatCrossesTheFirstResetOfAnIrs) {
    // make_irs() usa start()==0.0 (default) -- cualquier dt>0 ya cruza el primer reset de la pata
    // flotante (§7.4, ver el doc-comment de npv_from_market en measure.cpp).
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = upward_sloping_market();

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, swap, market, pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(GreeksFase5Test, GreekMeasureReachesTimeThetaThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::PriceResult result = engine::price(
        registries, product,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{{"metric", std::string("PayoffPriceQ")}, {"risk_factor", std::string("time.theta")}}}
        },
        model, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_LT(result[0].result.scalar, 0.0);
}

// --- Fase 6: segundo orden y derivadas cruzadas (Gamma, Vanna) ----------------------------------
//
// order=2 sin cross_factor reutiliza el estencil de 3 puntos `(up - 2*base + down)/h²`; order=1
// con cross_factor usa el estencil de 4 puntos `(up_up - up_down - down_up + down_down)/(4*h1*h2)`
// (PLAN_GREEKS.md §11 Fase 6). Ambos siguen soportados solo para
// ModelParameter/CurveParallel/CurvePillar/CreditParameter -- `TimeShift` queda excluido.

TEST(GreeksFase6Test, GammaOfACallMatchesClosedFormSecondDerivative) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{2, std::nullopt};
    // PLAN_HYPERDUAL.md §5: `method=Auto` ahora prefiere Pathwise (likelihood ratio) para esta
    // combinacion (ver GreeksHyperdualTest.AutoResolvesGammaOfACallToLikelihoodRatioPathwise) --
    // este test fija `BumpAndReval` explicito para seguir verificando el estencil GENERICO de 3
    // puntos independientemente de que prefiera Auto.
    request.method = GreekMethod::BumpAndReval;

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );

    double analytic_gamma = black_scholes_gamma(s0, strike, r, q, sigma, maturity);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_EQ(via_greek.order.order, 2);
    EXPECT_FALSE(via_greek.order.cross_factor.has_value());
    EXPECT_EQ(via_greek.method_used, GreekMethod::BumpAndReval);
    EXPECT_GT(via_greek.value, 0.0) << "la Gamma de una call vainilla es siempre positiva";
    EXPECT_NEAR(via_greek.value, analytic_gamma, 0.01)
        << "Greek=" << via_greek.value << " analytic=" << analytic_gamma;
}

TEST(GreeksFase6Test, VannaOfACallHasExpectedSignAndMatchesClosedFormMixedFiniteDifference) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const double h_spot = 1.0;     // default_model_parameter_bump(100.0) = max(1e-2*100, 1e-4)
    const double h_vol = 0.002;    // default_model_parameter_bump(0.2)   = max(1e-2*0.2, 1e-4)

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt}};
    // PLAN_HYPERDUAL.md §5: `method=Auto` ahora prefiere Pathwise (likelihood ratio) para esta
    // combinacion (ver GreeksHyperdualTest.AutoResolvesVannaOfACallToLikelihoodRatioPathwise) --
    // este test fija `BumpAndReval` explicito para seguir verificando el estencil GENERICO de 4
    // puntos (que ademas es el unico que reporta `bump_used`/`warnings` para el cross_factor).
    request.method = GreekMethod::BumpAndReval;

    engine::greeks::GreekResult via_greek = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );

    double manual =
        (black_scholes_call(s0 + h_spot, strike, r, q, sigma + h_vol, maturity) -
         black_scholes_call(s0 + h_spot, strike, r, q, sigma - h_vol, maturity) -
         black_scholes_call(s0 - h_spot, strike, r, q, sigma + h_vol, maturity) +
         black_scholes_call(s0 - h_spot, strike, r, q, sigma - h_vol, maturity)) /
        (4.0 * h_spot * h_vol);

    EXPECT_TRUE(via_greek.has_scalar);
    EXPECT_EQ(via_greek.order.order, 1);
    ASSERT_TRUE(via_greek.order.cross_factor.has_value());
    EXPECT_EQ(via_greek.order.cross_factor->name, "volatility");
    EXPECT_EQ(via_greek.method_used, GreekMethod::BumpAndReval);
    EXPECT_EQ(via_greek.bump_used, h_spot);
    EXPECT_FALSE(via_greek.warnings.empty()) << "el bump del cross_factor se documenta en warnings (§13)";
    EXPECT_NEAR(via_greek.value, manual, 0.02) << "Greek=" << via_greek.value << " manual=" << manual;
}

TEST(GreeksFase6Test, ComputeGreekRejectsOrderTwoWithCrossFactor) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{2, RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt}};

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(GreeksFase6Test, ComputeGreekRejectsSecondOrderOrCrossFactorForTimeShift) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest second_order_theta;
    second_order_theta.metric_name = "PayoffPriceQ";
    second_order_theta.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    second_order_theta.order = GreekOrder{2, std::nullopt};
    EXPECT_THROW(
        engine::greeks::compute_greek(
            registries, second_order_theta, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
        ),
        std::invalid_argument
    );

    GreekRequest cross_theta;
    cross_theta.metric_name = "PayoffPriceQ";
    cross_theta.risk_factor = RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
    cross_theta.order = GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt}};
    EXPECT_THROW(
        engine::greeks::compute_greek(
            registries, cross_theta, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
        ),
        std::invalid_argument
    );

    GreekRequest cross_with_theta = payoff_price_q_request("spot");
    cross_with_theta.order = GreekOrder{1, RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt}};
    EXPECT_THROW(
        engine::greeks::compute_greek(
            registries, cross_with_theta, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
        ),
        std::invalid_argument
    );
}

TEST(GreeksFase6Test, ComputeAllGreeksWithIncludeSecondOrderAddsGammaForEachModelParameterOnly) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreeksReport without_second_order = engine::greeks::compute_all_greeks(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(50'000, 7), cpu_execution(),
        /*include_curve_buckets=*/false, /*include_second_order=*/false
    );
    ASSERT_EQ(without_second_order.greeks.size(), 8u); // 4 params + curve.parallel + 2 credit + time.theta

    engine::greeks::GreeksReport with_second_order = engine::greeks::compute_all_greeks(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(50'000, 7), cpu_execution(),
        /*include_curve_buckets=*/false, /*include_second_order=*/true
    );
    EXPECT_TRUE(with_second_order.skipped.empty());
    ASSERT_EQ(with_second_order.greeks.size(), 12u); // + Gamma pura de los 4 parametros de modelo

    int order_two_count = 0;
    for (const auto& greek : with_second_order.greeks) {
        if (greek.order.order == 2) {
            ++order_two_count;
            EXPECT_EQ(greek.risk_factor.kind, engine::greeks::RiskFactorKind::ModelParameter)
                << "la Gamma automatica de compute_all_greeks es SOLO de parametros de modelo (PLAN_GREEKS.md §8.5 punto 5)";
        }
    }
    EXPECT_EQ(order_two_count, 4);
}

TEST(GreeksFase6Test, GreekMeasureReachesGammaThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::PriceResult result = engine::price(
        registries, product,
        std::vector<engine::MeasureSpec>{
            {"Greek", Params{{"metric", std::string("PayoffPriceQ")}, {"risk_factor", std::string("model.spot")}, {"order", 2.0}}}
        },
        model, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
}

// Fase 7 de PLAN_GREEKS.md (§11): rutas especializadas verificadas -- pathwise extendido
// (GbmGreek -> RiskFactor unificado, mas GbmP para "PayoffForecastP") y AAD reverse-mode
// (`irs_hull_white_npv_all_greeks`/`_2f_`, sobre la metrica nueva "HullWhiteModelNpv" -- "PV" NO
// depende del modelo desde PLAN_REAPI.md §6 Fase 4, ver el doc-comment de
// `HullWhiteModelNpvMeasure`). Criterio de aceptacion explicito: `method=auto` elige pathwise/AAD
// cuando existe y coincide con BumpAndReval dentro de tolerancia; pedir `method=pathwise` sobre
// `PayoffHitProbabilityQ` (u otra combinacion no verificada) falla explicito, nunca aproxima en
// silencio.

engine::HullWhite2FModel make_hull_white_2f() {
    return engine::HullWhite2FModel(
        Params{{"a", 0.1}, {"b", 0.2}, {"sigma", 0.01}, {"eta", 0.012}, {"rho", -0.7}, {"r0", 0.03}}
    );
}

pf::ContractPtr bermuda_put(
    const pf::ObservableId& spot, double strike, double maturity, const std::vector<pf::TimePoint>& dates
) {
    pf::ScalarExprPtr exercise_value = pf::maximum(pf::sub(pf::constant(strike), pf::current(spot)), pf::constant(0.0));
    return pf::exercise(pf::EventId{"EX"}, dates, exercise_value, european_call(spot, strike, maturity));
}

TEST(GreeksFase7Test, AutoSelectsPathwiseForPayoffPriceQOnGbmWhenTheContractHasNoExercise) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, payoff_price_q_request("spot"), model, product, flat_market(), pricing_context(50'000, 7),
        cpu_execution()
    );

    EXPECT_EQ(result.method_used, GreekMethod::Pathwise);
    EXPECT_FALSE(result.bump_used.has_value());
    EXPECT_TRUE(result.std_error.has_value()); // McEstimate::std_error real, a diferencia de BumpAndReval
    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::RiskNeutralQ);
}

TEST(GreeksFase7Test, PathwiseMatchesExplicitBumpAndRevalForVegaRhoDividendYield) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    for (const auto& [risk_factor_name, tolerance] :
         std::vector<std::pair<std::string, double>>{{"volatility", 0.5}, {"rate", 1.0}, {"dividend_yield", 1.0}}) {
        GreekRequest bump_request = payoff_price_q_request(risk_factor_name);
        bump_request.method = GreekMethod::BumpAndReval;
        engine::greeks::GreekResult bump_and_reval = engine::greeks::compute_greek(
            registries, bump_request, model, product, flat_market(), pricing_context(200'000, 7), cpu_execution()
        );
        engine::greeks::GreekResult pathwise = engine::greeks::compute_greek(
            registries, payoff_price_q_request(risk_factor_name), model, product, flat_market(),
            pricing_context(200'000, 7), cpu_execution()
        );

        EXPECT_EQ(pathwise.method_used, GreekMethod::Pathwise);
        EXPECT_EQ(bump_and_reval.method_used, GreekMethod::BumpAndReval);
        EXPECT_NEAR(pathwise.value, bump_and_reval.value, tolerance)
            << "risk_factor=" << risk_factor_name << " pathwise=" << pathwise.value
            << " bump_and_reval=" << bump_and_reval.value;
    }
}

TEST(GreeksFase7Test, AutoFallsBackToBumpAndRevalForAContractWithExercise) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("BERMUDA_PUT", bermuda_put(spot, 100.0, 1.0, {tp(0.25), tp(0.5), tp(0.75)}));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, payoff_price_q_request("spot"), model, product, flat_market(), pricing_context(20'000, 41),
        cpu_execution()
    );

    EXPECT_EQ(result.method_used, GreekMethod::BumpAndReval)
        << "un contrato con Exercise cae al fallback bump-and-reval, PLAN_GREEKS.md §5.1";
    ASSERT_TRUE(result.bump_used.has_value());
}

TEST(GreeksFase7Test, ExplicitPathwiseRejectsAContractWithExercise) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("BERMUDA_PUT", bermuda_put(spot, 100.0, 1.0, {tp(0.25), tp(0.5), tp(0.75)}));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.method = GreekMethod::Pathwise;

    try {
        engine::greeks::compute_greek(
            registries, request, model, product, flat_market(), pricing_context(20'000, 41), cpu_execution()
        );
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("Exercise"), std::string::npos) << e.what();
    }
}

TEST(GreeksFase7Test, ExplicitPathwiseRejectsAnIndicatorMetric) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product(
        "UP_AND_IN", up_and_in_call(spot, 120.0, 100.0, 1.0, {tp(0.25), tp(0.5), tp(0.75), tp(1.0)})
    );
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffHitProbabilityQ";
    request.metric_params = Params{{"event", std::string("UI")}};
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Pathwise;

    try {
        engine::greeks::compute_greek(
            registries, request, model, product, flat_market(), pricing_context(20'000, 7), cpu_execution()
        );
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("indicador"), std::string::npos) << e.what();
    }
}

TEST(GreeksFase7Test, ExplicitPathwiseRejectsAnUnverifiedCombination) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    GreekRequest request;
    request.metric_name = "PV";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "r0", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Pathwise;

    try {
        engine::greeks::compute_greek(
            registries, request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        );
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("no verificada"), std::string::npos) << e.what();
    }
}

TEST(GreeksFase7Test, PathwiseSpotDeltaUnderPMatchesExplicitBumpAndRevalForForecastP) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmPModel model = make_gbm_p(100.0, 0.08, 0.2, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffForecastP";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    engine::greeks::GreekResult pathwise = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );
    EXPECT_EQ(pathwise.method_used, GreekMethod::Pathwise);
    EXPECT_EQ(pathwise.measure, pf::ProbabilityMeasure::PhysicalP);

    GreekRequest bump_request = request;
    bump_request.method = GreekMethod::BumpAndReval;
    engine::greeks::GreekResult bump_and_reval = engine::greeks::compute_greek(
        registries, bump_request, model, product, flat_market(), pricing_context(200'000, 7), cpu_execution()
    );
    EXPECT_EQ(bump_and_reval.method_used, GreekMethod::BumpAndReval);

    EXPECT_NEAR(pathwise.value, bump_and_reval.value, 0.05)
        << "pathwise=" << pathwise.value << " bump_and_reval=" << bump_and_reval.value;
}

TEST(GreeksFase7Test, HullWhiteModelNpvMeasureMatchesTheFreeFunctions) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);

    auto measure = registries.measures.create("HullWhiteModelNpv", Params{});

    engine::HullWhite1FModel hw1f = make_hull_white();
    engine::MeasureResult result_1f =
        measure->evaluate(hw1f, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution());
    ASSERT_TRUE(result_1f.has_scalar);
    double expected_1f = engine::irs_hull_white_npv(
        hw1f.a(), hw1f.b(), hw1f.sigma(), hw1f.r0(), swap.notional(), swap.fixed_rate(), swap.use_par_rate(),
        swap.start(), swap.payment_times(), swap.accruals()
    );
    EXPECT_DOUBLE_EQ(result_1f.scalar, expected_1f);

    engine::HullWhite2FModel hw2f = make_hull_white_2f();
    engine::MeasureResult result_2f =
        measure->evaluate(hw2f, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution());
    ASSERT_TRUE(result_2f.has_scalar);
    double expected_2f = engine::irs_hull_white_2f_npv(
        hw2f.a(), hw2f.b(), hw2f.sigma(), hw2f.eta(), hw2f.rho(), hw2f.r0(), swap.notional(), swap.fixed_rate(),
        swap.use_par_rate(), swap.start(), swap.payment_times(), swap.accruals()
    );
    EXPECT_DOUBLE_EQ(result_2f.scalar, expected_2f);
}

TEST(GreeksFase7Test, AadMatchesExplicitBumpAndRevalForHullWhite1FModelNpvAllParameters) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    for (const std::string& risk_factor_name : {"a", "b", "sigma", "r0"}) {
        GreekRequest request;
        request.metric_name = "HullWhiteModelNpv";
        request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", risk_factor_name, std::nullopt};
        request.order = GreekOrder{1, std::nullopt};
        request.method = GreekMethod::Auto;

        engine::greeks::GreekResult aad = engine::greeks::compute_greek(
            registries, request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        );
        EXPECT_EQ(aad.method_used, GreekMethod::AadReverse) << "risk_factor=" << risk_factor_name;

        GreekRequest bump_request = request;
        bump_request.method = GreekMethod::BumpAndReval;
        engine::greeks::GreekResult bump_and_reval = engine::greeks::compute_greek(
            registries, bump_request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        );
        EXPECT_EQ(bump_and_reval.method_used, GreekMethod::BumpAndReval);

        // Tolerancia relativa (no el 1e-6 con h=1e-4 de
        // irs_hull_white_npv_all_greeks_matches_bump_and_reval_for_a_b_sigma en Rust): aqui el
        // bump-and-reval usa la politica de bump POR DEFECTO de §4.3 (1% relativo, no 1e-4), asi
        // que el error de truncamiento de la diferencia central es mayor -- 1e-3 sigue siendo
        // mucho mas ajustado que el error observado (~1e-5 relativo).
        const double tolerance = 1e-3 * std::max(1.0, std::abs(bump_and_reval.value));
        EXPECT_NEAR(aad.value, bump_and_reval.value, tolerance)
            << "risk_factor=" << risk_factor_name << " aad=" << aad.value << " bump_and_reval=" << bump_and_reval.value;
    }
}

TEST(GreeksFase7Test, AadMatchesExplicitBumpAndRevalForHullWhite2FModelNpvExcludingRho) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite2FModel model = make_hull_white_2f();

    for (const std::string& risk_factor_name : {"a", "b", "sigma", "eta", "r0"}) {
        GreekRequest request;
        request.metric_name = "HullWhiteModelNpv";
        request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", risk_factor_name, std::nullopt};
        request.order = GreekOrder{1, std::nullopt};
        request.method = GreekMethod::Auto;

        engine::greeks::GreekResult aad = engine::greeks::compute_greek(
            registries, request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        );
        EXPECT_EQ(aad.method_used, GreekMethod::AadReverse) << "risk_factor=" << risk_factor_name;

        GreekRequest bump_request = request;
        bump_request.method = GreekMethod::BumpAndReval;
        engine::greeks::GreekResult bump_and_reval = engine::greeks::compute_greek(
            registries, bump_request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        );

        const double tolerance = 1e-3 * std::max(1.0, std::abs(bump_and_reval.value)); // ver el comentario del test 1F
        EXPECT_NEAR(aad.value, bump_and_reval.value, tolerance)
            << "risk_factor=" << risk_factor_name << " aad=" << aad.value << " bump_and_reval=" << bump_and_reval.value;
    }
}

TEST(GreeksFase7Test, AutoFallsBackToBumpAndRevalForRhoOfHullWhite2FModelNpv) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite2FModel model = make_hull_white_2f();

    GreekRequest request;
    request.metric_name = "HullWhiteModelNpv";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "rho", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::Auto;

    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
    );
    EXPECT_EQ(result.method_used, GreekMethod::BumpAndReval)
        << "'rho' de HullWhite2F no es diferenciable via AAD (no es un tensor en el modelo), PLAN_GREEKS.md §5.2";
    ASSERT_TRUE(result.bump_used.has_value());
}

TEST(GreeksFase7Test, ExplicitAadRejectsRhoOfHullWhite2F) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite2FModel model = make_hull_white_2f();

    GreekRequest request;
    request.metric_name = "HullWhiteModelNpv";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "rho", std::nullopt};
    request.order = GreekOrder{1, std::nullopt};
    request.method = GreekMethod::AadReverse;

    try {
        engine::greeks::compute_greek(
            registries, request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        );
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("rho"), std::string::npos) << e.what();
    }
}

TEST(GreeksFase7Test, ExplicitMethodRejectsOrderTwoAndCrossFactor) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    GreekRequest order_two_request;
    order_two_request.metric_name = "HullWhiteModelNpv";
    order_two_request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "r0", std::nullopt};
    order_two_request.order = GreekOrder{2, std::nullopt};
    order_two_request.method = GreekMethod::AadReverse;
    EXPECT_THROW(
        engine::greeks::compute_greek(
            registries, order_two_request, model, swap, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
        ),
        std::invalid_argument
    );

    GreekRequest cross_factor_request;
    cross_factor_request.metric_name = "HullWhiteModelNpv";
    cross_factor_request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "r0", std::nullopt};
    cross_factor_request.order =
        GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "a", std::nullopt}};
    cross_factor_request.method = GreekMethod::AadReverse;
    EXPECT_THROW(
        engine::greeks::compute_greek(
            registries, cross_factor_request, model, swap, upward_sloping_market(), pricing_context(1'000, 7),
            cpu_execution()
        ),
        std::invalid_argument
    );
}

TEST(GreeksFase7Test, GreekMeasureReachesHullWhiteModelNpvAadThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    engine::PriceResult result = engine::price(
        registries, swap,
        std::vector<engine::MeasureSpec>{
            {"Greek",
             Params{
                 {"metric", std::string("HullWhiteModelNpv")}, {"risk_factor", std::string("model.r0")},
                 {"method", std::string("aad")}
             }}
        },
        model, upward_sloping_market(), pricing_context(1'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_NE(result[0].result.scalar, 0.0);
}

// PLAN_GREEKS.md §11 Fase 8 (lote): "Greek" no tiene ruta vectorizada dedicada en price.cpp
// (`known_batch_registered_types()` sigue siendo solo {"PV","DV01","UnilateralCVA",
// "ExposureProfile"}), pero desde esta fase ya no lanza dentro de un lote de `IrSwapProduct` --
// cae en un fallback genérico producto a producto (`Registry<IMeasure>::create(...)->evaluate`)
// dentro de `price_batch`. Se verifica trade a trade contra `price()` (que ya delega en
// `compute_greek` para "Greek"), mismo patrón que `PriceBatch.MatchesALoopOfScalarCallsPerTrade`
// en test_registry.cpp.
TEST(GreeksFase8Test, PriceBatchOfIrsMixesPvDv01AndGreekMatchingALoopOfScalarCalls) {
    Registries registries;
    register_builtins(registries);

    engine::HullWhite1FModel model = make_hull_white();
    engine::IrSwapProduct product_a = make_irs(1'000'000.0, 0.02);
    engine::IrSwapProduct product_b = make_irs(2'500'000.0, 0.015);
    MarketSnapshot market = upward_sloping_market();
    PricingContext pricing = pricing_context(1'000, 7);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{&product_a, &product_b};
    std::vector<engine::MeasureSpec> measures = {
        {"PV", {}},
        {"DV01", {}},
        {"Greek", Params{{"metric", std::string("PV")}, {"risk_factor", std::string("curve.parallel")}}},
        {"Greek",
         Params{
             {"metric", std::string("HullWhiteModelNpv")}, {"risk_factor", std::string("model.a")},
             {"method", std::string("aad")}
         }},
    };

    engine::PriceBatchResult batch = engine::price_batch(registries, products, measures, model, market, pricing, execution);
    ASSERT_EQ(batch.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        EXPECT_EQ(batch[i].trade_index, i);
        engine::PriceResult scalar = engine::price(registries, *products[i], measures, model, market, pricing, execution);
        ASSERT_EQ(batch[i].measures.size(), scalar.size());
        for (std::size_t m = 0; m < measures.size(); ++m) {
            ASSERT_TRUE(batch[i].measures[m].result.has_scalar) << "trade " << i << " medida " << m;
            ASSERT_TRUE(scalar[m].result.has_scalar) << "trade " << i << " medida " << m;
            EXPECT_NEAR(batch[i].measures[m].result.scalar, scalar[m].result.scalar, 1e-6)
                << "trade " << i << " medida " << m;
        }
    }
    // Sanity: los dos "Greek" realmente calcularon algo (no degeneraron a 0 por accidente).
    EXPECT_NE(batch[0].measures[2].result.scalar, 0.0);
    EXPECT_NE(batch[0].measures[3].result.scalar, 0.0);
}

// Antes de Fase 8, price_many delegaba en price_batch para cada grupo de IrSwapProduct del mismo
// calendario y ESE price_batch lanzaba std::invalid_argument("medida desconocida o no soportada
// en lote: 'Greek'") en cuanto una spec no estaba en el conjunto cerrado de 4 medidas
// vectorizadas -- por eso "price_many mezcla PV/DV01/Greek" no funcionaba en absoluto sobre IRS.
// Este test incluye ademas dos calendarios distintos (dos grupos) para ejercitar el fallback
// dentro de cada grupo, no solo dentro de un unico price_batch.
TEST(GreeksFase8Test, PriceManyGroupsIrsTradesAndAllowsGreekAcrossDifferentCalendars) {
    Registries registries;
    register_builtins(registries);

    engine::HullWhite1FModel model = make_hull_white();
    engine::IrSwapProduct product_a = make_irs(1'000'000.0, 0.02);  // calendario 1/2/3
    engine::IrSwapProduct product_b = make_irs(2'500'000.0, 0.015); // mismo calendario que a
    engine::IrSwapProduct product_c(Params{
        {"notional", 500'000.0},
        {"fixed_rate", 0.025},
        {"payment_times", std::vector<double>{0.5, 1.0}},
        {"accruals", std::vector<double>{0.5, 0.5}},
    }); // calendario distinto -> grupo propio dentro de price_many

    MarketSnapshot market = upward_sloping_market();
    PricingContext pricing = pricing_context(1'000, 7);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{&product_a, &product_b, &product_c};
    std::vector<engine::MeasureSpec> measures = {
        {"PV", {}},
        {"Greek", Params{{"metric", std::string("PV")}, {"risk_factor", std::string("curve.parallel")}}},
    };

    engine::PriceBatchResult many = engine::price_many(registries, products, measures, model, market, pricing, execution);
    ASSERT_EQ(many.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        EXPECT_EQ(many[i].trade_index, i);
        engine::PriceResult scalar = engine::price(registries, *products[i], measures, model, market, pricing, execution);
        for (std::size_t m = 0; m < measures.size(); ++m) {
            ASSERT_TRUE(many[i].measures[m].result.has_scalar);
            EXPECT_NEAR(many[i].measures[m].result.scalar, scalar[m].result.scalar, 1e-6) << "trade " << i << " medida " << m;
        }
    }
}

// Documenta (y protege de regresion) que un lote de PayoffProduct YA permitia mezclar "Greek" con
// "PayoffPriceQ" antes de Fase 8 -- price_batch_generic despacha cualquier medida registrada
// producto a producto con dedup por fingerprint (canonical_hash), sin la lista cerrada que sí
// tenia (y ya no tiene) el camino de IrSwapProduct. product_a/product_b comparten AST exacto
// (mismo id, mismo contrato) -> mismo canonical_hash -> el fingerprint cache de
// price_batch_generic reutiliza el resultado completo para el segundo producto, byte a byte.
TEST(GreeksFase8Test, PriceBatchOfPayoffProductsAlreadyAllowedGreekWithPayoffPriceQBeforeFase8) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.XYZ");
    MarketSnapshot market = flat_market();
    PricingContext pricing = pricing_context(50'000, 7);
    ExecutionContext execution = cpu_execution();

    pf::PayoffProduct product_a("CALL", european_call(spot, 100.0, 1.0));
    pf::PayoffProduct product_b("CALL", european_call(spot, 100.0, 1.0));

    std::vector<const engine::IProduct*> products{&product_a, &product_b};
    std::vector<engine::MeasureSpec> measures = {
        {"PayoffPriceQ", {}},
        {"Greek", Params{{"metric", std::string("PayoffPriceQ")}, {"risk_factor", std::string("model.spot")}}},
    };

    engine::PriceBatchResult batch = engine::price_batch(registries, products, measures, model, market, pricing, execution);
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_EQ(batch[0].measures[0].result.scalar, batch[1].measures[0].result.scalar);
    EXPECT_EQ(batch[0].measures[1].result.scalar, batch[1].measures[1].result.scalar);
    EXPECT_NE(batch[0].measures[1].result.scalar, 0.0);
}

// Non-regresion explicita: un nombre que no resuelve a NINGUNA medida (ni vectorizada ni
// registrada) sigue rechazandose dentro de un lote de IrSwapProduct -- el fallback generico de
// Fase 8 amplia lo que se acepta, no lo desactiva.
TEST(GreeksFase8Test, PriceBatchStillRejectsATrulyUnknownMeasureName) {
    Registries registries;
    register_builtins(registries);
    engine::HullWhite1FModel model = make_hull_white();
    engine::IrSwapProduct product = make_irs(1'000'000.0, 0.02);
    std::vector<const engine::IProduct*> products{&product};

    EXPECT_THROW(
        engine::price_batch(
            registries, products, std::vector<std::string>{"NoSuchMeasure"}, model, upward_sloping_market(),
            pricing_context(1'000, 7), cpu_execution()
        ),
        std::invalid_argument
    );
}

// PLAN_GREEKS.md §9.4 (criterio de aceptacion cruzado de la Fase 9): el MISMO fixture JSON
// (docs/schema/engine.payoff/examples/call.json, reutilizado de PLAN_PRODUCTS.md, no uno
// nuevo) debe producir el mismo conjunto de risk_factor, el mismo valor por factor y el mismo
// `skipped` en la capa nucleo C++ (compute_all_greeks) y en la C ABI (engine_abi_all_greeks) --
// analogo a PayoffFixturesCrossLayerTest (test_payoff_fixtures_cross_layer.cpp), que hace lo
// mismo para explain()/hash. La cobertura Python (nanobind vs C ABI via ctypes) vive en
// clients/python/tests/test_greeks_fixtures_cross_layer.py, mismo patron que
// test_payoff_fixtures_cross_layer.py -- las tres capas juntas satisfacen "ejercitado desde
// C++, Python y la sonda C ABI en un unico test de integracion" sin repetir infraestructura.
TEST(GreeksFase9Test, CoreAndAbiAgreeOnAllGreeksForTheCallFixture) {
    std::string spec = read_fixture_file("call.json");

    Registries registries;
    register_builtins(registries);
    auto product = registries.products.create("Payoff", Params{{"spec", spec}});
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.AAPL");
    MarketSnapshot market({1.0}, {0.05});
    PricingContext pricing = pricing_context(50'000, 7);
    ExecutionContext execution = cpu_execution();

    engine::greeks::GreeksReport core_report = engine::greeks::compute_all_greeks(
        registries, "PayoffPriceQ", Params{}, model, *product, market, pricing, execution
    );

    EngineParam spec_param{"spec", ENGINE_PARAM_STRING, 0.0, nullptr, 0, spec.c_str()};
    EngineProduct* abi_product = engine_abi_create_product("Payoff", &spec_param, 1);
    ASSERT_NE(abi_product, nullptr);
    EngineParam model_params[] = {
        EngineParam{"s0", ENGINE_PARAM_DOUBLE, 100.0, nullptr, 0, nullptr},
        EngineParam{"r", ENGINE_PARAM_DOUBLE, 0.05, nullptr, 0, nullptr},
        EngineParam{"q", ENGINE_PARAM_DOUBLE, 0.0, nullptr, 0, nullptr},
        EngineParam{"sigma", ENGINE_PARAM_DOUBLE, 0.2, nullptr, 0, nullptr},
        EngineParam{"observable", ENGINE_PARAM_STRING, 0.0, nullptr, 0, "EQ.SPOT.AAPL"},
    };
    EngineModel* abi_model = engine_abi_create_model("GBM", model_params, 5);
    ASSERT_NE(abi_model, nullptr);

    double pillars[] = {1.0};
    double zero_rates[] = {0.05};
    EngineMarketSnapshot abi_market{pillars, zero_rates, 1, 0.0, 0.0};
    EnginePricingContext abi_pricing{0.0, 50'000, 1, 7};
    EngineExecutionContext abi_execution{"cpu", "fp64"};

    EngineGreekResultEntry* greek_entries = nullptr;
    std::size_t n_greeks = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    int rc = engine_abi_all_greeks(
        abi_product, "PayoffPriceQ", nullptr, 0, abi_model, &abi_market, &abi_pricing, &abi_execution,
        /*include_curve_buckets=*/0, /*include_second_order=*/0, &greek_entries, &n_greeks, &skipped, &n_skipped
    );
    ASSERT_EQ(rc, 0);

    ASSERT_EQ(core_report.greeks.size(), n_greeks);
    for (const auto& core_greek : core_report.greeks) {
        const std::string core_factor = engine::greeks::to_string(core_greek.risk_factor);
        bool matched = false;
        for (std::size_t i = 0; i < n_greeks; ++i) {
            if (core_factor != greek_entries[i].risk_factor) continue;
            matched = true;
            EXPECT_EQ(core_greek.has_scalar, greek_entries[i].result.has_scalar != 0) << core_factor;
            EXPECT_DOUBLE_EQ(core_greek.value, greek_entries[i].result.scalar) << core_factor;
            EXPECT_EQ(engine::greeks::to_string(core_greek.method_used), std::string(greek_entries[i].method_used)) << core_factor;
            EXPECT_EQ(engine::greeks::to_string(core_greek.measure), std::string(greek_entries[i].measure)) << core_factor;
            EXPECT_EQ(core_greek.bump_used.has_value(), greek_entries[i].has_bump != 0) << core_factor;
            if (core_greek.bump_used.has_value() && greek_entries[i].has_bump) {
                EXPECT_DOUBLE_EQ(*core_greek.bump_used, greek_entries[i].bump_used) << core_factor;
            }
            break;
        }
        EXPECT_TRUE(matched) << "risk_factor de compute_all_greeks ausente en engine_abi_all_greeks: " << core_factor;
    }

    ASSERT_EQ(core_report.skipped.size(), n_skipped);
    for (const std::string& reason : core_report.skipped) {
        bool matched = false;
        for (std::size_t i = 0; i < n_skipped; ++i) {
            if (reason == skipped[i]) { matched = true; break; }
        }
        EXPECT_TRUE(matched) << "skipped de compute_all_greeks ausente en engine_abi_all_greeks: " << reason;
    }

    engine_abi_free_greeks_report(greek_entries, n_greeks, skipped, n_skipped);
    engine_abi_free_product(abi_product);
    engine_abi_free_model(abi_model);
}

// PLAN_BACKWARD.md §9 (verificacion final, C ABI): mismo criterio cruzado que
// GreeksFase9Test.CoreAndAbiAgreeOnAllGreeksForTheCallFixture de arriba, pero para
// engine_abi_hessian -- mismo fixture GBM/call que
// GreeksHessianTest.ComputeHessianWithEmptyFactorsReturnsGammaVolgaVannaAllViaLikelihoodRatio
// (Gamma/Volga/Vanna via likelihood ratio, factors={} = enumeracion automatica).
TEST(GreeksBackwardAbiTest, CoreAndAbiAgreeOnHessianForTheGbmCallFixture) {
    std::string spec = read_fixture_file("call.json");

    Registries registries;
    register_builtins(registries);
    auto product = registries.products.create("Payoff", Params{{"spec", spec}});
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.AAPL");
    MarketSnapshot market({1.0}, {0.05});

    engine::greeks::HessianReport core_report = engine::greeks::compute_hessian(
        registries, "PayoffPriceQ", Params{}, model, *product, market, pricing_context(500'000, 7), cpu_execution()
    );
    ASSERT_TRUE(core_report.skipped.empty());
    ASSERT_EQ(core_report.entries.size(), 3u);

    EngineParam spec_param{"spec", ENGINE_PARAM_STRING, 0.0, nullptr, 0, spec.c_str()};
    EngineProduct* abi_product = engine_abi_create_product("Payoff", &spec_param, 1);
    ASSERT_NE(abi_product, nullptr);
    EngineParam model_params[] = {
        EngineParam{"s0", ENGINE_PARAM_DOUBLE, 100.0, nullptr, 0, nullptr},
        EngineParam{"r", ENGINE_PARAM_DOUBLE, 0.05, nullptr, 0, nullptr},
        EngineParam{"q", ENGINE_PARAM_DOUBLE, 0.0, nullptr, 0, nullptr},
        EngineParam{"sigma", ENGINE_PARAM_DOUBLE, 0.2, nullptr, 0, nullptr},
        EngineParam{"observable", ENGINE_PARAM_STRING, 0.0, nullptr, 0, "EQ.SPOT.AAPL"},
    };
    EngineModel* abi_model = engine_abi_create_model("GBM", model_params, 5);
    ASSERT_NE(abi_model, nullptr);

    double pillars[] = {1.0};
    double zero_rates[] = {0.05};
    EngineMarketSnapshot abi_market{pillars, zero_rates, 1, 0.0, 0.0};
    EnginePricingContext abi_pricing{0.0, 500'000, 1, 7};
    EngineExecutionContext abi_execution{"cpu", "fp64"};

    EngineHessianEntry* entries = nullptr;
    std::size_t n_entries = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    int rc = engine_abi_hessian(
        abi_product, "PayoffPriceQ", nullptr, 0, abi_model, &abi_market, &abi_pricing, &abi_execution, nullptr, 0,
        &entries, &n_entries, &skipped, &n_skipped
    );
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(n_skipped, 0u);
    ASSERT_EQ(core_report.entries.size(), n_entries);

    for (const auto& core_entry : core_report.entries) {
        const std::string i = engine::greeks::to_string(core_entry.factor_i);
        const std::string j = engine::greeks::to_string(core_entry.factor_j);
        bool matched = false;
        for (std::size_t k = 0; k < n_entries; ++k) {
            if (entries[k].risk_factor_i != i || entries[k].risk_factor_j != j) continue;
            matched = true;
            EXPECT_DOUBLE_EQ(core_entry.value, entries[k].value) << i << "/" << j;
            EXPECT_EQ(engine::greeks::to_string(core_entry.method_used), std::string(entries[k].method_used));
            EXPECT_EQ(core_entry.std_error.has_value(), entries[k].has_std_error != 0);
            break;
        }
        EXPECT_TRUE(matched) << "entrada de compute_hessian ausente en engine_abi_hessian: " << i << "/" << j;
    }

    engine_abi_free_hessian(entries, n_entries, skipped, n_skipped);
    engine_abi_free_product(abi_product);
    engine_abi_free_model(abi_model);
}

// Mismo criterio cruzado, para engine_abi_hvp -- direccion = vector base e_spot (peso 1 en
// "model.spot", 0 en "model.volatility") sobre el mismo fixture GBM/call de arriba.
TEST(GreeksBackwardAbiTest, CoreAndAbiAgreeOnHvpForTheGbmCallFixture) {
    std::string spec = read_fixture_file("call.json");

    Registries registries;
    register_builtins(registries);
    auto product = registries.products.create("Payoff", Params{{"spec", spec}});
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.AAPL");
    MarketSnapshot market({1.0}, {0.05});

    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt},
    };
    std::vector<double> direction = {1.0, 0.0};

    engine::greeks::HvpReport core_report = engine::greeks::compute_hvp(
        registries, "PayoffPriceQ", Params{}, model, *product, market, pricing_context(500'000, 7), cpu_execution(),
        factors, direction
    );
    ASSERT_TRUE(core_report.skipped.empty());
    ASSERT_EQ(core_report.components.size(), 2u);

    EngineParam spec_param{"spec", ENGINE_PARAM_STRING, 0.0, nullptr, 0, spec.c_str()};
    EngineProduct* abi_product = engine_abi_create_product("Payoff", &spec_param, 1);
    ASSERT_NE(abi_product, nullptr);
    EngineParam model_params[] = {
        EngineParam{"s0", ENGINE_PARAM_DOUBLE, 100.0, nullptr, 0, nullptr},
        EngineParam{"r", ENGINE_PARAM_DOUBLE, 0.05, nullptr, 0, nullptr},
        EngineParam{"q", ENGINE_PARAM_DOUBLE, 0.0, nullptr, 0, nullptr},
        EngineParam{"sigma", ENGINE_PARAM_DOUBLE, 0.2, nullptr, 0, nullptr},
        EngineParam{"observable", ENGINE_PARAM_STRING, 0.0, nullptr, 0, "EQ.SPOT.AAPL"},
    };
    EngineModel* abi_model = engine_abi_create_model("GBM", model_params, 5);
    ASSERT_NE(abi_model, nullptr);

    double pillars[] = {1.0};
    double zero_rates[] = {0.05};
    EngineMarketSnapshot abi_market{pillars, zero_rates, 1, 0.0, 0.0};
    EnginePricingContext abi_pricing{0.0, 500'000, 1, 7};
    EngineExecutionContext abi_execution{"cpu", "fp64"};

    const char* direction_factors[] = {"model.spot", "model.volatility"};
    double direction_weights[] = {1.0, 0.0};

    EngineHvpComponent* components = nullptr;
    std::size_t n_components = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    int rc = engine_abi_hvp(
        abi_product, "PayoffPriceQ", nullptr, 0, abi_model, &abi_market, &abi_pricing, &abi_execution,
        direction_factors, direction_weights, 2, &components, &n_components, &skipped, &n_skipped
    );
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(n_skipped, 0u);
    ASSERT_EQ(core_report.components.size(), n_components);

    for (const auto& core_c : core_report.components) {
        const std::string name = engine::greeks::to_string(core_c.factor);
        bool matched = false;
        for (std::size_t k = 0; k < n_components; ++k) {
            if (components[k].risk_factor != name) continue;
            matched = true;
            EXPECT_DOUBLE_EQ(core_c.value, components[k].value) << name;
            EXPECT_EQ(engine::greeks::to_string(core_c.method_used), std::string(components[k].method_used));
            break;
        }
        EXPECT_TRUE(matched) << "componente de compute_hvp ausente en engine_abi_hvp: " << name;
    }

    engine_abi_free_hvp(components, n_components, skipped, n_skipped);
    engine_abi_free_product(abi_product);
    engine_abi_free_model(abi_model);
}

// PLAN_BACKWARD.md §9: engine_abi_hessian es "mejor esfuerzo" igual que compute_hessian --
// Hull-White1F + "PV" no esta en hessian_capabilities() (ver
// GreeksHessianTest.ComputeHessianOnAnUnsupportedModelMetricCombinationGoesToSkippedWithoutThrowing
// arriba), asi que cae en *out_skipped con return == 0, nunca return != 0.
TEST(GreeksBackwardAbiTest, HessianOnAnUnsupportedCombinationReturnsZeroWithEmptyEntriesAndNonEmptySkipped) {
    EngineParam model_params[] = {
        EngineParam{"a", ENGINE_PARAM_DOUBLE, 0.1, nullptr, 0, nullptr},
        EngineParam{"b", ENGINE_PARAM_DOUBLE, 0.03, nullptr, 0, nullptr},
        EngineParam{"sigma", ENGINE_PARAM_DOUBLE, 0.01, nullptr, 0, nullptr},
        EngineParam{"r0", ENGINE_PARAM_DOUBLE, 0.02, nullptr, 0, nullptr},
    };
    EngineModel* abi_model = engine_abi_create_model("HullWhite1F", model_params, 4);
    ASSERT_NE(abi_model, nullptr);

    double payment_times[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double accruals[] = {1.0, 1.0, 1.0, 1.0, 1.0};
    EngineParam product_params[] = {
        EngineParam{"notional", ENGINE_PARAM_DOUBLE, 1'000'000.0, nullptr, 0, nullptr},
        EngineParam{"fixed_rate", ENGINE_PARAM_DOUBLE, 0.02, nullptr, 0, nullptr},
        EngineParam{"payment_times", ENGINE_PARAM_VECTOR, 0.0, payment_times, 5, nullptr},
        EngineParam{"accruals", ENGINE_PARAM_VECTOR, 0.0, accruals, 5, nullptr},
    };
    EngineProduct* abi_product = engine_abi_create_product("IRSwap", product_params, 4);
    ASSERT_NE(abi_product, nullptr);

    double pillars[] = {1.0, 2.0};
    double zero_rates[] = {0.02, 0.02};
    EngineMarketSnapshot abi_market{pillars, zero_rates, 2, 0.0, 0.0};
    EnginePricingContext abi_pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext abi_execution{"cpu", "fp64"};

    EngineHessianEntry* entries = nullptr;
    std::size_t n_entries = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    int rc = engine_abi_hessian(
        abi_product, "PV", nullptr, 0, abi_model, &abi_market, &abi_pricing, &abi_execution, nullptr, 0, &entries,
        &n_entries, &skipped, &n_skipped
    );

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(n_entries, 0u);
    EXPECT_GT(n_skipped, 0u);

    engine_abi_free_hessian(entries, n_entries, skipped, n_skipped);
    engine_abi_free_product(abi_product);
    engine_abi_free_model(abi_model);
}

// PLAN_BACKWARD.md §9: NULLs -> return != 0 + engine_abi_last_error no vacio, mismo criterio que
// el resto de la C ABI (engine_abi_all_greeks incluido).
TEST(GreeksBackwardAbiTest, HessianWithNullProductReturnsNonZeroAndSetsLastError) {
    EngineHessianEntry* entries = nullptr;
    std::size_t n_entries = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    int rc = engine_abi_hessian(
        nullptr, "PayoffPriceQ", nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &entries, &n_entries,
        &skipped, &n_skipped
    );

    EXPECT_NE(rc, 0);
    EXPECT_EQ(entries, nullptr);
    EXPECT_EQ(n_entries, 0u);
    EXPECT_EQ(skipped, nullptr);
    EXPECT_EQ(n_skipped, 0u);

    std::size_t len = engine_abi_last_error(nullptr, 0);
    EXPECT_GT(len, 0u);
}

TEST(GreeksBackwardAbiTest, HvpWithNullDirectionReturnsNonZeroAndSetsLastError) {
    std::string spec = read_fixture_file("call.json");

    EngineParam model_params[] = {
        EngineParam{"s0", ENGINE_PARAM_DOUBLE, 100.0, nullptr, 0, nullptr},
        EngineParam{"r", ENGINE_PARAM_DOUBLE, 0.05, nullptr, 0, nullptr},
        EngineParam{"q", ENGINE_PARAM_DOUBLE, 0.0, nullptr, 0, nullptr},
        EngineParam{"sigma", ENGINE_PARAM_DOUBLE, 0.2, nullptr, 0, nullptr},
        EngineParam{"observable", ENGINE_PARAM_STRING, 0.0, nullptr, 0, "EQ.SPOT.AAPL"},
    };
    EngineModel* abi_model = engine_abi_create_model("GBM", model_params, 5);
    ASSERT_NE(abi_model, nullptr);

    EngineParam spec_param{"spec", ENGINE_PARAM_STRING, 0.0, nullptr, 0, spec.c_str()};
    EngineProduct* abi_product = engine_abi_create_product("Payoff", &spec_param, 1);
    ASSERT_NE(abi_product, nullptr);

    double pillars[] = {1.0};
    double zero_rates[] = {0.05};
    EngineMarketSnapshot abi_market{pillars, zero_rates, 1, 0.0, 0.0};
    EnginePricingContext abi_pricing{0.0, 500'000, 1, 7};
    EngineExecutionContext abi_execution{"cpu", "fp64"};

    EngineHvpComponent* components = nullptr;
    std::size_t n_components = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    int rc = engine_abi_hvp(
        abi_product, "PayoffPriceQ", nullptr, 0, abi_model, &abi_market, &abi_pricing, &abi_execution, nullptr, nullptr,
        0, &components, &n_components, &skipped, &n_skipped
    );

    EXPECT_NE(rc, 0);
    EXPECT_EQ(components, nullptr);
    EXPECT_EQ(n_components, 0u);
    std::size_t len = engine_abi_last_error(nullptr, 0);
    EXPECT_GT(len, 0u);

    engine_abi_free_product(abi_product);
    engine_abi_free_model(abi_model);
}

// --- PLAN_HYPERDUAL.md §5 (revisado): Gamma/Vanna via likelihood ratio -------------------------
//
// La generalizacion original del documento (`Dual2`/`HyperDual`) resulto matematicamente
// incorrecta para payoffs con kink (ver el doc-comment de `engine_core::payoff::lrm`). Estos tests
// verifican el reemplazo real: `method=Auto` debe preferir Pathwise (likelihood ratio) sobre el
// estencil generico de bump-and-reval para `(GBM, PayoffPriceQ)`/`(GBM_P, PayoffForecastP)` de un
// contrato de una unica fecha terminal, y debe seguir cayendo a bump-and-reval para un contrato
// path-dependiente (`payoff_supports_second_order_lrm` en `false`).

TEST(GreeksHyperdualTest, AutoResolvesGammaOfACallToLikelihoodRatioPathwise) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{2, std::nullopt};

    engine::greeks::GreekResult via_auto = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );

    double analytic_gamma = black_scholes_gamma(s0, strike, r, q, sigma, maturity);

    EXPECT_EQ(via_auto.method_used, engine::greeks::GreekMethod::Pathwise)
        << "Auto debe preferir likelihood-ratio (verificado) sobre bump-and-reval";
    EXPECT_NEAR(via_auto.value, analytic_gamma, 0.01) << "Greek=" << via_auto.value << " analytic=" << analytic_gamma;

    request.method = GreekMethod::Pathwise;
    engine::greeks::GreekResult via_explicit = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );
    EXPECT_EQ(via_explicit.method_used, engine::greeks::GreekMethod::Pathwise);
    EXPECT_NEAR(via_explicit.value, analytic_gamma, 0.01);
}

TEST(GreeksHyperdualTest, AutoResolvesVannaOfACallToLikelihoodRatioPathwise) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const double h_spot = 1.0, h_vol = 0.002;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt}};

    engine::greeks::GreekResult via_auto = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );

    double manual =
        (black_scholes_call(s0 + h_spot, strike, r, q, sigma + h_vol, maturity) -
         black_scholes_call(s0 + h_spot, strike, r, q, sigma - h_vol, maturity) -
         black_scholes_call(s0 - h_spot, strike, r, q, sigma + h_vol, maturity) +
         black_scholes_call(s0 - h_spot, strike, r, q, sigma - h_vol, maturity)) /
        (4.0 * h_spot * h_vol);

    EXPECT_EQ(via_auto.method_used, engine::greeks::GreekMethod::Pathwise);
    ASSERT_TRUE(via_auto.std_error.has_value());
    // El peso de Vanna via likelihood ratio involucra Z^3 (ver payoff::lrm::vanna_weight) -- mayor
    // varianza que un estimador pathwise puro, asi que la tolerancia se ancla al propio error
    // estandar del estimador Monte Carlo (mismo criterio que el resto de tests de sensibilidad
    // pathwise, no un valor fijo).
    double tolerance = 8.0 * *via_auto.std_error;
    EXPECT_NEAR(via_auto.value, manual, tolerance)
        << "Greek=" << via_auto.value << " (se=" << *via_auto.std_error << ") manual=" << manual;

    // El par inverso (volatility, spot) debe dar el mismo resultado (Vanna es simetrica).
    GreekRequest reversed = payoff_price_q_request("volatility");
    reversed.order = GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt}};
    engine::greeks::GreekResult via_reversed = engine::greeks::compute_greek(
        registries, reversed, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );
    EXPECT_EQ(via_reversed.method_used, engine::greeks::GreekMethod::Pathwise);
    EXPECT_NEAR(via_reversed.value, via_auto.value, 1e-9);
}

TEST(GreeksHyperdualTest, AutoResolvesGammaOfAForecastUnderPToLikelihoodRatioPathwise) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, mu = 0.08, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);

    GreekRequest request;
    request.metric_name = "PayoffForecastP";
    request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt};
    request.order = GreekOrder{2, std::nullopt};
    request.method = GreekMethod::Auto;

    engine::greeks::GreekResult via_auto = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );
    EXPECT_EQ(via_auto.method_used, engine::greeks::GreekMethod::Pathwise);
    EXPECT_EQ(via_auto.measure, pf::ProbabilityMeasure::PhysicalP);
}

TEST(GreeksHyperdualTest, AutoStillFallsBackToBumpAndRevalForAPathDependentContractsGamma) {
    // Un contrato path-dependiente (bermuda, con Exercise -- y por tanto mas de una fecha
    // requerida) no soporta Gamma/Vanna via likelihood ratio (PLAN_HYPERDUAL.md §5.2) --
    // payoff_supports_second_order_lrm debe ser `false`, y Auto debe seguir cayendo al estencil
    // generico de bump-and-reval, nunca aproximar en silencio de otra forma.
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("BERMUDA_PUT", bermuda_put(spot, 100.0, 1.0, {tp(0.25), tp(0.5), tp(0.75)}));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{2, std::nullopt};

    engine::greeks::GreekResult via_auto = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_context(20'000, 7), cpu_execution()
    );
    EXPECT_EQ(via_auto.method_used, engine::greeks::GreekMethod::BumpAndReval);

    request.method = GreekMethod::Pathwise;
    try {
        engine::greeks::compute_greek(
            registries, request, model, product, flat_market(), pricing_context(20'000, 7), cpu_execution()
        );
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("path-dependiente"), std::string::npos) << e.what();
    }
}

TEST(GreeksHyperdualTest, ExplicitPathwiseRejectsAVannaPairOtherThanSpotAndVolatility) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "rate", std::nullopt}};
    request.method = GreekMethod::Pathwise;

    try {
        engine::greeks::compute_greek(
            registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
        );
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("volatility"), std::string::npos) << e.what();
    }
}

// PLAN_BACKWARD.md §9 Fase 1: Hessiano local del motor de payoff (likelihood ratio, sin AD) --
// `compute_hessian` fusiona en UNA SOLA simulacion lo que `GreeksHyperdualTest` de arriba pedia
// como dos llamadas independientes a `compute_greek` (Gamma order=2, Vanna order=1
// cross=volatility). Mismo fixture/tolerancias que esos tests.

double black_scholes_vega(double s0, double strike, double r, double q, double sigma, double maturity) {
    double sqrt_t = std::sqrt(maturity);
    double d1 = (std::log(s0 / strike) + (r - q + 0.5 * sigma * sigma) * maturity) / (sigma * sqrt_t);
    return s0 * std::exp(-q * maturity) * norm_pdf(d1) * sqrt_t;
}

// Volga cerrado de Black-Scholes: `Vega * d1 * d2 / sigma` (convencion estandar, d1/d2 con la
// misma formula que `black_scholes_call`/`black_scholes_gamma` de arriba).
double black_scholes_volga(double s0, double strike, double r, double q, double sigma, double maturity) {
    double sqrt_t = std::sqrt(maturity);
    double d1 = (std::log(s0 / strike) + (r - q + 0.5 * sigma * sigma) * maturity) / (sigma * sqrt_t);
    double d2 = d1 - sigma * sqrt_t;
    return black_scholes_vega(s0, strike, r, q, sigma, maturity) * d1 * d2 / sigma;
}

TEST(GreeksHessianTest, ComputeHessianWithEmptyFactorsReturnsGammaVolgaVannaAllViaLikelihoodRatio) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );

    ASSERT_TRUE(report.skipped.empty()) << (report.skipped.empty() ? "" : report.skipped.front());
    ASSERT_EQ(report.entries.size(), 3u);
    for (const auto& entry : report.entries) {
        EXPECT_EQ(entry.method_used, engine::greeks::GreekMethod::LikelihoodRatioHessian);
        EXPECT_EQ(entry.measure, pf::ProbabilityMeasure::RiskNeutralQ);
    }

    auto find_entry = [&](const std::string& name_i, const std::string& name_j) -> const engine::greeks::HessianEntry& {
        for (const auto& entry : report.entries) {
            if (entry.factor_i.name == name_i && entry.factor_j.name == name_j) return entry;
        }
        throw std::runtime_error("entrada no encontrada: " + name_i + "/" + name_j);
    };
    const engine::greeks::HessianEntry& gamma_ss = find_entry("spot", "spot");
    const engine::greeks::HessianEntry& volga_vv = find_entry("volatility", "volatility");
    const engine::greeks::HessianEntry& vanna_sv = find_entry("spot", "volatility");

    double analytic_gamma = black_scholes_gamma(s0, strike, r, q, sigma, maturity);
    double analytic_volga = black_scholes_volga(s0, strike, r, q, sigma, maturity);
    const double h_spot = 1.0, h_vol = 0.002;
    double analytic_vanna =
        (black_scholes_call(s0 + h_spot, strike, r, q, sigma + h_vol, maturity) -
         black_scholes_call(s0 + h_spot, strike, r, q, sigma - h_vol, maturity) -
         black_scholes_call(s0 - h_spot, strike, r, q, sigma + h_vol, maturity) +
         black_scholes_call(s0 - h_spot, strike, r, q, sigma - h_vol, maturity)) /
        (4.0 * h_spot * h_vol);

    ASSERT_TRUE(gamma_ss.std_error.has_value());
    EXPECT_NEAR(gamma_ss.value, analytic_gamma, 8.0 * *gamma_ss.std_error)
        << "gamma=" << gamma_ss.value << " analytic=" << analytic_gamma;

    ASSERT_TRUE(volga_vv.std_error.has_value());
    EXPECT_NEAR(volga_vv.value, analytic_volga, 8.0 * *volga_vv.std_error)
        << "volga=" << volga_vv.value << " analytic=" << analytic_volga;

    ASSERT_TRUE(vanna_sv.std_error.has_value());
    EXPECT_NEAR(vanna_sv.value, analytic_vanna, 8.0 * *vanna_sv.std_error)
        << "vanna=" << vanna_sv.value << " analytic=" << analytic_vanna;

    // Mismo seed/n_paths que compute_greek (Gamma order=2 / Vanna cruzada existentes) -> UNA SOLA
    // simulacion reutilizada implica coincidencia bit a bit (verificado tambien del lado Rust,
    // rust/crates/engine-core/src/payoff/api.rs::local_hessian_matches_separate_gamma_and_vanna_calls...).
    GreekRequest gamma_request = payoff_price_q_request("spot");
    gamma_request.order = GreekOrder{2, std::nullopt};
    engine::greeks::GreekResult gamma_via_compute_greek = engine::greeks::compute_greek(
        registries, gamma_request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );
    EXPECT_NEAR(gamma_ss.value, gamma_via_compute_greek.value, 1e-9);

    GreekRequest vanna_request = payoff_price_q_request("spot");
    vanna_request.order = GreekOrder{1, RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt}};
    engine::greeks::GreekResult vanna_via_compute_greek = engine::greeks::compute_greek(
        registries, vanna_request, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution()
    );
    EXPECT_NEAR(vanna_sv.value, vanna_via_compute_greek.value, 1e-9);
}

TEST(GreeksHessianTest, ComputeHessianOnAnUnsupportedModelMetricCombinationGoesToSkippedWithoutThrowing) {
    // Hull-White + "PV" no esta en hessian_capabilities() (Fase 1 solo cubre GBM/GBM_P) --
    // compute_hessian es "mejor esfuerzo" (a diferencia de compute_greek), nunca lanza.
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "PV", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    EXPECT_TRUE(report.entries.empty());
    ASSERT_FALSE(report.skipped.empty());
    EXPECT_NE(report.skipped.front().find("PV"), std::string::npos) << report.skipped.front();
}

TEST(GreeksHessianTest, ComputeHessianRequestingAFactorOutsideSpotVolatilityGoesToSkippedForThatEntryOnly) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "rate", std::nullopt},
    };

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(200'000, 7), cpu_execution(),
        factors
    );

    // "rate" cae a skipped explicito; "spot" solo (sin "volatility" pedido) solo puede formar la
    // diagonal gamma_ss -- volga/vanna necesitan "volatility", que nadie pidio.
    ASSERT_EQ(report.entries.size(), 1u);
    EXPECT_EQ(report.entries.front().factor_i.name, "spot");
    EXPECT_EQ(report.entries.front().factor_j.name, "spot");

    bool found_rate_skip = std::any_of(report.skipped.begin(), report.skipped.end(), [](const std::string& msg) {
        return msg.find("rate") != std::string::npos;
    });
    EXPECT_TRUE(found_rate_skip);
}

// PLAN_BACKWARD.md §9 Fase 2: Hessiano cerrado de Hull-White 1F via Dual2/HyperDual NUEVOS
// (forward-over-forward). La comparacion fuerte valor/gradiente vs la ruta Burn ya se hizo del
// lado Rust (engine_core::models::hull_white_dual::tests) -- estos tests confirman el WIRING C++:
// `compute_hessian` resuelve "HullWhite1F"+"HullWhiteModelNpv" al metodo correcto, produce
// exactamente las 10 entradas esperadas, y coinciden con un estencil de bump-and-reval de segundo
// orden construido en el propio test sobre `engine::irs_hull_white_npv` (oraculo independiente,
// sin reutilizar el motor de Hessiano bajo prueba).

TEST(GreeksHessianHullWhiteTest, ComputeHessianWithEmptyFactorsReturnsAllTenEntriesViaForwardOverForward) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    ASSERT_TRUE(report.skipped.empty()) << (report.skipped.empty() ? "" : report.skipped.front());
    ASSERT_EQ(report.entries.size(), 10u);
    for (const auto& entry : report.entries) {
        EXPECT_EQ(entry.method_used, engine::greeks::GreekMethod::AadForwardOverForward);
        EXPECT_EQ(entry.measure, pf::ProbabilityMeasure::DeterministicScenario);
        EXPECT_FALSE(entry.std_error.has_value());
    }

    auto find_entry = [&](const std::string& name_i, const std::string& name_j) -> const engine::greeks::HessianEntry& {
        for (const auto& entry : report.entries) {
            if ((entry.factor_i.name == name_i && entry.factor_j.name == name_j) ||
                (entry.factor_i.name == name_j && entry.factor_j.name == name_i)) {
                return entry;
            }
        }
        throw std::runtime_error("entrada no encontrada: " + name_i + "/" + name_j);
    };

    const double a = model.a(), b = model.b(), sigma = model.sigma(), r0 = model.r0();
    const double notional = swap.notional(), fixed_rate = swap.fixed_rate(), start = swap.start();
    const std::vector<double>& payment_times = swap.payment_times();
    const std::vector<double>& accruals = swap.accruals();

    auto npv = [&](double a_, double b_, double sigma_, double r0_) {
        return engine::irs_hull_white_npv(a_, b_, sigma_, r0_, notional, fixed_rate, false, start, payment_times, accruals);
    };

    const double h_a = 1e-4 * std::max(std::abs(a), 1.0);
    const double h_b = 1e-4 * std::max(std::abs(b), 1.0);
    const double h_sigma = 1e-4 * std::max(std::abs(sigma), 1.0);
    const double h_r0 = 1e-4 * std::max(std::abs(r0), 1.0);
    auto tol = [](double reference) { return 1e-3 * std::max(std::abs(reference), 1.0); };

    double bump_aa = (npv(a + h_a, b, sigma, r0) - 2.0 * npv(a, b, sigma, r0) + npv(a - h_a, b, sigma, r0)) / (h_a * h_a);
    EXPECT_NEAR(find_entry("a", "a").value, bump_aa, tol(bump_aa));

    double bump_bb = (npv(a, b + h_b, sigma, r0) - 2.0 * npv(a, b, sigma, r0) + npv(a, b - h_b, sigma, r0)) / (h_b * h_b);
    EXPECT_NEAR(find_entry("b", "b").value, bump_bb, tol(bump_bb));

    double bump_sigmasigma =
        (npv(a, b, sigma + h_sigma, r0) - 2.0 * npv(a, b, sigma, r0) + npv(a, b, sigma - h_sigma, r0)) / (h_sigma * h_sigma);
    EXPECT_NEAR(find_entry("sigma", "sigma").value, bump_sigmasigma, tol(bump_sigmasigma));

    double bump_r0r0 = (npv(a, b, sigma, r0 + h_r0) - 2.0 * npv(a, b, sigma, r0) + npv(a, b, sigma, r0 - h_r0)) / (h_r0 * h_r0);
    EXPECT_NEAR(find_entry("r0", "r0").value, bump_r0r0, tol(bump_r0r0));

    double cross_ab = (npv(a + h_a, b + h_b, sigma, r0) - npv(a + h_a, b - h_b, sigma, r0) - npv(a - h_a, b + h_b, sigma, r0) +
                        npv(a - h_a, b - h_b, sigma, r0)) /
                       (4.0 * h_a * h_b);
    EXPECT_NEAR(find_entry("a", "b").value, cross_ab, tol(cross_ab));

    double cross_asigma = (npv(a + h_a, b, sigma + h_sigma, r0) - npv(a + h_a, b, sigma - h_sigma, r0) -
                            npv(a - h_a, b, sigma + h_sigma, r0) + npv(a - h_a, b, sigma - h_sigma, r0)) /
                           (4.0 * h_a * h_sigma);
    EXPECT_NEAR(find_entry("a", "sigma").value, cross_asigma, tol(cross_asigma));

    double cross_ar0 = (npv(a + h_a, b, sigma, r0 + h_r0) - npv(a + h_a, b, sigma, r0 - h_r0) -
                         npv(a - h_a, b, sigma, r0 + h_r0) + npv(a - h_a, b, sigma, r0 - h_r0)) /
                        (4.0 * h_a * h_r0);
    EXPECT_NEAR(find_entry("a", "r0").value, cross_ar0, tol(cross_ar0));

    double cross_bsigma = (npv(a, b + h_b, sigma + h_sigma, r0) - npv(a, b + h_b, sigma - h_sigma, r0) -
                            npv(a, b - h_b, sigma + h_sigma, r0) + npv(a, b - h_b, sigma - h_sigma, r0)) /
                           (4.0 * h_b * h_sigma);
    EXPECT_NEAR(find_entry("b", "sigma").value, cross_bsigma, tol(cross_bsigma));

    double cross_br0 = (npv(a, b + h_b, sigma, r0 + h_r0) - npv(a, b + h_b, sigma, r0 - h_r0) -
                         npv(a, b - h_b, sigma, r0 + h_r0) + npv(a, b - h_b, sigma, r0 - h_r0)) /
                        (4.0 * h_b * h_r0);
    EXPECT_NEAR(find_entry("b", "r0").value, cross_br0, tol(cross_br0));

    double cross_sigmar0 = (npv(a, b, sigma + h_sigma, r0 + h_r0) - npv(a, b, sigma + h_sigma, r0 - h_r0) -
                             npv(a, b, sigma - h_sigma, r0 + h_r0) + npv(a, b, sigma - h_sigma, r0 - h_r0)) /
                            (4.0 * h_sigma * h_r0);
    EXPECT_NEAR(find_entry("sigma", "r0").value, cross_sigmar0, tol(cross_sigmar0));
}

TEST(GreeksHessianHullWhiteTest, ComputeHessianWithASubsetOfFactorsReturnsOnlyThePairsFormableFromThem) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "a", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "b", std::nullopt},
    };

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution(),
        factors
    );

    ASSERT_TRUE(report.skipped.empty()) << (report.skipped.empty() ? "" : report.skipped.front());
    ASSERT_EQ(report.entries.size(), 3u); // (a,a), (b,b), (a,b) -- ni sigma ni r0 se pidieron
    for (const auto& entry : report.entries) {
        EXPECT_TRUE(entry.factor_i.name == "a" || entry.factor_i.name == "b");
        EXPECT_TRUE(entry.factor_j.name == "a" || entry.factor_j.name == "b");
    }
}

TEST(GreeksHessianHullWhiteTest, GradientFromTheHessianStructMatchesTheExistingAadReverseGreekResult) {
    // Confirma que el gradiente que expone `HullWhite1FHessian` (calculado con Dual2, camino
    // ENTERAMENTE distinto al AAD reverse-mode de Burn que ya usa `try_aad_reverse`/"Greek" orden
    // 1) referencia el MISMO modelo/producto -- la verificacion fuerte valor/gradiente vs Burn ya
    // esta hecha en Rust (hull_white_dual::tests::value_and_gradient_match_the_burn_autodiff_route);
    // esto solo confirma que el wiring C++ (accessors de HullWhite1FModel/IrSwapProduct) no
    // introdujo ninguna discrepancia adicional.
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    engine::HullWhite1FHessian h = engine::hull_white_1f_hessian(
        model.a(), model.b(), model.sigma(), model.r0(), swap.notional(), swap.fixed_rate(), swap.use_par_rate(),
        swap.start(), swap.payment_times(), swap.accruals()
    );

    auto aad_greek = [&](const std::string& name) {
        GreekRequest request;
        request.metric_name = "HullWhiteModelNpv";
        request.risk_factor = RiskFactor{RiskFactorKind::ModelParameter, "model", name, std::nullopt};
        request.order = GreekOrder{1, std::nullopt};
        request.method = GreekMethod::Auto;
        return engine::greeks::compute_greek(
                   registries, request, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution()
        )
            .value;
    };

    auto rel_tol = [](double reference) { return 1e-6 * std::max(std::abs(reference), 1.0); };
    EXPECT_NEAR(h.d_a, aad_greek("a"), rel_tol(aad_greek("a")));
    EXPECT_NEAR(h.d_b, aad_greek("b"), rel_tol(aad_greek("b")));
    EXPECT_NEAR(h.d_sigma, aad_greek("sigma"), rel_tol(aad_greek("sigma")));
    EXPECT_NEAR(h.d_r0, aad_greek("r0"), rel_tol(aad_greek("r0")));
}

TEST(GreeksHessianHullWhite2FTest, ComputeHessianWithEmptyFactorsReturnsAllFifteenEntriesViaForwardOverForward) {
    // PLAN_BACKWARD.md §9 Fase 3: HullWhite2F+"HullWhiteModelNpv" YA esta en hessian_capabilities()
    // (generalizacion de try_hessian_forward_over_forward) -- 15 entradas (5 diagonales + 10
    // cruzadas), todas AadForwardOverForward, NINGUNA con "rho" (no es diferenciable). Verificado
    // contra un estencil de bump-and-reval: la diagonal completa (5 entradas) + 3 pares cruzados
    // representativos (no hace falta repetir las 15, ya cubiertas en Rust
    // hull_white_dual::tests::hessian_*_matches_a_*_bump_and_reval_stencil_2f).
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite2FModel model = make_hull_white_2f();

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    ASSERT_TRUE(report.skipped.empty()) << (report.skipped.empty() ? "" : report.skipped.front());
    ASSERT_EQ(report.entries.size(), 15u);
    for (const auto& entry : report.entries) {
        EXPECT_EQ(entry.method_used, engine::greeks::GreekMethod::AadForwardOverForward);
        EXPECT_EQ(entry.measure, pf::ProbabilityMeasure::DeterministicScenario);
        EXPECT_FALSE(entry.std_error.has_value());
        EXPECT_NE(entry.factor_i.name, "rho");
        EXPECT_NE(entry.factor_j.name, "rho");
    }

    auto find_entry = [&](const std::string& name_i, const std::string& name_j) -> const engine::greeks::HessianEntry& {
        for (const auto& entry : report.entries) {
            if ((entry.factor_i.name == name_i && entry.factor_j.name == name_j) ||
                (entry.factor_i.name == name_j && entry.factor_j.name == name_i)) {
                return entry;
            }
        }
        throw std::runtime_error("entrada no encontrada: " + name_i + "/" + name_j);
    };

    const double a = model.a(), b = model.b(), sigma = model.sigma(), eta = model.eta(), rho = model.rho(), r0 = model.r0();
    const double notional = swap.notional(), fixed_rate = swap.fixed_rate(), start = swap.start();
    const std::vector<double>& payment_times = swap.payment_times();
    const std::vector<double>& accruals = swap.accruals();

    auto npv = [&](double a_, double b_, double sigma_, double eta_, double r0_) {
        return engine::irs_hull_white_2f_npv(
            a_, b_, sigma_, eta_, rho, r0_, notional, fixed_rate, false, start, payment_times, accruals
        );
    };

    const double h_a = 1e-4 * std::max(std::abs(a), 1.0);
    const double h_b = 1e-4 * std::max(std::abs(b), 1.0);
    const double h_sigma = 1e-4 * std::max(std::abs(sigma), 1.0);
    const double h_eta = 1e-4 * std::max(std::abs(eta), 1.0);
    const double h_r0 = 1e-4 * std::max(std::abs(r0), 1.0);
    auto tol = [](double reference) { return 1e-3 * std::max(std::abs(reference), 1.0); };

    double base = npv(a, b, sigma, eta, r0);

    double bump_aa = (npv(a + h_a, b, sigma, eta, r0) - 2.0 * base + npv(a - h_a, b, sigma, eta, r0)) / (h_a * h_a);
    EXPECT_NEAR(find_entry("a", "a").value, bump_aa, tol(bump_aa));

    double bump_bb = (npv(a, b + h_b, sigma, eta, r0) - 2.0 * base + npv(a, b - h_b, sigma, eta, r0)) / (h_b * h_b);
    EXPECT_NEAR(find_entry("b", "b").value, bump_bb, tol(bump_bb));

    double bump_sigmasigma =
        (npv(a, b, sigma + h_sigma, eta, r0) - 2.0 * base + npv(a, b, sigma - h_sigma, eta, r0)) / (h_sigma * h_sigma);
    EXPECT_NEAR(find_entry("sigma", "sigma").value, bump_sigmasigma, tol(bump_sigmasigma));

    double bump_etaeta = (npv(a, b, sigma, eta + h_eta, r0) - 2.0 * base + npv(a, b, sigma, eta - h_eta, r0)) / (h_eta * h_eta);
    EXPECT_NEAR(find_entry("eta", "eta").value, bump_etaeta, tol(bump_etaeta));

    double bump_r0r0 = (npv(a, b, sigma, eta, r0 + h_r0) - 2.0 * base + npv(a, b, sigma, eta, r0 - h_r0)) / (h_r0 * h_r0);
    EXPECT_NEAR(find_entry("r0", "r0").value, bump_r0r0, tol(bump_r0r0));

    double cross_ab = (npv(a + h_a, b + h_b, sigma, eta, r0) - npv(a + h_a, b - h_b, sigma, eta, r0) -
                        npv(a - h_a, b + h_b, sigma, eta, r0) + npv(a - h_a, b - h_b, sigma, eta, r0)) /
                       (4.0 * h_a * h_b);
    EXPECT_NEAR(find_entry("a", "b").value, cross_ab, tol(cross_ab));

    double cross_sigmaeta = (npv(a, b, sigma + h_sigma, eta + h_eta, r0) - npv(a, b, sigma + h_sigma, eta - h_eta, r0) -
                              npv(a, b, sigma - h_sigma, eta + h_eta, r0) + npv(a, b, sigma - h_sigma, eta - h_eta, r0)) /
                             (4.0 * h_sigma * h_eta);
    EXPECT_NEAR(find_entry("sigma", "eta").value, cross_sigmaeta, tol(cross_sigmaeta));

    double cross_ar0 = (npv(a + h_a, b, sigma, eta, r0 + h_r0) - npv(a + h_a, b, sigma, eta, r0 - h_r0) -
                         npv(a - h_a, b, sigma, eta, r0 + h_r0) + npv(a - h_a, b, sigma, eta, r0 - h_r0)) /
                        (4.0 * h_a * h_r0);
    EXPECT_NEAR(find_entry("a", "r0").value, cross_ar0, tol(cross_ar0));
}

TEST(GreeksHessianHullWhite2FTest, RequestingRhoExplicitlyGoesToSkippedBecauseItIsNotDifferentiable) {
    // Confirma que el filtrado GENERICO ya existente en compute_hessian (distinct_factors_in/
    // is_valid, sin tocar) maneja "rho" solo: try_hessian_forward_over_forward nunca emite una
    // HessianEntry con "rho", asi que "rho" nunca aparece en valid_factors y cualquier peticion
    // explicita de ese factor cae a skipped -- sin necesitar ningun caso especial nuevo.
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite2FModel model = make_hull_white_2f();

    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "a", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "rho", std::nullopt},
    };

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution(),
        factors
    );

    ASSERT_EQ(report.entries.size(), 1u); // solo (a,a) -- "rho" no forma ningun par valido
    EXPECT_EQ(report.entries.front().factor_i.name, "a");
    EXPECT_EQ(report.entries.front().factor_j.name, "a");
    ASSERT_FALSE(report.skipped.empty());
    EXPECT_NE(report.skipped.front().find("rho"), std::string::npos) << report.skipped.front();
}

TEST(GreeksHessianHullWhiteTest, ComputeHessianOnAnUnsupportedMetricForHullWhite1FGoesToSkippedWithoutThrowing) {
    // Mismo criterio que el equivalente de Fase 1 (GreeksHessianTest.
    // ComputeHessianOnAnUnsupportedModelMetricCombinationGoesToSkippedWithoutThrowing), aqui con
    // el modelo correcto (HullWhite1F) pero una metrica fuera de tabla ("PV" no esta en
    // hessian_capabilities() -- solo "HullWhiteModelNpv" lo esta).
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "PV", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    EXPECT_TRUE(report.entries.empty());
    ASSERT_FALSE(report.skipped.empty());
    EXPECT_NE(report.skipped.front().find("PV"), std::string::npos) << report.skipped.front();
}

// PLAN_BACKWARD.md §7/§9 Fase 3: compute_hvp se apoya en compute_hessian YA EXISTENTE (sin
// dispatch nuevo por modelo) -- estos tests lo ejercitan sobre Hull-White (AadForwardOverForward)
// y, opcionalmente, sobre GBM/payoff (LikelihoodRatioHessian) para demostrar la genericidad.

TEST(GreeksHvpTest, MatchesTheFirstColumnOfTheHessianForABaseDirectionVectorOnHullWhite1F) {
    // direction = vector base (1,0,0,0) -> Hv debe coincidir EXACTAMENTE (es una multiplicacion de
    // numeros ya calculados, no una nueva estimacion) con la primera columna del Hessiano,
    // calculada a mano en el test desde un compute_hessian POR SEPARADO (oraculo independiente,
    // nunca reutilizando codigo interno de compute_hvp).
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    const std::vector<std::string> names = {"a", "b", "sigma", "r0"};
    std::vector<RiskFactor> factors;
    for (const std::string& name : names) {
        factors.push_back(RiskFactor{RiskFactorKind::ModelParameter, "model", name, std::nullopt});
    }
    const std::vector<double> direction = {1.0, 0.0, 0.0, 0.0};

    engine::greeks::HvpReport hvp = engine::greeks::compute_hvp(
        registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution(),
        factors, direction
    );
    ASSERT_TRUE(hvp.skipped.empty()) << (hvp.skipped.empty() ? "" : hvp.skipped.front());
    ASSERT_EQ(hvp.components.size(), factors.size());

    engine::greeks::HessianReport hessian = engine::greeks::compute_hessian(
        registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution(),
        factors
    );
    ASSERT_TRUE(hessian.skipped.empty()) << (hessian.skipped.empty() ? "" : hessian.skipped.front());

    auto find_h = [&](const std::string& name_i, const std::string& name_j) -> double {
        for (const auto& entry : hessian.entries) {
            if ((entry.factor_i.name == name_i && entry.factor_j.name == name_j) ||
                (entry.factor_i.name == name_j && entry.factor_j.name == name_i)) {
                return entry.value;
            }
        }
        throw std::runtime_error("entrada H no encontrada: " + name_i + "/" + name_j);
    };

    for (std::size_t i = 0; i < names.size(); ++i) {
        double expected = 0.0;
        for (std::size_t j = 0; j < names.size(); ++j) {
            expected += find_h(names[i], names[j]) * direction[j];
        }
        EXPECT_EQ(hvp.components[i].factor.name, names[i]);
        EXPECT_EQ(hvp.components[i].method_used, engine::greeks::GreekMethod::AadForwardOverForward);
        EXPECT_NEAR(hvp.components[i].value, expected, 1e-9 * std::max(1.0, std::abs(expected)))
            << "factor=" << names[i];
    }
}

TEST(GreeksHvpTest, MismatchedDirectionAndFactorsSizeThrowsInvalidArgument) {
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap = make_irs(1'000'000.0, 0.02);
    engine::HullWhite1FModel model = make_hull_white();

    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "a", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "b", std::nullopt},
    };
    std::vector<double> direction = {1.0}; // tamano distinto a factors -- error estructural del llamante

    EXPECT_THROW(
        engine::greeks::compute_hvp(
            registries, "HullWhiteModelNpv", Params{}, model, swap, flat_market(), pricing_context(1'000, 7),
            cpu_execution(), factors, direction
        ),
        std::invalid_argument
    );
}

TEST(GreeksHvpTest, WorksGenericallyOverLikelihoodRatioHessianForGbmPayoff) {
    // Opcional (no obligatorio, PLAN_BACKWARD.md §9 Fase 3): demuestra que compute_hvp es
    // GENERICO -- funciona igual de bien sobre el Hessiano de likelihood ratio del motor de payoff
    // (GBM) que sobre el forward-over-forward de Hull-White, sin ningun codigo especifico de
    // modelo dentro de compute_hvp (se apoya solo en compute_hessian).
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt},
    };
    const std::vector<double> direction = {1.0, 1.0};

    engine::greeks::HvpReport hvp = engine::greeks::compute_hvp(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution(),
        factors, direction
    );
    ASSERT_TRUE(hvp.skipped.empty()) << (hvp.skipped.empty() ? "" : hvp.skipped.front());
    ASSERT_EQ(hvp.components.size(), 2u);

    // Mismo seed/n_paths que la llamada interna de compute_hvp -> coincidencia bit a bit (mismo
    // criterio ya usado en GreeksHessianTest.ComputeHessianWithEmptyFactorsReturnsGammaVolgaVannaAllViaLikelihoodRatio).
    engine::greeks::HessianReport hessian = engine::greeks::compute_hessian(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(500'000, 7), cpu_execution(),
        factors
    );
    auto find_h = [&](const std::string& name_i, const std::string& name_j) -> double {
        for (const auto& entry : hessian.entries) {
            if ((entry.factor_i.name == name_i && entry.factor_j.name == name_j) ||
                (entry.factor_i.name == name_j && entry.factor_j.name == name_i)) {
                return entry.value;
            }
        }
        throw std::runtime_error("entrada H no encontrada: " + name_i + "/" + name_j);
    };

    double expected_spot = find_h("spot", "spot") * direction[0] + find_h("spot", "volatility") * direction[1];
    double expected_vol = find_h("volatility", "spot") * direction[0] + find_h("volatility", "volatility") * direction[1];
    EXPECT_NEAR(hvp.components[0].value, expected_spot, 1e-9 * std::max(1.0, std::abs(expected_spot)));
    EXPECT_NEAR(hvp.components[1].value, expected_vol, 1e-9 * std::max(1.0, std::abs(expected_vol)));
}

// PLAN_IMPROVE_NOTEBOOK2.md Fase 0 (ADR-IN2-01): `try_pathwise`/`try_pathwise2`/`try_pathwise_cross`
// (y `try_hessian_likelihood_ratio` via `compute_hessian`) nunca recibian `pricing.pricing_date()`
// -- cualquier Greek pedida sobre GBM/"PayoffPriceQ" con `pricing_date() != 0` devolvia
// SILENCIOSAMENTE el mismo valor que a `pricing_date=0` (se descubrio porque charm, una diferencia
// finita de delta entre dos `pricing_date`, salia exactamente 0.0 en
// `09_option_strategies_and_greeks.ipynb`). Decision tomada (opcion 2 de PLAN_IMPROVE_NOTEBOOK2.md
// §2 Fase 0, ver el ADR en ese documento): rechazar la especializacion pathwise/likelihood-ratio
// cuando `pricing_date() != 0` -- `method=Auto` cae al estencil generico de bump-and-reval (que SI
// reconstruye el `MeasureResult` con `pricing_date` desplazado, via `metric->evaluate` ->
// `PayoffPriceQMeasure::evaluate`), `method=Pathwise` explicito lanza un error claro en vez de
// aproximar en silencio.
TEST(GreeksImproveNotebook2Fase0Test, AutoDeltaOfPayoffPriceQFallsBackToBumpAndRevalAndMatchesBlackScholesUnderNonZeroPricingDate) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    const double bump = 1.0; // default_model_parameter_bump(100.0) = max(1e-2*100, 1e-4)

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    PricingContext pricing_at_zero = pricing_context(500'000, 7);
    PricingContext pricing_shifted(
        Params{{"pricing_date", 0.4}, {"n_paths", 500'000.0}, {"n_steps", 1.0}, {"seed", 7.0}}
    );

    engine::greeks::GreekResult at_zero = engine::greeks::compute_greek(
        registries, payoff_price_q_request("spot"), model, product, flat_market(), pricing_at_zero, cpu_execution()
    );
    ASSERT_EQ(at_zero.method_used, GreekMethod::Pathwise) << "baseline: sigue resolviendo pathwise a pricing_date=0";

    engine::greeks::GreekResult shifted = engine::greeks::compute_greek(
        registries, payoff_price_q_request("spot"), model, product, flat_market(), pricing_shifted, cpu_execution()
    );
    EXPECT_EQ(shifted.method_used, GreekMethod::BumpAndReval)
        << "pricing_date != 0 rechaza la especializacion pathwise -- method=Auto cae al estencil generico";
    ASSERT_TRUE(shifted.bump_used.has_value());
    EXPECT_DOUBLE_EQ(*shifted.bump_used, bump);

    const double remaining_maturity = maturity - 0.4;
    double analytic_delta_shifted =
        (black_scholes_call(s0 + bump, strike, r, q, sigma, remaining_maturity) -
         black_scholes_call(s0 - bump, strike, r, q, sigma, remaining_maturity)) /
        (2.0 * bump);

    EXPECT_NEAR(shifted.value, analytic_delta_shifted, 0.02)
        << "Greek=" << shifted.value << " analytic=" << analytic_delta_shifted;
    // El bug real que motiva esta fase: antes de la correccion, `shifted.value` habria salido
    // identico (dentro del ruido MC) a `at_zero.value`, ignorando el desplazamiento por completo --
    // aqui deberian diferir de forma economicamente significativa (remaining_maturity 0.6 vs 1.0).
    EXPECT_GT(std::abs(shifted.value - at_zero.value), 0.02) << "shifted=" << shifted.value << " at_zero=" << at_zero.value;
}

TEST(GreeksImproveNotebook2Fase0Test, ExplicitPathwiseMethodRejectsANonZeroPricingDateWithAnExplicitError) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.method = GreekMethod::Pathwise;

    PricingContext pricing_shifted(
        Params{{"pricing_date", 0.4}, {"n_paths", 200'000.0}, {"n_steps", 1.0}, {"seed", 7.0}}
    );

    try {
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_shifted, cpu_execution());
        FAIL() << "se esperaba std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("pricing_date"), std::string::npos) << e.what();
    }
}

TEST(GreeksImproveNotebook2Fase0Test, AutoGammaFallsBackToTheGenericBumpAndRevalStencilUnderNonZeroPricingDate) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{2, std::nullopt};

    PricingContext pricing_shifted(
        Params{{"pricing_date", 0.4}, {"n_paths", 500'000.0}, {"n_steps", 1.0}, {"seed", 7.0}}
    );

    engine::greeks::GreekResult shifted = engine::greeks::compute_greek(
        registries, request, model, product, flat_market(), pricing_shifted, cpu_execution()
    );

    EXPECT_EQ(shifted.method_used, GreekMethod::BumpAndReval)
        << "Gamma via likelihood ratio tambien se rechaza bajo pricing_date != 0 (mismo motivo que Delta)";
    double analytic_gamma = black_scholes_gamma(s0, strike, r, q, sigma, maturity - 0.4);
    EXPECT_NEAR(shifted.value, analytic_gamma, 0.02) << "Greek=" << shifted.value << " analytic=" << analytic_gamma;
}

TEST(GreeksImproveNotebook2Fase0Test, ComputeHessianSkipsWithAnExplicitPricingDateReasonInsteadOfAWrongLikelihoodRatioValue) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    PricingContext pricing_shifted(
        Params{{"pricing_date", 0.4}, {"n_paths", 200'000.0}, {"n_steps", 1.0}, {"seed", 7.0}}
    );

    engine::greeks::HessianReport report = engine::greeks::compute_hessian(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_shifted, cpu_execution()
    );

    EXPECT_TRUE(report.entries.empty());
    ASSERT_EQ(report.skipped.size(), 1u);
    EXPECT_NE(report.skipped.front().find("pricing_date"), std::string::npos) << report.skipped.front();
}
