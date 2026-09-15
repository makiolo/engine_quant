// Fase 1 de PLAN_GREEKS.md: `IModel::to_params()` + motor de bump-and-reval genérico, limitado a
// `RiskFactorKind::ModelParameter`/orden 1/`GreekMethod::BumpAndReval`, sobre métricas sin
// parámetros propios ("PV"/"PayoffPriceQ"/"PayoffForecastP"). Criterios de aceptación explícitos
// de la fase: Delta/Vega/Rho de una call vía "Greek" coinciden (dentro de tolerancia MC) con
// `PayoffSensitivityQ` existente y con Black-Scholes cerrado; `bump_used`/`method_used` correctos
// en `GreekResult`; `compute_all_greeks("PayoffPriceQ", ..., Gbm)` devuelve exactamente
// {spot, rate, dividend_yield, volatility}, ninguno en `skipped`.

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
#include "engine/payoff/expression.hpp"
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

TEST(GreeksFase1Test, ComputeAllGreeksOnGbmReturnsExactlyTheFourModelParameters) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    engine::greeks::GreeksReport report = engine::greeks::compute_all_greeks(
        registries, "PayoffPriceQ", Params{}, model, product, flat_market(), pricing_context(100'000, 7), cpu_execution()
    );

    EXPECT_TRUE(report.skipped.empty());
    ASSERT_EQ(report.greeks.size(), 4u);
    std::vector<std::string> names;
    for (const auto& greek : report.greeks) names.push_back(greek.risk_factor.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"dividend_yield", "rate", "spot", "volatility"}));
}

TEST(GreeksFase1Test, ComputeAllGreeksOnIrsPvIsZeroForEveryHullWhiteParameter) {
    // PresentValueMeasure::evaluate para IrSwapProduct replica la curva de MarketSnapshot y no
    // usa el modelo en absoluto (measure.cpp, PLAN_REAPI.md §6 Fase 4) -- una derivada nula es la
    // respuesta correcta aqui, no un fallo (PLAN_GREEKS.md §8.5: "una derivada nula es una
    // respuesta valida, distinta de 'no aplica'").
    Registries registries;
    register_builtins(registries);
    engine::IrSwapProduct swap(Params{
        {"notional", 1'000'000.0},
        {"fixed_rate", 0.02},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0}},
    });
    engine::HullWhite1FModel model = make_hull_white();

    engine::greeks::GreeksReport report = engine::greeks::compute_all_greeks(
        registries, "PV", Params{}, model, swap, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );

    EXPECT_TRUE(report.skipped.empty());
    ASSERT_EQ(report.greeks.size(), 4u);
    std::vector<std::string> names;
    for (const auto& greek : report.greeks) {
        names.push_back(greek.risk_factor.name);
        EXPECT_DOUBLE_EQ(greek.value, 0.0);
    }
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "r0", "sigma"}));
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

TEST(GreeksFase1Test, ComputeGreekRejectsNonModelParameterRiskFactorKinds) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);

    GreekRequest request = payoff_price_q_request("spot");
    request.risk_factor = engine::greeks::parse_risk_factor("curve.parallel");

    EXPECT_THROW(
        engine::greeks::compute_greek(registries, request, model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
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
