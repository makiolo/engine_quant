// Cableado de las medidas Monte Carlo de PayoffProduct (bajo Q y bajo P) a Registry<IMeasure>/
// Engine.price (PLAN_PRODUCTS.md §12): hasta este cambio, `risk_neutral_price_gbm`/
// `exercise_price_gbm`/`hit_probability_gbm`/`payoff_exposure_profile_gbm`/`forecast_gbm_p`/
// `pnl_distribution_gbm_p` (ver test_gbm_measures.cpp/test_gbm_measures_p.cpp/
// test_gbm_exercise_measures.cpp) solo eran alcanzables llamandolas directamente desde C++ --
// este archivo confirma que las mismas siete medidas son alcanzables POR NOMBRE a traves de
// `Registry<IMeasure>`/`engine::price(...)`, igual que "PV"/"DV01"/"ExposureProfile" para
// IrSwapProduct, y que rechazan producto/modelo incompatibles con `std::invalid_argument`
// (mismo criterio que el resto de medidas de measure.cpp).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/model.hpp"
#include "engine/payoff/barrier_templates.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/payoff/predicate.hpp"
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

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

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
    return PricingContext(Params{{"pricing_date", 0.0}, {"n_paths", static_cast<double>(n_paths)}, {"n_steps", 1.0}, {"seed", static_cast<double>(seed)}});
}

ExecutionContext cpu_execution() {
    return ExecutionContext(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
}

MarketSnapshot flat_market() { return MarketSnapshot({1.0}, {0.02}); }

pf::ContractPtr european_call(const pf::ObservableId& spot, double strike, double maturity) {
    return pf::when(
        tp(maturity),
        pf::cashflow(pf::Currency{"USD"}, pf::maximum(pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(strike)), pf::constant(0.0)))
    );
}

pf::ContractPtr bermuda_put(const pf::ObservableId& spot, double strike, double maturity, const std::vector<pf::TimePoint>& dates) {
    pf::ScalarExprPtr exercise_value = pf::maximum(pf::sub(pf::constant(strike), pf::current(spot)), pf::constant(0.0));
    pf::ContractPtr continuation = pf::when(
        tp(maturity),
        pf::cashflow(pf::Currency{"USD"}, pf::maximum(pf::sub(pf::constant(strike), pf::fixing(spot, tp(maturity))), pf::constant(0.0)))
    );
    return pf::exercise(pf::EventId{"EX"}, dates, exercise_value, continuation);
}

// Producto ficticio para los tests de rechazo de tipo (mismo patron que FakeProduct en
// test_registry.cpp).
class FakeProduct : public engine::IProduct {
public:
    std::string type_name() const override { return "Fake"; }
};

TEST(PayoffMeasureWiringTest, RegisterBuiltinsRegistersTheSevenNewMeasureNames) {
    Registries registries;
    register_builtins(registries);

    EXPECT_TRUE(registries.measures.contains("PayoffPriceQ"));
    EXPECT_TRUE(registries.measures.contains("PayoffExerciseQ"));
    EXPECT_TRUE(registries.measures.contains("PayoffHitProbabilityQ"));
    EXPECT_TRUE(registries.measures.contains("PayoffExposureProfileQ"));
    EXPECT_TRUE(registries.measures.contains("PayoffForecastP"));
    EXPECT_TRUE(registries.measures.contains("PayoffHitProbabilityP"));
    EXPECT_TRUE(registries.measures.contains("PayoffPnlDistributionP"));
}

TEST(PayoffMeasureWiringTest, PayoffPriceQViaRegistryMatchesDirectCallToRiskNeutralPriceGbm) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);
    auto measure = registries.measures.create("PayoffPriceQ");

    engine::MeasureResult via_registry =
        measure->evaluate(model, product, flat_market(), pricing_context(50'000, 7), cpu_execution());
    pf::QValuationResult direct = pf::risk_neutral_price_gbm(*product.payoff_program(), model, 50'000, 7);

    EXPECT_TRUE(via_registry.has_scalar);
    EXPECT_DOUBLE_EQ(via_registry.scalar, direct.mean);
}

// Confirma el cableado de punta a punta pedido explicitamente: no solo `Registry<IMeasure>`
// directo (test anterior), sino tambien `engine::price(...)` (lo que usan Python/Excel/C ABI).
TEST(PayoffMeasureWiringTest, PayoffPriceQIsReachableThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);

    engine::PriceResult result = engine::price(
        registries, product, {"PayoffPriceQ"}, model, flat_market(), pricing_context(50'000, 7), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].measure_name, "PayoffPriceQ");
    EXPECT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
}

TEST(PayoffMeasureWiringTest, PayoffPriceQRejectsAProductThatIsNotPayoffProduct) {
    Registries registries;
    register_builtins(registries);
    FakeProduct fake;
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.XYZ");
    auto measure = registries.measures.create("PayoffPriceQ");

    EXPECT_THROW(
        measure->evaluate(model, fake, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(PayoffMeasureWiringTest, PayoffPriceQRejectsAModelThatIsNotGbmModel) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::HullWhite1FModel hw = make_hull_white();
    auto measure = registries.measures.create("PayoffPriceQ");

    EXPECT_THROW(
        measure->evaluate(hw, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(PayoffMeasureWiringTest, PayoffExerciseQViaRegistryReportsOneDiagnosticPerDecisionDate) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    std::vector<pf::TimePoint> dates{tp(0.25), tp(0.5), tp(0.75)};

    pf::PayoffProduct product("BERMUDA_PUT", bermuda_put(spot, strike, maturity, dates));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);
    auto measure = registries.measures.create("PayoffExerciseQ");

    engine::MeasureResult result =
        measure->evaluate(model, product, flat_market(), pricing_context(20'000, 41), cpu_execution());

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
    ASSERT_EQ(result.times.size(), dates.size());
    ASSERT_EQ(result.primary.size(), dates.size());
    ASSERT_EQ(result.secondary.size(), dates.size());
    EXPECT_EQ(result.times, (std::vector<double>{0.25, 0.5, 0.75}));
    for (std::size_t i = 0; i < dates.size(); ++i) {
        EXPECT_GE(result.primary[i], 0.0);
        EXPECT_LE(result.primary[i], 1.0);
        EXPECT_GE(result.secondary[i], 0.0);
    }
}

TEST(PayoffMeasureWiringTest, PayoffHitProbabilityQViaRegistryRequiresAnEventParam) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, barrier = 120.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    std::vector<pf::TimePoint> monitoring_times{tp(0.25), tp(0.5), tp(0.75), tp(maturity)};

    pf::ContractPtr vanilla = european_call(spot, 100.0, maturity);
    pf::ContractPtr up_and_in = pf::templates::up_and_in(pf::EventId{"UI"}, spot, barrier, monitoring_times, vanilla);
    pf::PayoffProduct product("UI", up_and_in);
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);
    auto measure = registries.measures.create("PayoffHitProbabilityQ", Params{{"event", std::string("UI")}});

    engine::MeasureResult result =
        measure->evaluate(model, product, flat_market(), pricing_context(20'000, 7), cpu_execution());

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GE(result.scalar, 0.0);
    EXPECT_LE(result.scalar, 1.0);

    // Un evento que no existe en el contrato es un error de EVALUACION (lo detecta Rust), no de
    // preflight -- confirma que llega como excepcion en vez de un resultado silenciosamente
    // incorrecto.
    auto bad_measure = registries.measures.create("PayoffHitProbabilityQ", Params{{"event", std::string("NOT_A_REAL_EVENT")}});
    EXPECT_THROW(
        bad_measure->evaluate(model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::exception
    );
}

TEST(PayoffMeasureWiringTest, PayoffExposureProfileQViaRegistryMatchesEeAndPfeShape) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product("CALL", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);
    auto measure = registries.measures.create(
        "PayoffExposureProfileQ", Params{{"exposure_times", std::vector<double>{0.0, maturity}}}
    );

    engine::MeasureResult result =
        measure->evaluate(model, product, flat_market(), pricing_context(50'000, 7), cpu_execution());

    EXPECT_FALSE(result.has_scalar);
    ASSERT_EQ(result.times.size(), 2u);
    ASSERT_EQ(result.primary.size(), 2u);
    ASSERT_EQ(result.secondary.size(), 2u);
    for (std::size_t i = 0; i < result.times.size(); ++i) {
        EXPECT_GE(result.primary[i], 0.0);
        EXPECT_GE(result.secondary[i], result.primary[i]);
    }
}

TEST(PayoffMeasureWiringTest, PayoffForecastPViaRegistryRejectsAGbmQModel) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("SPOT", pf::when(tp(1.0), pf::cashflow(pf::Currency{"USD"}, pf::fixing(spot, tp(1.0)))));
    engine::GbmModel q_model = make_gbm_q(100.0, 0.05, 0.0, 0.2, spot.value);
    auto measure = registries.measures.create("PayoffForecastP");

    // "el motor rechaza combinaciones Q/P invalidas" (PLAN_PRODUCTS.md §12 Fase 7): un GbmModel
    // (solo declara RiskNeutralQ) pedido con la medida P debe fallar, no degradar en silencio.
    EXPECT_THROW(
        measure->evaluate(q_model, product, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(PayoffMeasureWiringTest, PayoffForecastPViaRegistryMatchesTheAnalyticExpectedTerminalSpot) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, mu = 0.30, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product(
        "TERMINAL_SPOT", pf::when(tp(maturity), pf::cashflow(pf::Currency{"USD"}, pf::fixing(spot, tp(maturity))))
    );
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);
    auto measure = registries.measures.create("PayoffForecastP");

    engine::MeasureResult result =
        measure->evaluate(model, product, flat_market(), pricing_context(200'000, 7), cpu_execution());

    double analytic = s0 * std::exp(mu * maturity);
    EXPECT_TRUE(result.has_scalar);
    EXPECT_NEAR(result.scalar, analytic, 0.05 * analytic);
}

TEST(PayoffMeasureWiringTest, PayoffHitProbabilityPViaRegistryRequiresAnEventParam) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, barrier = 120.0, mu = 0.05, sigma = 0.2, maturity = 1.0;
    std::vector<pf::TimePoint> monitoring_times{tp(0.25), tp(0.5), tp(0.75), tp(maturity)};

    pf::ContractPtr vanilla = european_call(spot, 100.0, maturity);
    pf::ContractPtr up_and_in = pf::templates::up_and_in(pf::EventId{"UI"}, spot, barrier, monitoring_times, vanilla);
    pf::PayoffProduct product("UI", up_and_in);
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);
    auto measure = registries.measures.create("PayoffHitProbabilityP", Params{{"event", std::string("UI")}});

    engine::MeasureResult result =
        measure->evaluate(model, product, flat_market(), pricing_context(20'000, 7), cpu_execution());

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GE(result.scalar, 0.0);
    EXPECT_LE(result.scalar, 1.0);
}

TEST(PayoffMeasureWiringTest, PayoffPnlDistributionPViaRegistryReportsEsAtLeastAsSevereAsVar) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, mu = 0.05, sigma = 0.2, maturity = 1.0;

    pf::PayoffProduct product(
        "LONG_SPOT",
        pf::when(tp(maturity), pf::cashflow(pf::Currency{"USD"}, pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(s0))))
    );
    engine::GbmPModel model = make_gbm_p(s0, mu, sigma, spot.value);
    auto measure = registries.measures.create("PayoffPnlDistributionP", Params{{"confidence", 0.95}});

    engine::MeasureResult result =
        measure->evaluate(model, product, flat_market(), pricing_context(50'000, 11), cpu_execution());

    EXPECT_TRUE(result.has_scalar);
    ASSERT_EQ(result.primary.size(), 1u);
    ASSERT_EQ(result.secondary.size(), 1u);
    double var = result.primary[0];
    double es = result.secondary[0];
    EXPECT_GE(es, var);
}

} // namespace
