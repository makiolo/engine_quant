// PayoffUnilateralCvaQMeasure (PLAN_IMPROVE_NOTEBOOK.md Fase 5): CVA unilateral nativo para un
// PayoffProduct con opcionalidad real bajo GbmModel -- hasta esta medida, `06_exposure_cva_
// portfolio.ipynb` §2 integraba el CVA a mano en Python desde `PayoffExposureProfileQ`
// (`manual_cva`), duplicando la formula que `UnilateralCvaMeasure` ya usa en C++ para IRS. Este
// archivo confirma:
//   1. que "PayoffUnilateralCvaQ" queda registrado en Registry<IMeasure> (mismo patron que
//      test_registry_wiring_payoff_measures.cpp para el resto de medidas "Payoff*Q"),
//   2. que rechaza un producto que no es PayoffProduct,
//   3. sobre todo, un test de PARIDAD NUMERICA (regresion): el resultado de
//      PayoffUnilateralCvaQMeasure coincide EXACTAMENTE (no solo dentro de ruido Monte Carlo)
//      con integrar a mano, en este mismo test, la formula "(1-R) * sum_i EE_i * DeltaPD_i *
//      DF_i" sobre el perfil EE que devuelve PayoffExposureProfileQMeasure -- si algun dia se
//      rompe la composicion (measure.cpp::PayoffUnilateralCvaQMeasure::evaluate /
//      compute_cva_from_exposure_market), este test lo detecta.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/model.hpp"
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

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

engine::GbmModel make_gbm_q(double s0, double r, double q, double sigma, const std::string& observable) {
    return engine::GbmModel(Params{{"s0", s0}, {"r", r}, {"q", q}, {"sigma", sigma}, {"observable", observable}});
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

// Curva con pendiente real (no plana) para ejercitar MarketSnapshot::discount_factor de verdad
// -- si la medida descontara con un DF constante por error, este test lo detectaria.
MarketSnapshot credit_market(double hazard_rate, double recovery_rate) {
    return MarketSnapshot({0.5, 1.0}, {0.03, 0.05}, hazard_rate, recovery_rate);
}

pf::ContractPtr european_call(const pf::ObservableId& spot, double strike, double maturity) {
    return pf::when(
        tp(maturity),
        pf::cashflow(pf::Currency{"USD"}, pf::maximum(pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(strike)), pf::constant(0.0)))
    );
}

class FakeProduct : public engine::IProduct {
public:
    std::string type_name() const override { return "Fake"; }
};

TEST(PayoffUnilateralCvaQMeasureTest, RegisterBuiltinsRegistersTheName) {
    Registries registries;
    register_builtins(registries);

    EXPECT_TRUE(registries.measures.contains("PayoffUnilateralCvaQ"));
}

TEST(PayoffUnilateralCvaQMeasureTest, RejectsAProductThatIsNotPayoffProduct) {
    Registries registries;
    register_builtins(registries);
    FakeProduct fake;
    engine::GbmModel model = make_gbm_q(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.XYZ");
    auto measure = registries.measures.create("PayoffUnilateralCvaQ", Params{{"exposure_times", std::vector<double>{0.0, 1.0}}});

    EXPECT_THROW(
        measure->evaluate(model, fake, credit_market(0.02, 0.4), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

TEST(PayoffUnilateralCvaQMeasureTest, RejectsAModelThatIsNotGbmModel) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("CALL", european_call(spot, 100.0, 1.0));
    engine::HullWhite1FModel hw = make_hull_white();
    auto measure = registries.measures.create("PayoffUnilateralCvaQ", Params{{"exposure_times", std::vector<double>{0.0, 1.0}}});

    EXPECT_THROW(
        measure->evaluate(hw, product, credit_market(0.02, 0.4), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );
}

// Test de regresion (paridad numerica exacta, no dentro de ruido Monte Carlo): calcula el
// perfil EE llamando directamente a PayoffExposureProfileQMeasure con el MISMO pricing (mismo
// n_paths/seed => mismas trayectorias), integra el CVA a mano en este test con la formula
// "(1-R) * sum_i EE_i * (S(t_{i-1})-S(t_i)) * DF(t_i)" y compara con PayoffUnilateralCvaQMeasure
// via EXPECT_DOUBLE_EQ.
TEST(PayoffUnilateralCvaQMeasureTest, MatchesManualIntegrationOfTheSameExposureProfile) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.04, q = 0.0, sigma = 0.25, maturity = 1.0;
    const double hazard_rate = 0.02, recovery_rate = 0.4;
    const std::vector<double> exposure_times{0.0, 0.25, 0.5, 0.75, 1.0};

    pf::PayoffProduct product("OPT_CPTY", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);
    MarketSnapshot market = credit_market(hazard_rate, recovery_rate);
    PricingContext pricing = pricing_context(50'000, 5);
    ExecutionContext execution = cpu_execution();

    // 1) Perfil EE directo, mismo camino que consume PayoffUnilateralCvaQMeasure por dentro.
    auto exposure_measure = registries.measures.create("PayoffExposureProfileQ", Params{{"exposure_times", exposure_times}});
    engine::MeasureResult exposure = exposure_measure->evaluate(model, product, market, pricing, execution);
    ASSERT_EQ(exposure.times.size(), exposure_times.size());

    // 2) Integracion manual EN EL TEST, misma formula que measure.cpp::compute_cva_from_exposure_market.
    double expected_cva = 0.0;
    double prev_survival = 1.0;
    for (std::size_t i = 0; i < exposure.times.size(); ++i) {
        double survival = std::exp(-hazard_rate * exposure.times[i]);
        double default_prob = prev_survival - survival;
        expected_cva += (1.0 - recovery_rate) * exposure.primary[i] * default_prob * market.discount_factor(exposure.times[i]);
        prev_survival = survival;
    }
    ASSERT_GT(expected_cva, 0.0);

    // 3) Medida via registry, mismo pricing/market -- debe coincidir EXACTAMENTE.
    auto cva_measure = registries.measures.create("PayoffUnilateralCvaQ", Params{{"exposure_times", exposure_times}});
    engine::MeasureResult result = cva_measure->evaluate(model, product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_DOUBLE_EQ(result.scalar, expected_cva);
    // El perfil EE que arrastra el resultado (times/primary/secondary) es el mismo que devuelve
    // PayoffExposureProfileQ -- la medida de CVA no lo descarta, lo reexpone junto al escalar.
    EXPECT_EQ(result.times, exposure.times);
    EXPECT_EQ(result.primary, exposure.primary);
    EXPECT_EQ(result.secondary, exposure.secondary);
}

// engine::price(...) es el camino que usan Python/Excel/C ABI (no solo Registry<IMeasure>
// directo) -- confirma el cableado de punta a punta, mismo criterio que
// PayoffMeasureWiringTest.PayoffPriceQIsReachableThroughEnginePrice.
TEST(PayoffUnilateralCvaQMeasureTest, IsReachableThroughEnginePrice) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.04, q = 0.0, sigma = 0.25, maturity = 1.0;

    pf::PayoffProduct product("OPT_CPTY", european_call(spot, strike, maturity));
    engine::GbmModel model = make_gbm_q(s0, r, q, sigma, spot.value);
    MarketSnapshot market = credit_market(0.02, 0.4);

    engine::PriceResult result = engine::price(
        registries, product,
        std::vector<engine::MeasureSpec>{
            {"PayoffUnilateralCvaQ", Params{{"exposure_times", std::vector<double>{0.0, 0.5, 1.0}}}},
        },
        model, market, pricing_context(50'000, 5), cpu_execution()
    );

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].measure_name, "PayoffUnilateralCvaQ");
    EXPECT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
}

} // namespace
