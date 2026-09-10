// Tests de la API universal en C ABI (PLAN.md §7.15, engine/abi.h). A diferencia de
// test_registry.cpp (que enlaza directamente contra los tipos C++ del registry), este fichero
// solo usa la superficie extern "C" de abi.h -- las mismas funciones que vería un consumidor
// en Julia/.NET/Go -- para confirmar que la traducción a structs planos/punteros+longitud
// funciona de punta a punta, sin reimplementar la validación numérica fina que ya cubre Rust
// (PLAN.md §5.6 capa 2). El valor de referencia exacto de UnilateralCVA vive en
// test_registry.cpp (mismo caso, comprobado allí bit a bit) -- aquí basta con invariantes
// (no negatividad, PFE95>=EE, etc.), ya que esta suite verifica el mecanismo de la ABI, no
// vuelve a fijar el número.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/abi.h"
#include "engine/engine.hpp"

namespace {

EngineParam scalar_param(const char* key, double value) {
    EngineParam p{};
    p.key = key;
    p.kind = ENGINE_PARAM_DOUBLE;
    p.scalar = value;
    return p;
}

EngineParam vector_param(const char* key, const std::vector<double>& values) {
    EngineParam p{};
    p.key = key;
    p.kind = ENGINE_PARAM_VECTOR;
    p.values = values.data();
    p.count = values.size();
    return p;
}

std::string last_error() {
    std::size_t len = engine_abi_last_error(nullptr, 0);
    std::string buffer(len, '\0');
    engine_abi_last_error(buffer.data(), len + 1);
    return buffer;
}

// RAII mínimo para los handles opacos: evita fugas/duplicar el try/catch en cada TEST si una
// aserción falla a mitad de test (ASSERT_* hace return temprano).
struct ModelHandle {
    EngineModel* ptr;
    ~ModelHandle() { engine_abi_free_model(ptr); }
};
struct ProductHandle {
    EngineProduct* ptr;
    ~ProductHandle() { engine_abi_free_product(ptr); }
};
struct CalcResultsHandle {
    EngineCalcResultEntry* entries = nullptr;
    std::size_t count = 0;
    ~CalcResultsHandle() { engine_abi_free_calc_results(entries, count); }

    const EngineCalcResultEntry* find(const char* name) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(entries[i].measure_name, name) == 0) return &entries[i];
        }
        return nullptr;
    }
};

ModelHandle create_hull_white() {
    EngineParam params[] = {
        scalar_param("a", 0.1),
        scalar_param("b", 0.03),
        scalar_param("sigma", 0.01),
        scalar_param("r0", 0.02),
    };
    return ModelHandle{engine_abi_create_model("HullWhite1F", params, 4)};
}

// Vive fuera de la función para que payment_times/accruals sigan vivos mientras el
// EngineParam (que solo apunta a ellos, no los copia) se usa en engine_abi_create_product.
struct ParIrs5y {
    std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};

    ProductHandle create() const {
        EngineParam params[] = {
            scalar_param("notional", 1'000'000.0),
            vector_param("payment_times", payment_times),
            vector_param("accruals", accruals),
        };
        return ProductHandle{engine_abi_create_product("IRSwap", params, 3)};
    }
};

} // namespace

TEST(Abi, VersionIsAtLeastTwo) {
    // Version 2 (PLAN.md §7.15): engine_abi_calc sustituye a engine_abi_create_measure/
    // engine_abi_evaluate, EngineMarketSnapshot gana hazard_rate/recovery_rate.
    EXPECT_GE(engine_abi_version(), 2);
}

TEST(Abi, ListModelsIncludesHullWhite1F) {
    const char** names = nullptr;
    std::size_t count = engine_abi_list_models(&names);
    ASSERT_GT(count, 0u);

    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(names[i], "HullWhite1F") == 0) found = true;
    }
    EXPECT_TRUE(found);
    engine_abi_free_string_list(names, count);
}

TEST(Abi, ListMeasuresIncludesTheFiveCalcNames) {
    const char** names = nullptr;
    std::size_t count = engine_abi_list_measures(&names);
    ASSERT_GT(count, 0u);

    auto contains = [&](const char* target) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(names[i], target) == 0) return true;
        }
        return false;
    };
    EXPECT_TRUE(contains("PV"));
    EXPECT_TRUE(contains("DV01"));
    EXPECT_TRUE(contains("ExpectedExposure"));
    EXPECT_TRUE(contains("PFE95"));
    EXPECT_TRUE(contains("UnilateralCVA"));
    engine_abi_free_string_list(names, count);
}

TEST(Abi, CreateUnknownModelReturnsNullAndSetsLastError) {
    EngineModel* model = engine_abi_create_model("NoExiste", nullptr, 0);
    EXPECT_EQ(model, nullptr);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, CalcRejectsNullHandles) {
    const char* names[] = {"PV"};
    EngineCalcResultEntry* entries = nullptr;
    std::size_t count = 0;
    int rc = engine_abi_calc(nullptr, names, 1, nullptr, nullptr, nullptr, nullptr, &entries, &count);
    EXPECT_NE(rc, 0);
    EXPECT_EQ(entries, nullptr);
    EXPECT_EQ(count, 0u);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, CalcRejectsUnknownMeasureName) {
    ModelHandle model = create_hull_white();
    ParIrs5y irs;
    ProductHandle product = irs.create();

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &rate;
    market.count = 1;

    EnginePricingContext pricing{};
    pricing.n_paths = 1000;
    pricing.n_steps = 52;
    pricing.seed = 1;

    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"NoExiste"};
    EngineCalcResultEntry* entries = nullptr;
    std::size_t count = 0;
    int rc = engine_abi_calc(
        product.ptr, names, 1, model.ptr, &market, &pricing, &execution, &entries, &count);
    EXPECT_NE(rc, 0);
    EXPECT_EQ(entries, nullptr);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, CalcComputesAllFiveMeasuresInOneBatch) {
    ModelHandle model = create_hull_white();
    ParIrs5y irs;
    ProductHandle product = irs.create();

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &rate;
    market.count = 1;
    market.hazard_rate = 0.02;
    market.recovery_rate = 0.4;

    EnginePricingContext pricing{};
    pricing.pricing_date = 0.0;
    pricing.n_paths = 5000;
    pricing.n_steps = 208; // ~1 paso/semana sobre los 4 años hasta la última fecha de reseteo
    pricing.seed = 7;

    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"};
    CalcResultsHandle results;
    int rc = engine_abi_calc(
        product.ptr, names, 5, model.ptr, &market, &pricing, &execution, &results.entries, &results.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(results.count, 5u);

    const EngineCalcResultEntry* pv = results.find("PV");
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->result.has_scalar);
    EXPECT_NEAR(pv->result.scalar, 0.0, 1e-6); // swap a la par: NPV ~ 0 en start

    const EngineCalcResultEntry* dv01 = results.find("DV01");
    ASSERT_NE(dv01, nullptr);
    EXPECT_TRUE(dv01->result.has_scalar);
    EXPECT_GT(dv01->result.scalar, 0.0); // swap pagador: sube de valor cuando suben los tipos

    // ExpectedExposure/PFE95 comparten una sola simulación (PLAN.md §7.15): 5 fechas de
    // reseteo auto-derivadas del swap (0,1,2,3,4), no una lista pedida a mano.
    const EngineCalcResultEntry* ee = results.find("ExpectedExposure");
    ASSERT_NE(ee, nullptr);
    ASSERT_EQ(ee->result.len, 5u);
    const EngineCalcResultEntry* pfe = results.find("PFE95");
    ASSERT_NE(pfe, nullptr);
    ASSERT_EQ(pfe->result.len, 5u);
    for (std::size_t i = 0; i < ee->result.len; ++i) {
        EXPECT_GE(ee->result.primary[i], 0.0);
        EXPECT_GE(pfe->result.primary[i], ee->result.primary[i]);
    }

    const EngineCalcResultEntry* cva = results.find("UnilateralCVA");
    ASSERT_NE(cva, nullptr);
    EXPECT_TRUE(cva->result.has_scalar);
    EXPECT_GT(cva->result.scalar, 0.0);
}

TEST(Abi, IsGpuBackendAvailableIsBoolLike) {
    int available = engine_abi_is_gpu_backend_available();
    EXPECT_TRUE(available == 0 || available == 1);
}

TEST(Abi, CalibrateHullWhiteRecoversKnownParametersFromASyntheticMarket) {
    // Mercado "falso" (PLAN.md §7.14) fabricado a mano aquí (no via
    // MarketSnapshot::synthetic_from_hull_white, que es C++ interno, no parte de esta ABI):
    // valores calculados con hull_white_zero_coupon_bond del propio motor.
    double pillars[] = {0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
    double true_a = 0.15, true_b = 0.025, sigma = 0.008, r0 = 0.02;
    double zero_rates[10];
    for (int i = 0; i < 10; ++i) {
        double price = engine::hull_white_zero_coupon_bond(true_a, true_b, sigma, r0, 0.0, pillars[i]);
        zero_rates[i] = -std::log(price) / pillars[i];
    }
    EngineMarketSnapshot market{};
    market.pillars = pillars;
    market.zero_rates = zero_rates;
    market.count = 10;

    EngineHullWhiteCalibration result{};
    int rc = engine_abi_calibrate_hull_white(&market, 0.3, 0.01, sigma, r0, &result);
    ASSERT_EQ(rc, 0) << last_error();

    EXPECT_TRUE(result.converged);
    EXPECT_NEAR(result.a, true_a, 1e-4);
    EXPECT_NEAR(result.b, true_b, 1e-4);
    EXPECT_EQ(result.sigma, sigma);
    EXPECT_EQ(result.r0, r0);
}

TEST(Abi, CalibrateHullWhiteRejectsNullMarket) {
    EngineHullWhiteCalibration result{};
    int rc = engine_abi_calibrate_hull_white(nullptr, 0.1, 0.03, 0.01, 0.02, &result);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(last_error().empty());
}
