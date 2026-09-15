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
#include <stdexcept>
#include <string>
#include <vector>

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
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreekResult result = engine::greeks::compute_greek(
        registries, payoff_price_q_request("spot"), model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    EXPECT_EQ(result.method_used, GreekMethod::BumpAndReval);
    ASSERT_TRUE(result.bump_used.has_value());
    EXPECT_DOUBLE_EQ(*result.bump_used, 1.0); // max(1e-2*|100|, 1e-4) = 1.0, PLAN_GREEKS.md §4.3
    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::RiskNeutralQ); // "PayoffPriceQ" -> sufijo Q
    EXPECT_EQ(result.risk_factor.name, "spot");
    EXPECT_EQ(result.order.order, 1);
}

// Actualizado en Fase 3 (PLAN_GREEKS.md §8.5 punto 2): "curve.parallel" ahora se enumera SIEMPRE,
// además de los 4 parámetros de modelo -- 5 candidatos en vez de 4, ninguno en skipped.
TEST(GreeksFase1Test, ComputeAllGreeksOnGbmReturnsTheFourModelParametersPlusCurveParallel) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreeksReport report = engine::greeks::compute_all_greeks(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(100'000, 7), cpu_execution()
    );

    EXPECT_TRUE(report.skipped.empty());
    ASSERT_EQ(report.greeks.size(), 5u);
    std::vector<std::string> factors;
    for (const auto& greek : report.greeks) factors.push_back(engine::greeks::to_string(greek.risk_factor));
    std::sort(factors.begin(), factors.end());
    EXPECT_EQ(
        factors,
        (std::vector<std::string>{"curve.parallel", "model.dividend_yield", "model.rate", "model.spot", "model.volatility"})
    );

    // PayoffPriceQMeasure ignora `market` (measure.cpp) -- ambas evaluaciones +-h usan
    // exactamente el mismo modelo/paths, así que la derivada respecto de la curva es 0 exacto,
    // no una aproximación de Monte Carlo.
    for (const auto& greek : report.greeks) {
        if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::CurveParallel) {
            EXPECT_DOUBLE_EQ(greek.value, 0.0);
        }
    }
}

// Actualizado en Fase 3: además de los 4 parámetros de Hull-White (derivada nula, sin cambios --
// PresentValueMeasure::evaluate para IrSwapProduct no usa el modelo en absoluto), "curve.parallel"
// se enumera SIEMPRE y su valor SÍ depende de la curva (PLAN_GREEKS.md §8.5: "una derivada nula es
// una respuesta valida, distinta de 'no aplica'" -- aquí, al revés, una derivada no nula es la
// respuesta correcta porque PV de un IRS es, por construcción, función de la curva de descuento).
TEST(GreeksFase1Test, ComputeAllGreeksOnIrsPvIsZeroForHullWhiteButNonZeroForCurveParallel) {
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

    EXPECT_TRUE(report.skipped.empty());
    ASSERT_EQ(report.greeks.size(), 5u);

    auto pv = registries.measures.create("PV", Params{});
    const double h = 0.0001; // default de curva (PLAN_GREEKS.md §4.3)
    MarketSnapshot market_up = engine::bump_market_parallel(market, h);
    MarketSnapshot market_down = engine::bump_market_parallel(market, -h);
    double manual_curve_parallel = (pv->evaluate(model, swap, market_up, pricing_context(1'000, 7), cpu_execution()).scalar -
                                     pv->evaluate(model, swap, market_down, pricing_context(1'000, 7), cpu_execution()).scalar) /
                                    (2.0 * h);

    std::vector<std::string> model_param_names;
    bool saw_curve_parallel = false;
    for (const auto& greek : report.greeks) {
        if (greek.risk_factor.kind == engine::greeks::RiskFactorKind::ModelParameter) {
            model_param_names.push_back(greek.risk_factor.name);
            EXPECT_DOUBLE_EQ(greek.value, 0.0);
        } else {
            ASSERT_EQ(greek.risk_factor.kind, engine::greeks::RiskFactorKind::CurveParallel);
            saw_curve_parallel = true;
            EXPECT_NEAR(greek.value, manual_curve_parallel, 1e-6);
        }
    }
    EXPECT_TRUE(saw_curve_parallel);
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

TEST(GreeksFase1Test, ComputeGreekRejectsSecondOrder) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.order = GreekOrder{2, std::nullopt};

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(GreeksFase1Test, ComputeGreekRejectsPathwiseMethodExplicitly) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.method = GreekMethod::Pathwise;

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

// Actualizado en Fase 3: "curve.parallel"/"curve.pillar:<i>" ya son soportados (ver
// GreeksFase3Test más abajo) -- este test se mueve a los dos RiskFactorKind que siguen
// pendientes de las Fases 4-5 ("credit.*"/"time.theta").
TEST(GreeksFase1Test, ComputeGreekRejectsCreditAndTimeRiskFactorKindsStillPending) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    for (const std::string& factor : {"credit.hazard_rate", "time.theta"}) {
        GreekRequest request = payoff_price_q_request("spot");
        request.risk_factor = engine::greeks::parse_risk_factor(factor);

        EXPECT_THROW(
            engine::greeks::compute_greek(
                registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()
            ),
            std::invalid_argument
        ) << factor;
    }
}

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
    EXPECT_TRUE(without_buckets.skipped.empty());
    ASSERT_EQ(without_buckets.greeks.size(), 5u); // 4 params HW1F + curve.parallel

    engine::greeks::GreeksReport with_buckets = engine::greeks::compute_all_greeks(
        registries, "PV", Params{}, model, swap, market, pricing_context(1'000, 7), cpu_execution(),
        /*include_curve_buckets=*/true
    );
    EXPECT_TRUE(with_buckets.skipped.empty());
    ASSERT_EQ(with_buckets.greeks.size(), 8u); // + curve.pillar:0/1/2

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
