// Tests de integración del registry/composición de medidas (PLAN.md §5.4, Fase 2). No
// pretenden repetir la validación numérica fina que ya vive en Rust (PLAN.md §5.6 capa 2,
// `rust/crates/engine-core/src/exposure.rs`) — solo confirman que el wiring C++
// (Registry<T> + IModel/IProduct/IMeasure + bootstrap) funciona de punta a punta.

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "engine/bootstrap.hpp"

namespace {

using engine::Params;
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

Params par_irs_5y_params() {
    return Params{
        {"notional", 1'000'000.0},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0, 1.0, 1.0}},
    };
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
    EXPECT_TRUE(registries.products.contains("IRSwap"));
    EXPECT_TRUE(registries.measures.contains("ExposureProfile"));
    EXPECT_TRUE(registries.measures.contains("UnilateralCVA"));
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

    Params measure_params{
        {"monitoring_times", std::vector<double>{0.0, 1.0, 2.0}},
        {"n_paths", 5000.0},
        {"seed", 7.0},
    };

    engine::MeasureResult result = measure->evaluate(*model, *product, measure_params);

    ASSERT_EQ(result.primary.size(), result.secondary.size());
    for (std::size_t i = 0; i < result.primary.size(); ++i) {
        EXPECT_GE(result.primary[i], 0.0);
        EXPECT_GE(result.secondary[i], result.primary[i]);
    }
}

TEST(Registry, UnilateralCvaIsPositiveForNonzeroHazardRate) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    Params measure_params{
        {"monitoring_times", std::vector<double>{0.0, 1.0, 2.0, 3.0}},
        {"n_paths", 5000.0},
        {"seed", 13.0},
        {"hazard_rate", 0.02},
        {"recovery_rate", 0.4},
    };

    engine::MeasureResult result = measure->evaluate(*model, *product, measure_params);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
}

TEST(Registry, UnilateralCvaIsZeroWhenHazardRateIsZero) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto product = registries.products.create("IRSwap", par_irs_5y_params());
    auto measure = registries.measures.create("UnilateralCVA");

    Params measure_params{
        {"monitoring_times", std::vector<double>{0.0, 1.0, 2.0, 3.0}},
        {"n_paths", 5000.0},
        {"seed", 13.0},
        {"hazard_rate", 0.0},
        {"recovery_rate", 0.4},
    };

    engine::MeasureResult result = measure->evaluate(*model, *product, measure_params);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_LT(std::abs(result.scalar), 1e-9);
}

TEST(Registry, MeasureRejectsWrongProductType) {
    Registries registries;
    register_builtins(registries);

    auto model = registries.models.create("HullWhite1F", hull_white_params());
    auto measure = registries.measures.create("ExposureProfile");

    FakeProduct fake_product;
    Params measure_params{
        {"monitoring_times", std::vector<double>{0.0, 1.0}},
        {"n_paths", 100.0},
        {"seed", 1.0},
    };

    EXPECT_THROW(measure->evaluate(*model, fake_product, measure_params), std::invalid_argument);
}
