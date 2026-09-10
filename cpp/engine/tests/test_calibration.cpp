// Tests de MarketSnapshot/ICalibrator (PLAN.md §7.14). No repiten la validación numérica fina
// del optimizador (eso ya lo cubre `rust/crates/engine-core/src/calibration.rs`): confirman
// que el wiring C++ (Registry<ICalibrator> + bootstrap + MarketSnapshot) funciona de punta a
// punta, igual que test_registry.cpp hace para modelos/productos/medidas.

#include <cmath>
#include <stdexcept>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "engine/bootstrap.hpp"
#include "engine/engine.hpp"

namespace {

using engine::MarketSnapshot;
using engine::Params;
using engine::Registries;
using engine::register_builtins;

} // namespace

TEST(Market, DiscountFactorMatchesContinuousCompoundingFormula) {
    MarketSnapshot market({1.0, 2.0}, {0.02, 0.02});
    double expected = std::exp(-0.02 * 2.0);
    EXPECT_NEAR(market.discount_factor(2.0), expected, 1e-12);
}

TEST(Market, ZeroRateInterpolatesLinearlyBetweenPillars) {
    MarketSnapshot market({1.0, 2.0, 5.0}, {0.02, 0.03, 0.04});
    EXPECT_NEAR(market.zero_rate(1.0), 0.02, 1e-12);
    EXPECT_NEAR(market.zero_rate(2.0), 0.03, 1e-12);
    EXPECT_NEAR(market.zero_rate(3.5), 0.035, 1e-12); // a mitad de camino entre 2.0 y 5.0
}

TEST(Market, ZeroRateExtrapolatesFlatOutsidePillars) {
    MarketSnapshot market({1.0, 5.0}, {0.02, 0.04});
    EXPECT_NEAR(market.zero_rate(0.1), 0.02, 1e-12);
    EXPECT_NEAR(market.zero_rate(10.0), 0.04, 1e-12);
}

TEST(Market, ConstructorRejectsMismatchedLengths) {
    EXPECT_THROW(MarketSnapshot({1.0, 2.0}, {0.02}), std::invalid_argument);
}

TEST(Market, ConstructorRejectsNonIncreasingPillars) {
    EXPECT_THROW(MarketSnapshot({1.0, 1.0}, {0.02, 0.03}), std::invalid_argument);
}

TEST(Market, SyntheticFromHullWhiteReproducesTheModelsOwnPrices) {
    double a = 0.1, b = 0.03, sigma = 0.01, r0 = 0.02;
    std::vector<double> pillars{1.0, 2.0, 5.0, 10.0};
    MarketSnapshot market = MarketSnapshot::synthetic_from_hull_white(a, b, sigma, r0, pillars);

    for (double t : pillars) {
        double expected = engine::hull_white_zero_coupon_bond(a, b, sigma, r0, 0.0, t);
        EXPECT_NEAR(market.discount_factor(t), expected, 1e-9) << "t=" << t;
    }
}

TEST(Registry, RegisterBuiltinsPopulatesCalibratorRegistry) {
    Registries registries;
    register_builtins(registries);

    EXPECT_TRUE(registries.calibrators.contains("HullWhite1F"));
}

TEST(Calibrator, HullWhite1FRecoversKnownParametersFromASyntheticMarket) {
    Registries registries;
    register_builtins(registries);

    double true_a = 0.15, true_b = 0.025, sigma = 0.008, r0 = 0.02;
    std::vector<double> pillars{0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
    MarketSnapshot market = MarketSnapshot::synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars);

    auto calibrator = registries.calibrators.create("HullWhite1F");
    // Estimación inicial deliberadamente lejos de los parámetros "verdaderos".
    Params initial_guess{{"a", 0.3}, {"b", 0.01}, {"sigma", sigma}, {"r0", r0}};

    engine::CalibrationResult result = calibrator->calibrate(market, initial_guess);

    EXPECT_TRUE(result.converged) << "rmse=" << result.rmse << " iterations=" << result.iterations;
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("a")), true_a, 1e-4);
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("b")), true_b, 1e-4);
    EXPECT_EQ(std::get<double>(result.optimal_params.at("sigma")), sigma);
    EXPECT_EQ(std::get<double>(result.optimal_params.at("r0")), r0);

    // El resultado debe poder alimentar directamente Registry<IModel>::create -- ese es el
    // punto de calibrar: cerrar el círculo Market -> calibrar -> Model calibrado.
    auto calibrated_model = registries.models.create("HullWhite1F", result.optimal_params);
    EXPECT_EQ(calibrated_model->type_name(), "HullWhite1F");
}

TEST(Market, SyntheticFromHullWhite2fReproducesTheModelsOwnPrices) {
    double a = 0.1, b = 0.2, sigma = 0.01, eta = 0.012, rho = -0.7, r0 = 0.03;
    std::vector<double> pillars{1.0, 2.0, 5.0, 10.0};
    MarketSnapshot market = MarketSnapshot::synthetic_from_hull_white_2f(a, b, sigma, eta, rho, r0, pillars);

    for (double t : pillars) {
        double expected = engine::hull_white_2f_zero_coupon_bond(a, b, sigma, eta, rho, r0, t);
        EXPECT_NEAR(market.discount_factor(t), expected, 1e-9) << "t=" << t;
    }
}

TEST(Registry, RegisterBuiltinsPopulatesHullWhite2fCalibrator) {
    Registries registries;
    register_builtins(registries);

    EXPECT_TRUE(registries.calibrators.contains("HullWhite2F"));
}

TEST(Calibrator, HullWhite2FRecoversKnownParametersFromASyntheticMarket) {
    Registries registries;
    register_builtins(registries);

    double true_a = 0.15, true_b = 0.25, sigma = 0.008, eta = 0.01, rho = -0.6, r0 = 0.02;
    std::vector<double> pillars{0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
    MarketSnapshot market = MarketSnapshot::synthetic_from_hull_white_2f(true_a, true_b, sigma, eta, rho, r0, pillars);

    auto calibrator = registries.calibrators.create("HullWhite2F");
    // Estimación inicial deliberadamente lejos de los parámetros "verdaderos".
    Params initial_guess{{"a", 0.4}, {"b", 0.05}, {"sigma", sigma}, {"eta", eta}, {"rho", rho}, {"r0", r0}};

    engine::CalibrationResult result = calibrator->calibrate(market, initial_guess);

    EXPECT_TRUE(result.converged) << "rmse=" << result.rmse << " iterations=" << result.iterations;
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("a")), true_a, 1e-4);
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("b")), true_b, 1e-4);
    EXPECT_EQ(std::get<double>(result.optimal_params.at("sigma")), sigma);
    EXPECT_EQ(std::get<double>(result.optimal_params.at("eta")), eta);
    EXPECT_EQ(std::get<double>(result.optimal_params.at("rho")), rho);
    EXPECT_EQ(std::get<double>(result.optimal_params.at("r0")), r0);

    // El resultado debe poder alimentar directamente Registry<IModel>::create -- ese es el
    // punto de calibrar: cerrar el círculo Market -> calibrar -> Model calibrado.
    auto calibrated_model = registries.models.create("HullWhite2F", result.optimal_params);
    EXPECT_EQ(calibrated_model->type_name(), "HullWhite2F");
}

TEST(Calibrator, CreateUnknownCalibratorThrows) {
    Registries registries;
    register_builtins(registries);

    EXPECT_THROW(registries.calibrators.create("NoExiste"), std::out_of_range);
}
