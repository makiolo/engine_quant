// Tests de integración del registry/composición de medidas (PLAN.md §5.4, Fase 2; firma de
// IMeasure::evaluate actualizada en PLAN.md §7.15). No pretenden repetir la validación
// numérica fina que ya vive en Rust (PLAN.md §5.6 capa 2, `rust/crates/engine-core/src/
// exposure.rs`) — solo confirman que el wiring C++ (Registry<T> + IModel/IProduct/IMeasure +
// bootstrap) funciona de punta a punta.
//
// UnilateralCvaMatchesGoldenValue fija el valor de referencia exacto que se propaga al resto
// de clientes (Python, Excel) para la capa 4 de test de §5.6 ("equivalencia entre clientes")
// -- ver clients/excel/README.md.

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "engine/bootstrap.hpp"
#include "engine/price.hpp"

namespace {

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

Params hull_white_params() {
    return Params{
        {"a", 0.1},
        {"b", 0.03},
        {"sigma", 0.01},
        {"r0", 0.02},
    };
}

// Hull-White 2 factores (PLAN.md §7.16, G2++): mismos parámetros de referencia de literatura
// que `rust/crates/engine-core/src/models/hull_white_2f.rs::tests::reference_model` -- no
// calibrados a mercado real, solo un punto de referencia razonable.
Params hull_white_2f_params() {
    return Params{
        {"a", 0.1},
        {"b", 0.2},
        {"sigma", 0.01},
        {"eta", 0.012},
        {"rho", -0.7},
        {"r0", 0.03},
    };
}

Params par_irs_5y_params() {
    return Params{
        {"notional", 1'000'000.0},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0, 1.0, 1.0}},
    };
}

// Con fixed_rate explícito (PLAN.md §7.19): price_batch/price_many no soportan use_par_rate, a
// diferencia de price() -- cada trade de un lote debe traer ya su tipo fijo.
Params irs_5y_params(double notional, double fixed_rate) {
    return Params{
        {"notional", notional},
        {"fixed_rate", fixed_rate},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0, 1.0, 1.0}},
    };
}

// Mismo calendario que irs_5y_params, distinto tenor (3 años) -- usado para ejercitar el
// agrupamiento heterogéneo de price_many.
Params irs_3y_params(double notional, double fixed_rate) {
    return Params{
        {"notional", notional},
        {"fixed_rate", fixed_rate},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0}},
    };
}

// Mercado mínimo (1 pillar, curva plana al 2% por extrapolación -- PLAN_REAPI.md §6 Fase 4:
// PV/DV01 SÍ descuentan por esta curva ahora, EE/PFE95 siguen usando solo el modelo). Curva
// deliberadamente simple para no acoplar estos tests a una forma de curva concreta; ver
// ParSwapWithExplicitParRateFromAMultiPillarCurveIsZero para un caso con curva no plana.
MarketSnapshot market_with_credit(double hazard_rate, double recovery_rate) {
    return MarketSnapshot({1.0}, {0.02}, hazard_rate, recovery_rate);
}

// Curva no plana (creciente) que cubre el vencimiento del swap 5y -- a diferencia de
// market_with_credit(), aquí "1 pillar extrapolado" no es representativo (PLAN_REAPI.md §6
// Fase 4, "coste de migración de fixtures").
MarketSnapshot upward_sloping_market() {
    return MarketSnapshot({1.0, 2.0, 3.0, 4.0, 5.0}, {0.018, 0.019, 0.020, 0.0205, 0.021});
}

PricingContext golden_pricing(std::uint64_t n_paths, std::uint64_t seed) {
    return PricingContext(Params{
        {"pricing_date", 0.0},
        {"n_paths", static_cast<double>(n_paths)},
        {"n_steps", 208.0}, // ~1 paso/semana sobre los 4 años hasta la última fecha de reseteo
        {"seed", static_cast<double>(seed)},
    });
}

ExecutionContext cpu_execution() {
    return ExecutionContext(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
}

// Producto ficticio para el test de rechazo de tipos (measure.hpp documenta que evaluate()
// lanza std::invalid_argument si el producto recibido no es el tipo concreto esperado).
class FakeProduct : public engine::IProduct {
public:
    std::string type_name() const override { return "Fake"; }
};

} // namespace

TEST(Registry, RegisterBuiltinsPopulatesAllRegistries) {
    Registries registries;
    register_builtins(registries);

    EXPECT_TRUE(registries.models.contains("HullWhite1F"));
    EXPECT_TRUE(registries.models.contains("HullWhite2F"));
    EXPECT_TRUE(registries.products.contains("IRSwap"));
    EXPECT_TRUE(registries.measures.contains("ExposureProfile"));
    EXPECT_TRUE(registries.measures.contains("UnilateralCVA"));
    EXPECT_TRUE(registries.measures.contains("PV"));
    EXPECT_TRUE(registries.measures.contains("DV01"));
}

TEST(Registry, CreateUnknownModelThrows) {
    Registries registries;
    register_builtins(registries);

    EXPECT_THROW(registries.models.create("NoExiste", {}), std::out_of_range);
}

TEST(Registry, ExposureProfileIsNonNegativeAndPfeDominatesEe) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("ExposureProfile");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    ASSERT_EQ(result.primary.size(), result.secondary.size());
    for (std::size_t i = 0; i < result.primary.size(); ++i) {
        EXPECT_GE(result.primary[i], 0.0);
        EXPECT_GE(result.secondary[i], result.primary[i]);
    }
}

// Mismo caso/semilla que UnilateralCvaMatchesGoldenValue -- valor de referencia que se
// propaga al resto de clientes (Python, Excel), ver clients/excel/README.md.
TEST(Registry, ExposureProfileMatchesGoldenValue) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("ExposureProfile");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    ASSERT_EQ(result.times.size(), 5u); // fechas de reseteo auto-derivadas: 0,1,2,3,4
    const double expected_times[] = {0.0, 1.0, 2.0, 3.0, 4.0};
    const double expected_ee[] = {0.0, 12862.61794, 13673.52975, 11957.81610, 7124.10624};
    const double expected_pfe95[] = {0.0, 51009.92088, 53607.17082, 46152.44562, 27535.55727};
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_NEAR(result.times[i], expected_times[i], 1e-9);
        EXPECT_NEAR(result.primary[i], expected_ee[i], 1.0);
        EXPECT_NEAR(result.secondary[i], expected_pfe95[i], 1.0);
    }
}

TEST(Registry, UnilateralCvaIsPositiveForNonzeroHazardRate) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
}

TEST(Registry, UnilateralCvaIsZeroWhenHazardRateIsZero) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    MarketSnapshot market = market_with_credit(0.0, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_LT(std::abs(result.scalar), 1e-9);
}

// Caso/semilla de referencia que documenta clients/excel/README.md ("Verificación manual",
// PLAN.md §5.6 capa 4): el mismo IRS 5y a la par + Hull-White 1F, evaluado a través de
// ENGINE.PRICE en Python/Excel, debe dar exactamente este número.
TEST(Registry, UnilateralCvaMatchesGoldenValue) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_NEAR(result.scalar, 503.6419407799754, 1e-6);
}

TEST(Registry, PresentValueOfAParSwapIsNearZero) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("PV");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(1, 1); // PV es determinista, n_paths/seed no importan
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_NEAR(result.scalar, 0.0, 1e-6);
}

TEST(Registry, Dv01OfAPayerSwapIsPositive) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("DV01");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(1, 1);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
}

// Interfaz homogénea (PLAN.md §7.16): exactamente los mismos IMeasure (ExposureProfile/
// UnilateralCVA/PV/DV01, ni una línea distinta) evaluados sobre HullWhite2F en vez de
// HullWhite1F -- measure.cpp despacha internamente al modelo correcto, sin que este test (ni
// ENGINE.PRICE en Python/Excel) necesite saber que existen dos implementaciones.
TEST(Registry, ExposureProfile2FIsNonNegativeAndPfeDominatesEe) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("ExposureProfile");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    ASSERT_EQ(result.primary.size(), result.secondary.size());
    ASSERT_EQ(result.primary.size(), 5u); // fechas de reseteo auto-derivadas: 0,1,2,3,4
    for (std::size_t i = 0; i < result.primary.size(); ++i) {
        EXPECT_GE(result.primary[i], 0.0);
        EXPECT_GE(result.secondary[i], result.primary[i]);
    }
}

TEST(Registry, UnilateralCva2FIsPositiveForNonzeroHazardRate) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
}

TEST(Registry, UnilateralCva2FIsZeroWhenHazardRateIsZero) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    MarketSnapshot market = market_with_credit(0.0, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_LT(std::abs(result.scalar), 1e-9);
}

TEST(Registry, PresentValue2FOfAParSwapIsNearZero) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("PV");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(1, 1); // PV es determinista, n_paths/seed no importan
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_NEAR(result.scalar, 0.0, 1e-6);
}

TEST(Registry, Dv01OfAPayerSwap2FIsPositive) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("DV01");

    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(1, 1);
    ExecutionContext execution = cpu_execution();

    engine::MeasureResult result = measure->evaluate(*model, *product, market, pricing, execution);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
}

TEST(Price, ComputesTheFullBatchInOneCallUnderHullWhite2F) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::PriceResult result = engine::price(
        registries, *product, {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"},
        *model, market, pricing, execution
    );

    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0].measure_name, "PV");
    EXPECT_EQ(result[2].measure_name, "ExpectedExposure");
    EXPECT_EQ(result[3].measure_name, "PFE95");
    ASSERT_EQ(result[2].result.primary.size(), 5u); // fechas de reseteo: 0,1,2,3,4
    ASSERT_EQ(result[3].result.primary.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_GE(result[3].result.primary[i], result[2].result.primary[i]); // PFE95 >= EE
    }
    EXPECT_TRUE(result[4].result.has_scalar);
    EXPECT_GT(result[4].result.scalar, 0.0); // UnilateralCVA > 0 con hazard_rate > 0
}

TEST(Registry, MeasureRejectsWrongProductType) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto measure = registries.measures.create("ExposureProfile");

    FakeProduct fake_product;
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();

    EXPECT_THROW(measure->evaluate(*model, fake_product, market, pricing, execution), std::invalid_argument);
}

TEST(Price, ComputesTheFullBatchInOneCall) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::PriceResult result = engine::price(
        registries, *product, {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"},
        *model, market, pricing, execution
    );

    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0].measure_name, "PV");
    EXPECT_EQ(result[2].measure_name, "ExpectedExposure");
    EXPECT_EQ(result[3].measure_name, "PFE95");
    ASSERT_EQ(result[2].result.primary.size(), 5u); // fechas de reseteo: 0,1,2,3,4
    ASSERT_EQ(result[3].result.primary.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_GE(result[3].result.primary[i], result[2].result.primary[i]); // PFE95 >= EE
    }
}

TEST(Price, RejectsUnknownMeasureName) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();

    EXPECT_THROW(
        engine::price(registries, *product, {"NoExiste"}, *model, market, pricing, execution),
        std::invalid_argument
    );
}

// PLAN_REAPI.md §6 Fase 3 (propuesta 3): price() ya no está limitado a la tabla curada de 5
// nombres -- "ExposureProfile" (el type_name() real de la medida detrás de "ExpectedExposure"/
// "PFE95") resuelve directamente contra el registry y devuelve el MeasureResult completo (times
// + primary=EE + secondary=PFE95), sin recortar campos.
TEST(Price, ResolvesMeasureNameDirectlyFromTheRegistry) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    engine::PriceResult result = engine::price(registries, *product, {"ExposureProfile"}, *model, market, pricing, execution);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].measure_name, "ExposureProfile");
    ASSERT_EQ(result[0].result.primary.size(), 5u);   // EE
    ASSERT_EQ(result[0].result.secondary.size(), 5u);  // PFE95, no recortado (a diferencia de "ExpectedExposure")
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_GE(result[0].result.secondary[i], result[0].result.primary[i]); // PFE95 >= EE
    }
}

// PLAN_REAPI.md §6 Fase 3: primera medida con Params real -- DV01(bump=...) escala la misma
// derivada dNPV/dr0 de Rust (irs_hull_white_npv_delta_r0) por un multiplicador configurable en
// vez del 0.0001 (1 punto básico) hardcodeado. Dos bumps distintos deben dar escalares
// distintos y aproximadamente proporcionales -- "aproximadamente" porque desde
// PLAN_REAPI.md §6 Fase 4 DV01 es bump-and-reval sobre la curva de descuento (no ya
// `d(NPV)/d(r0)` exacto vía autodiff): hay una convexidad de segundo orden real en
// `exp(-zero_rate(t)*t)` que hace que duplicar el bump no duplique el DV01 al bit exacto
// (tolerancia relativa, no absoluta -- ver el propio valor para la magnitud del efecto).
TEST(Price, Dv01BumpIsConfigurableViaMeasureSpec) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(1, 1); // DV01 es determinista
    ExecutionContext execution = cpu_execution();

    std::vector<engine::MeasureSpec> measures = {
        {"DV01", {}},                                  // default: bump=0.0001
        {"DV01", {{"bump", 0.0002}}},
    };
    engine::PriceResult result = engine::price(registries, *product, measures, *model, market, pricing, execution);

    ASSERT_EQ(result.size(), 2u);
    ASSERT_TRUE(result[0].result.has_scalar);
    ASSERT_TRUE(result[1].result.has_scalar);
    EXPECT_NE(result[0].result.scalar, result[1].result.scalar);
    EXPECT_NEAR(result[1].result.scalar, result[0].result.scalar * 2.0, 0.01 * result[0].result.scalar); // ~1% de convexidad
}

// Sanity check recomendado explícitamente por PLAN_REAPI.md §6 Fase 4: un swap con
// fixed_rate EXPLÍCITO (no el sentinel PAR/use_par_rate) igual al par rate calculado a mano
// con la MISMA fórmula que usa el motor (P(start)-P(end)) / Σ accrual_i·P(Ti)) sobre una
// curva NO plana debe dar PV ≈ 0 -- a diferencia de market_with_credit() (1 pillar, plana por
// extrapolación), aquí la curva tiene forma real, así que esto no es una tautología del caso
// "1 pillar" -- es la regresión barata que exige el punto de control de la Fase 4.
TEST(Price, ParSwapWithExplicitParRateFromAMultiPillarCurveIsZero) {
    Registries registries;
    register_builtins(registries);

    MarketSnapshot market = upward_sloping_market();
    std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};

    double numerator = market.discount_factor(0.0) - market.discount_factor(payment_times.back());
    double denominator = 0.0;
    for (std::size_t i = 0; i < payment_times.size(); ++i) {
        denominator += accruals[i] * market.discount_factor(payment_times[i]);
    }
    double par_rate = numerator / denominator;

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create(
        "IRSwap", Params{
                      {"notional", 1'000'000.0},
                      {"fixed_rate", par_rate}, // explícito -- NO se omite la clave
                      {"payment_times", payment_times},
                      {"accruals", accruals},
                  }
    );
    PricingContext pricing = golden_pricing(1, 1); // PV es determinista
    ExecutionContext execution = cpu_execution();

    engine::PriceResult result = engine::price(registries, *product, {"PV"}, *model, market, pricing, execution);

    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_NEAR(result[0].result.scalar, 0.0, 1e-6);
}

// Mismo caso que arriba pero via el sentinel use_par_rate() (fixed_rate omitido) -- confirma
// que ambos caminos (par rate calculado a mano vs. delegado al motor) coinciden, sobre la
// MISMA curva no plana.
TEST(Price, ParSwapViaUseParRateMatchesExplicitParRateOnANonFlatCurve) {
    Registries registries;
    register_builtins(registries);

    MarketSnapshot market = upward_sloping_market();
    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto par_product = registries.products.create("IRSwap", par_irs_5y_params()); // fixed_rate omitido
    PricingContext pricing = golden_pricing(1, 1);
    ExecutionContext execution = cpu_execution();

    engine::PriceResult result = engine::price(
        registries, *par_product, std::vector<std::string>{"PV", "DV01"}, *model, market, pricing, execution
    );

    ASSERT_TRUE(result[0].result.has_scalar);
    EXPECT_NEAR(result[0].result.scalar, 0.0, 1e-6); // PV
    ASSERT_TRUE(result[1].result.has_scalar);
    EXPECT_GT(result[1].result.scalar, 0.0); // DV01 de un swap pagador: > 0 pase lo que pase con la curva
}

// PLAN_REAPI.md §6 Fase 5 (construida sobre la Fase 4): DV01(bucketed=true) bumpea cada pillar
// individualmente -- la suma de los deltas por pillar debe coincidir con el DV01 "parcial"
// (bump paralelo) sobre la MISMA curva/bump, exactamente el test de consistencia que pide el
// propio PLAN_REAPI.md.
TEST(Price, BucketedDv01SumsToTheParallelDv01) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    MarketSnapshot market = upward_sloping_market();
    PricingContext pricing = golden_pricing(1, 1); // DV01 es determinista
    ExecutionContext execution = cpu_execution();

    std::vector<engine::MeasureSpec> measures = {
        {"DV01", {}},                                       // escalar, bump paralelo
        {"DV01", {{"bucketed", true}}},                      // vector de deltas por pillar
    };
    engine::PriceResult result = engine::price(registries, *product, measures, *model, market, pricing, execution);

    ASSERT_EQ(result.size(), 2u);
    ASSERT_TRUE(result[0].result.has_scalar);
    ASSERT_FALSE(result[1].result.has_scalar);
    ASSERT_EQ(result[1].result.times.size(), market.pillars().size());
    ASSERT_EQ(result[1].result.primary.size(), market.pillars().size());

    double bucketed_sum = 0.0;
    for (double delta : result[1].result.primary) bucketed_sum += delta;
    EXPECT_NEAR(bucketed_sum, result[0].result.scalar, 1e-6);
}

// Equivalente de lote (PLAN.md §7.19-style): price_batch con DV01(bucketed=true) debe coincidir,
// trade a trade, con llamar a price() una vez por trade -- mismo patrón que
// PriceBatch.MatchesALoopOfScalarCallsPerTrade para el resto de medidas.
TEST(PriceBatch, BucketedDv01MatchesALoopOfScalarCalls) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product_a = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    auto product_b = registries.products.create("IRSwap", irs_5y_params(2'500'000.0, 0.015));
    MarketSnapshot market = upward_sloping_market();
    PricingContext pricing = golden_pricing(1, 1);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{product_a.get(), product_b.get()};
    std::vector<engine::MeasureSpec> measures = {{"DV01", {{"bucketed", true}}}};

    engine::PriceBatchResult batch = engine::price_batch(registries, products, measures, *model, market, pricing, execution);
    ASSERT_EQ(batch.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        engine::PriceResult scalar = engine::price(registries, *products[i], measures, *model, market, pricing, execution);
        ASSERT_EQ(batch[i].measures[0].result.primary.size(), scalar[0].result.primary.size());
        for (std::size_t p = 0; p < scalar[0].result.primary.size(); ++p) {
            EXPECT_NEAR(batch[i].measures[0].result.primary[p], scalar[0].result.primary[p], 1e-6);
        }
    }
}

// PLAN.md §7.19: price_batch (lote homogéneo) debe coincidir, trade a trade, con llamar a
// price() una vez por trade -- mismo espíritu que los tests "matches a loop of scalar calls" de
// Rust, aquí a nivel de la orquestación C++.
TEST(PriceBatch, MatchesALoopOfScalarCallsPerTrade) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product_a = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    auto product_b = registries.products.create("IRSwap", irs_5y_params(2'500'000.0, 0.015));
    auto product_c = registries.products.create("IRSwap", irs_5y_params(500'000.0, 0.025));
    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{product_a.get(), product_b.get(), product_c.get()};
    std::vector<std::string> measures{"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"};

    engine::PriceBatchResult batch = engine::price_batch(registries, products, measures, *model, market, pricing, execution);
    ASSERT_EQ(batch.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        EXPECT_EQ(batch[i].trade_index, i);
        engine::PriceResult scalar = engine::price(registries, *products[i], measures, *model, market, pricing, execution);
        ASSERT_EQ(batch[i].measures.size(), scalar.size());
        for (std::size_t m = 0; m < measures.size(); ++m) {
            EXPECT_EQ(batch[i].measures[m].measure_name, scalar[m].measure_name);
            EXPECT_EQ(batch[i].measures[m].result.has_scalar, scalar[m].result.has_scalar);
            if (scalar[m].result.has_scalar) {
                EXPECT_NEAR(batch[i].measures[m].result.scalar, scalar[m].result.scalar, 1e-6)
                    << "trade " << i << " medida " << measures[m];
            } else {
                ASSERT_EQ(batch[i].measures[m].result.primary.size(), scalar[m].result.primary.size());
                for (std::size_t k = 0; k < scalar[m].result.primary.size(); ++k) {
                    EXPECT_NEAR(batch[i].measures[m].result.primary[k], scalar[m].result.primary[k], 1e-6)
                        << "trade " << i << " medida " << measures[m] << " fecha " << k;
                }
            }
        }
    }
}

TEST(PriceBatch, MatchesALoopOfScalarCallsPerTradeUnderHullWhite2F) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product_a = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    auto product_b = registries.products.create("IRSwap", irs_5y_params(2'500'000.0, 0.015));
    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{product_a.get(), product_b.get()};
    std::vector<std::string> measures{"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"};

    engine::PriceBatchResult batch = engine::price_batch(registries, products, measures, *model, market, pricing, execution);
    ASSERT_EQ(batch.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        engine::PriceResult scalar = engine::price(registries, *products[i], measures, *model, market, pricing, execution);
        for (std::size_t m = 0; m < measures.size(); ++m) {
            if (scalar[m].result.has_scalar) {
                EXPECT_NEAR(batch[i].measures[m].result.scalar, scalar[m].result.scalar, 1e-6);
            } else {
                for (std::size_t k = 0; k < scalar[m].result.primary.size(); ++k) {
                    EXPECT_NEAR(batch[i].measures[m].result.primary[k], scalar[m].result.primary[k], 1e-6);
                }
            }
        }
    }
}

TEST(PriceBatch, RejectsMismatchedCalendars) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product_5y = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    auto product_3y = registries.products.create("IRSwap", irs_3y_params(1'000'000.0, 0.02));
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{product_5y.get(), product_3y.get()};
    EXPECT_THROW(
        engine::price_batch(registries, products, {"PV"}, *model, market, pricing, execution),
        std::invalid_argument
    );
}

TEST(PriceBatch, RejectsUseParRate) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto par_product = registries.products.create("IRSwap", par_irs_5y_params()); // sin fixed_rate
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{par_product.get()};
    EXPECT_THROW(
        engine::price_batch(registries, products, {"PV"}, *model, market, pricing, execution),
        std::invalid_argument
    );
}

TEST(PriceBatch, RejectsEmptyBatch) {
    Registries registries;
    register_builtins(registries);
    auto model = registries.models.create("HullWhite1F", hull_white_params());
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products;
    EXPECT_THROW(
        engine::price_batch(registries, products, {"PV"}, *model, market, pricing, execution),
        std::invalid_argument
    );
}

// PLAN.md §7.19, Nivel 2: price_many acepta calendarios distintos (agrupa internamente) y
// devuelve el resultado en el orden de entrada, sin importar el orden de los grupos.
TEST(PriceMany, GroupsByCalendarAndPreservesInputOrder) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product_5y_a = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    auto product_3y = registries.products.create("IRSwap", irs_3y_params(2'000'000.0, 0.018));
    auto product_5y_b = registries.products.create("IRSwap", irs_5y_params(3'000'000.0, 0.022));
    MarketSnapshot market = market_with_credit(0.02, 0.4);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    // Intercalados a propósito: 5y, 3y, 5y -- dos grupos de calendario, no en bloques contiguos.
    std::vector<const engine::IProduct*> products{product_5y_a.get(), product_3y.get(), product_5y_b.get()};
    std::vector<std::string> measures{"PV", "UnilateralCVA"};

    engine::PriceBatchResult many = engine::price_many(registries, products, measures, *model, market, pricing, execution);
    ASSERT_EQ(many.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        EXPECT_EQ(many[i].trade_index, i);
        engine::PriceResult scalar = engine::price(registries, *products[i], measures, *model, market, pricing, execution);
        for (std::size_t m = 0; m < measures.size(); ++m) {
            EXPECT_NEAR(many[i].measures[m].result.scalar, scalar[m].result.scalar, 1e-6)
                << "trade " << i << " medida " << measures[m];
        }
    }
}

TEST(PriceMany, RejectsEmptyList) {
    Registries registries;
    register_builtins(registries);
    auto model = registries.models.create("HullWhite1F", hull_white_params());
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products;
    EXPECT_THROW(
        engine::price_many(registries, products, {"PV"}, *model, market, pricing, execution),
        std::invalid_argument
    );
}

// PLAN.md §7.19: price_grid explota Trades x Models x Markets -- PricingContext/
// ExecutionContext son compartidos, no forman parte de la rejilla.
TEST(PriceGrid, ComputesTradesTimesModelsTimesMarkets) {
    Registries registries;
    register_builtins(registries);

    auto model_1f = registries.models.create("HullWhite1F", hull_white_params());
    auto model_2f = registries.models.create("HullWhite2F", hull_white_2f_params());
    auto product_a = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    auto product_b = registries.products.create("IRSwap", irs_5y_params(2'000'000.0, 0.018));
    MarketSnapshot market_a = market_with_credit(0.02, 0.4);
    MarketSnapshot market_b = market_with_credit(0.05, 0.3);
    PricingContext pricing = golden_pricing(5000, 7);
    ExecutionContext execution = cpu_execution();

    std::vector<const engine::IProduct*> products{product_a.get(), product_b.get()};
    std::vector<const engine::IModel*> models{model_1f.get(), model_2f.get()};
    std::vector<MarketSnapshot> markets{market_a, market_b};
    std::vector<std::string> measures{"PV", "UnilateralCVA"};

    engine::PriceGridResult grid = engine::price_grid(registries, products, measures, models, markets, pricing, execution);
    ASSERT_EQ(grid.size(), products.size() * models.size() * markets.size());

    // Cada celda de la rejilla debe coincidir con la llamada escalar equivalente.
    for (const auto& cell : grid) {
        engine::PriceResult scalar = engine::price(
            registries, *products[cell.trade_index], measures, *models[cell.model_index], markets[cell.market_index],
            pricing, execution
        );
        for (std::size_t m = 0; m < measures.size(); ++m) {
            EXPECT_NEAR(cell.measures[m].result.scalar, scalar[m].result.scalar, 1e-6)
                << "trade=" << cell.trade_index << " model=" << cell.model_index << " market=" << cell.market_index;
        }
    }
}

TEST(PriceGrid, RejectsEmptyModelsOrMarkets) {
    Registries registries;
    register_builtins(registries);
    auto product = registries.products.create("IRSwap", irs_5y_params(1'000'000.0, 0.02));
    MarketSnapshot market = market_with_credit(0.0, 0.0);
    PricingContext pricing = golden_pricing(100, 1);
    ExecutionContext execution = cpu_execution();
    auto model = registries.models.create("HullWhite1F", hull_white_params());

    std::vector<const engine::IProduct*> products{product.get()};
    std::vector<const engine::IModel*> no_models;
    std::vector<const engine::IModel*> some_models{model.get()};
    std::vector<MarketSnapshot> no_markets;
    std::vector<MarketSnapshot> some_markets{market};

    EXPECT_THROW(
        engine::price_grid(registries, products, {"PV"}, no_models, some_markets, pricing, execution),
        std::invalid_argument
    );
    EXPECT_THROW(
        engine::price_grid(registries, products, {"PV"}, some_models, no_markets, pricing, execution),
        std::invalid_argument
    );
}
