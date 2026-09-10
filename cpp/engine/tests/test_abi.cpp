// Tests de la API universal en C ABI (PLAN.md Fase 6, §5.5, engine/abi.h). A diferencia de
// test_registry.cpp (que enlaza directamente contra los tipos C++ del registry), este fichero
// solo usa la superficie extern "C" de abi.h -- las mismas funciones que vería un consumidor
// en Julia/.NET/Go -- para confirmar que la traducción a structs planos/punteros+longitud
// funciona de punta a punta, sin reimplementar la validación numérica fina que ya cubre Rust
// (PLAN.md §5.6 capa 2). Los valores de referencia de UnilateralCVA/ExposureProfile son los
// mismos "caso dorado" que documenta clients/excel/README.md (verificación manual de Excel
// real) y que test_registry.py fija con tolerancia laxa -- aquí, al ser el mismo cálculo por
// debajo, se comprueban con tolerancia estrecha (PLAN.md §5.6 capa 4: equivalencia entre
// TODOS los clientes, C ABI incluida desde esta fase).

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/abi.h"

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
struct MeasureHandle {
    EngineMeasure* ptr;
    ~MeasureHandle() { engine_abi_free_measure(ptr); }
};
struct ResultHandle {
    EngineMeasureResult value{};
    ~ResultHandle() { engine_abi_free_measure_result(&value); }
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

TEST(Abi, VersionIsPositive) {
    EXPECT_GT(engine_abi_version(), 0);
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

TEST(Abi, CreateUnknownModelReturnsNullAndSetsLastError) {
    EngineModel* model = engine_abi_create_model("NoExiste", nullptr, 0);
    EXPECT_EQ(model, nullptr);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, EvaluateRejectsNullHandles) {
    EngineMeasureResult result{};
    int rc = engine_abi_evaluate(nullptr, nullptr, nullptr, nullptr, 0, &result);
    EXPECT_NE(rc, 0);
    EXPECT_EQ(result.len, 0u);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, ExposureProfileMatchesGoldenCaseFromOtherClients) {
    ModelHandle model = create_hull_white();
    ParIrs5y irs;
    ProductHandle product = irs.create();
    MeasureHandle measure{engine_abi_create_measure("ExposureProfile")};
    ASSERT_NE(model.ptr, nullptr);
    ASSERT_NE(product.ptr, nullptr);
    ASSERT_NE(measure.ptr, nullptr);

    std::vector<double> monitoring_times{0.0, 1.0, 2.0};
    EngineParam params[] = {
        vector_param("monitoring_times", monitoring_times),
        scalar_param("n_paths", 5000.0),
        scalar_param("seed", 7.0),
    };

    ResultHandle result;
    int rc = engine_abi_evaluate(measure.ptr, model.ptr, product.ptr, params, 3, &result.value);
    ASSERT_EQ(rc, 0) << last_error();

    ASSERT_EQ(result.value.len, 3u);
    // Mismo caso/semilla que clients/excel/README.md ("Verificación manual", ExposureProfile):
    // EE≈[0, 12862.62, 13673.53], PFE95≈[0, 51009.92, 53607.17].
    EXPECT_NEAR(result.value.primary[0], 0.0, 1e-6);
    EXPECT_NEAR(result.value.primary[1], 12862.62, 1.0);
    EXPECT_NEAR(result.value.primary[2], 13673.53, 1.0);
    EXPECT_NEAR(result.value.secondary[0], 0.0, 1e-6);
    EXPECT_NEAR(result.value.secondary[1], 51009.92, 1.0);
    EXPECT_NEAR(result.value.secondary[2], 53607.17, 1.0);
    EXPECT_EQ(result.value.has_scalar, 0);
}

TEST(Abi, UnilateralCvaMatchesGoldenCaseFromOtherClients) {
    ModelHandle model = create_hull_white();
    ParIrs5y irs;
    ProductHandle product = irs.create();
    MeasureHandle measure{engine_abi_create_measure("UnilateralCVA")};
    ASSERT_NE(model.ptr, nullptr);
    ASSERT_NE(product.ptr, nullptr);
    ASSERT_NE(measure.ptr, nullptr);

    std::vector<double> monitoring_times{0.0, 1.0, 2.0, 3.0};
    EngineParam params[] = {
        vector_param("monitoring_times", monitoring_times),
        scalar_param("n_paths", 5000.0),
        scalar_param("seed", 13.0),
        scalar_param("hazard_rate", 0.02),
        scalar_param("recovery_rate", 0.4),
    };

    ResultHandle result;
    int rc = engine_abi_evaluate(measure.ptr, model.ptr, product.ptr, params, 5, &result.value);
    ASSERT_EQ(rc, 0) << last_error();

    // Mismo caso/semilla que clients/excel/README.md: CVA = 426.7618244093184.
    EXPECT_TRUE(result.value.has_scalar);
    EXPECT_NEAR(result.value.scalar, 426.7618244093184, 1e-6);
}

TEST(Abi, ComputeBackendDefaultsToCpuAndRejectsUnknownName) {
    char buffer[16] = {};
    std::size_t len = engine_abi_get_compute_backend(buffer, sizeof(buffer));
    EXPECT_EQ(std::string(buffer, len), "cpu");

    EXPECT_EQ(engine_abi_set_compute_backend("tpu"), 0);
    len = engine_abi_get_compute_backend(buffer, sizeof(buffer));
    EXPECT_EQ(std::string(buffer, len), "cpu"); // sin cambios tras el nombre invalido

    EXPECT_EQ(engine_abi_set_compute_backend("cpu"), 1); // no-op valido, ver PLAN.md §7.12
}

TEST(Abi, IsGpuBackendAvailableIsBoolLike) {
    int available = engine_abi_is_gpu_backend_available();
    EXPECT_TRUE(available == 0 || available == 1);
}
