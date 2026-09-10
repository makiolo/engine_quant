// Tests de la API universal en C ABI (PLAN.md §7.15, engine/abi.h). A diferencia de
// test_registry.cpp (que enlaza directamente contra los tipos C++ del registry), este fichero
// solo usa la superficie extern "C" de abi.h -- las mismas funciones que vería un consumidor
// en Julia/.NET/Go -- para confirmar que la traducción a structs planos/punteros+longitud
// funciona de punta a punta, sin reimplementar la validación numérica fina que ya cubre Rust
// (PLAN.md §5.6 capa 2). El valor de referencia exacto de UnilateralCVA vive en
// test_registry.cpp (mismo caso, comprobado allí bit a bit) -- aquí basta con invariantes
// (no negatividad, PFE95>=EE, etc.), ya que esta suite verifica el mecanismo de la ABI, no
// vuelve a fijar el número.

#include <algorithm>
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

// Segundo modelo del motor, Hull-White 2 factores (PLAN.md §7.16): mismos parámetros de
// referencia que cpp/engine/tests/test_registry.cpp::hull_white_2f_params().
ModelHandle create_hull_white_2f() {
    EngineParam params[] = {
        scalar_param("a", 0.1),
        scalar_param("b", 0.2),
        scalar_param("sigma", 0.01),
        scalar_param("eta", 0.012),
        scalar_param("rho", -0.7),
        scalar_param("r0", 0.03),
    };
    return ModelHandle{engine_abi_create_model("HullWhite2F", params, 6)};
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

// Con fixed_rate explícito (PLAN.md §7.19): engine_abi_calc_batch/_many no soportan
// use_par_rate, a diferencia de ParIrs5y.
struct Irs5y {
    double notional;
    double fixed_rate;
    std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};

    ProductHandle create() const {
        EngineParam params[] = {
            scalar_param("notional", notional),
            scalar_param("fixed_rate", fixed_rate),
            vector_param("payment_times", payment_times),
            vector_param("accruals", accruals),
        };
        return ProductHandle{engine_abi_create_product("IRSwap", params, 4)};
    }
};

struct Irs3y {
    double notional;
    double fixed_rate;
    std::vector<double> payment_times{1.0, 2.0, 3.0};
    std::vector<double> accruals{1.0, 1.0, 1.0};

    ProductHandle create() const {
        EngineParam params[] = {
            scalar_param("notional", notional),
            scalar_param("fixed_rate", fixed_rate),
            vector_param("payment_times", payment_times),
            vector_param("accruals", accruals),
        };
        return ProductHandle{engine_abi_create_product("IRSwap", params, 4)};
    }
};

struct CalcBatchResultsHandle {
    EngineCalcBatchResultEntry* entries = nullptr;
    std::size_t count = 0;
    ~CalcBatchResultsHandle() { engine_abi_free_calc_batch_results(entries, count); }
};

struct CalcGridResultsHandle {
    EngineCalcGridResultEntry* entries = nullptr;
    std::size_t count = 0;
    ~CalcGridResultsHandle() { engine_abi_free_calc_grid_results(entries, count); }
};

const EngineCalcResultEntry* find_measure(const EngineCalcResultEntry* entries, std::size_t count, const char* name) {
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(entries[i].measure_name, name) == 0) return &entries[i];
    }
    return nullptr;
}

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

TEST(Abi, ListModelsIncludesHullWhite2F) {
    const char** names = nullptr;
    std::size_t count = engine_abi_list_models(&names);
    ASSERT_GT(count, 0u);

    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(names[i], "HullWhite2F") == 0) found = true;
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

// Interfaz homogénea (PLAN.md §7.16) llevada hasta la ABI en C: mismo engine_abi_calc, mismo
// EngineModel* opaco, solo cambia el nombre pasado a engine_abi_create_model -- ni un
// consumidor en Julia/.NET/Go tendría que distinguir HullWhite1F de HullWhite2F salvo por el
// nombre y los parámetros.
TEST(Abi, CalcComputesAllFiveMeasuresInOneBatchUnderHullWhite2F) {
    ModelHandle model = create_hull_white_2f();
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
    pricing.n_steps = 208;
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
    EXPECT_NEAR(pv->result.scalar, 0.0, 1e-6);

    const EngineCalcResultEntry* dv01 = results.find("DV01");
    ASSERT_NE(dv01, nullptr);
    EXPECT_TRUE(dv01->result.has_scalar);
    EXPECT_GT(dv01->result.scalar, 0.0);

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

// PLAN.md §7.19: engine_abi_calc_batch (lote homogéneo) debe coincidir, trade a trade, con
// llamar a engine_abi_calc una vez por trade.
TEST(Abi, CalcBatchMatchesALoopOfScalarCallsPerTrade) {
    ModelHandle model = create_hull_white();
    Irs5y irs_a{1'000'000.0, 0.02};
    Irs5y irs_b{2'500'000.0, 0.015};
    Irs5y irs_c{500'000.0, 0.025};
    ProductHandle product_a = irs_a.create();
    ProductHandle product_b = irs_b.create();
    ProductHandle product_c = irs_c.create();

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &rate;
    market.count = 1;
    market.hazard_rate = 0.02;
    market.recovery_rate = 0.4;

    EnginePricingContext pricing{};
    pricing.n_paths = 5000;
    pricing.n_steps = 208;
    pricing.seed = 7;

    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"};
    const EngineProduct* products[] = {product_a.ptr, product_b.ptr, product_c.ptr};

    CalcBatchResultsHandle batch;
    int rc = engine_abi_calc_batch(
        products, 3, names, 5, model.ptr, &market, &pricing, &execution, &batch.entries, &batch.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(batch.count, 3u);

    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(batch.entries[i].trade_index, i);
        CalcResultsHandle scalar;
        int scalar_rc = engine_abi_calc(
            products[i], names, 5, model.ptr, &market, &pricing, &execution, &scalar.entries, &scalar.count);
        ASSERT_EQ(scalar_rc, 0) << last_error();

        for (const char* name : names) {
            const EngineCalcResultEntry* from_batch = find_measure(batch.entries[i].measures, batch.entries[i].n_measures, name);
            const EngineCalcResultEntry* from_scalar = find_measure(scalar.entries, scalar.count, name);
            ASSERT_NE(from_batch, nullptr);
            ASSERT_NE(from_scalar, nullptr);
            if (from_scalar->result.has_scalar) {
                EXPECT_NEAR(from_batch->result.scalar, from_scalar->result.scalar, 1e-6) << name;
            } else {
                ASSERT_EQ(from_batch->result.len, from_scalar->result.len);
                for (std::size_t k = 0; k < from_scalar->result.len; ++k) {
                    EXPECT_NEAR(from_batch->result.primary[k], from_scalar->result.primary[k], 1e-6) << name << " " << k;
                }
            }
        }
    }
}

TEST(Abi, CalcBatchRejectsMismatchedCalendars) {
    ModelHandle model = create_hull_white();
    Irs5y irs_5y{1'000'000.0, 0.02};
    Irs3y irs_3y{1'000'000.0, 0.02};
    ProductHandle product_5y = irs_5y.create();
    ProductHandle product_3y = irs_3y.create();

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &rate;
    market.count = 1;

    EnginePricingContext pricing{};
    pricing.n_paths = 100;
    pricing.n_steps = 10;
    pricing.seed = 1;
    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"PV"};
    const EngineProduct* products[] = {product_5y.ptr, product_3y.ptr};
    CalcBatchResultsHandle batch;
    int rc = engine_abi_calc_batch(
        products, 2, names, 1, model.ptr, &market, &pricing, &execution, &batch.entries, &batch.count);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(last_error().empty());
}

// PLAN.md §7.19, Nivel 2: engine_abi_calc_many agrupa internamente por calendario y nunca
// falla por heterogeneidad (a diferencia de engine_abi_calc_batch en el test anterior).
TEST(Abi, CalcManyGroupsHeterogeneousCalendars) {
    ModelHandle model = create_hull_white();
    Irs5y irs_5y{1'000'000.0, 0.02};
    Irs3y irs_3y{2'000'000.0, 0.018};
    ProductHandle product_5y = irs_5y.create();
    ProductHandle product_3y = irs_3y.create();

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &rate;
    market.count = 1;
    market.hazard_rate = 0.02;
    market.recovery_rate = 0.4;

    EnginePricingContext pricing{};
    pricing.n_paths = 5000;
    pricing.n_steps = 208;
    pricing.seed = 7;
    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"PV"};
    const EngineProduct* products[] = {product_5y.ptr, product_3y.ptr};
    CalcBatchResultsHandle many;
    int rc = engine_abi_calc_many(
        products, 2, names, 1, model.ptr, &market, &pricing, &execution, &many.entries, &many.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(many.count, 2u);
    EXPECT_EQ(many.entries[0].trade_index, 0u);
    EXPECT_EQ(many.entries[1].trade_index, 1u);
}

// PLAN.md §7.19: engine_abi_calc_grid explota Trades x Models x Markets.
TEST(Abi, CalcGridComputesTradesTimesModelsTimesMarkets) {
    ModelHandle model_1f = create_hull_white();
    ModelHandle model_2f = create_hull_white_2f();
    Irs5y irs_a{1'000'000.0, 0.02};
    Irs5y irs_b{2'000'000.0, 0.018};
    ProductHandle product_a = irs_a.create();
    ProductHandle product_b = irs_b.create();

    double pillar_a = 1.0, rate_a = 0.02;
    double pillar_b = 1.0, rate_b = 0.03;
    EngineMarketSnapshot market_a{};
    market_a.pillars = &pillar_a;
    market_a.zero_rates = &rate_a;
    market_a.count = 1;
    market_a.hazard_rate = 0.02;
    market_a.recovery_rate = 0.4;
    EngineMarketSnapshot market_b{};
    market_b.pillars = &pillar_b;
    market_b.zero_rates = &rate_b;
    market_b.count = 1;
    market_b.hazard_rate = 0.05;
    market_b.recovery_rate = 0.3;

    EnginePricingContext pricing{};
    pricing.n_paths = 5000;
    pricing.n_steps = 208;
    pricing.seed = 7;
    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"PV", "UnilateralCVA"};
    const EngineProduct* products[] = {product_a.ptr, product_b.ptr};
    const EngineModel* models[] = {model_1f.ptr, model_2f.ptr};
    EngineMarketSnapshot markets[] = {market_a, market_b};

    CalcGridResultsHandle grid;
    int rc = engine_abi_calc_grid(
        products, 2, names, 2, models, 2, markets, 2, &pricing, &execution, &grid.entries, &grid.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(grid.count, 8u); // 2 trades x 2 models x 2 markets

    for (std::size_t i = 0; i < grid.count; ++i) {
        const auto& cell = grid.entries[i];
        EXPECT_LT(cell.trade_index, 2u);
        EXPECT_LT(cell.model_index, 2u);
        EXPECT_LT(cell.market_index, 2u);
        const EngineCalcResultEntry* pv = find_measure(cell.measures, cell.n_measures, "PV");
        ASSERT_NE(pv, nullptr);
        EXPECT_TRUE(pv->result.has_scalar);
    }
}

TEST(Abi, CalcGridRejectsEmptyModelsOrMarkets) {
    ModelHandle model = create_hull_white();
    Irs5y irs{1'000'000.0, 0.02};
    ProductHandle product = irs.create();

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &rate;
    market.count = 1;
    EnginePricingContext pricing{};
    pricing.n_paths = 100;
    pricing.n_steps = 10;
    pricing.seed = 1;
    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    const char* names[] = {"PV"};
    const EngineProduct* products[] = {product.ptr};
    const EngineModel* models[] = {model.ptr};

    CalcGridResultsHandle grid;
    int rc = engine_abi_calc_grid(
        products, 1, names, 1, models, 0, &market, 1, &pricing, &execution, &grid.entries, &grid.count);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, IsGpuBackendAvailableIsBoolLike) {
    int available = engine_abi_is_gpu_backend_available();
    EXPECT_TRUE(available == 0 || available == 1);
}

namespace {

// EngineCalibrationResult::optimal_params llega como array desordenado (mismo orden que
// engine::Params, un unordered_map) -- busca por clave, igual que haría un consumidor real.
double find_param(const EngineCalibrationResult& result, const char* key) {
    for (std::size_t i = 0; i < result.n_params; ++i) {
        if (std::strcmp(result.optimal_params[i].key, key) == 0) return result.optimal_params[i].scalar;
    }
    ADD_FAILURE() << "clave no encontrada en optimal_params: " << key;
    return 0.0;
}

} // namespace

TEST(Abi, ListCalibratorsIncludesBothModels) {
    const char** names = nullptr;
    std::size_t count = engine_abi_list_calibrators(&names);
    std::vector<std::string> list(names, names + count);
    engine_abi_free_string_list(names, count);

    EXPECT_NE(std::find(list.begin(), list.end(), "HullWhite1F"), list.end());
    EXPECT_NE(std::find(list.begin(), list.end(), "HullWhite2F"), list.end());
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

    EngineCalibrator* calibrator = engine_abi_create_calibrator("HullWhite1F");
    ASSERT_NE(calibrator, nullptr) << last_error();

    EngineParam initial_guess[] = {
        scalar_param("a", 0.3), scalar_param("b", 0.01), scalar_param("sigma", sigma), scalar_param("r0", r0)
    };
    EngineCalibrationResult result{};
    int rc = engine_abi_calibrate(calibrator, &market, initial_guess, 4, &result);
    ASSERT_EQ(rc, 0) << last_error();

    EXPECT_TRUE(result.converged);
    EXPECT_NEAR(find_param(result, "a"), true_a, 1e-4);
    EXPECT_NEAR(find_param(result, "b"), true_b, 1e-4);
    EXPECT_EQ(find_param(result, "sigma"), sigma);
    EXPECT_EQ(find_param(result, "r0"), r0);

    // El resultado se pasa directamente a engine_abi_create_model, sin traducción -- cierra
    // el círculo Mercado -> calibrar -> Modelo calibrado (PLAN.md §7.14/§7.18).
    EngineModel* calibrated_model = engine_abi_create_model("HullWhite1F", result.optimal_params, result.n_params);
    EXPECT_NE(calibrated_model, nullptr) << last_error();
    engine_abi_free_model(calibrated_model);

    engine_abi_free_calibration_result(&result);
    engine_abi_free_calibrator(calibrator);
}

TEST(Abi, CalibrateHullWhite2FRecoversKnownParametersFromASyntheticMarket) {
    double pillars[] = {0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
    double true_a = 0.15, true_b = 0.25, sigma = 0.008, eta = 0.01, rho = -0.6, r0 = 0.02;
    double zero_rates[10];
    for (int i = 0; i < 10; ++i) {
        double price = engine::hull_white_2f_zero_coupon_bond(true_a, true_b, sigma, eta, rho, r0, pillars[i]);
        zero_rates[i] = -std::log(price) / pillars[i];
    }
    EngineMarketSnapshot market{};
    market.pillars = pillars;
    market.zero_rates = zero_rates;
    market.count = 10;

    EngineCalibrator* calibrator = engine_abi_create_calibrator("HullWhite2F");
    ASSERT_NE(calibrator, nullptr) << last_error();

    EngineParam initial_guess[] = {
        scalar_param("a", 0.4), scalar_param("b", 0.05), scalar_param("sigma", sigma),
        scalar_param("eta", eta), scalar_param("rho", rho), scalar_param("r0", r0)
    };
    EngineCalibrationResult result{};
    int rc = engine_abi_calibrate(calibrator, &market, initial_guess, 6, &result);
    ASSERT_EQ(rc, 0) << last_error();

    EXPECT_TRUE(result.converged);
    EXPECT_NEAR(find_param(result, "a"), true_a, 1e-4);
    EXPECT_NEAR(find_param(result, "b"), true_b, 1e-4);
    EXPECT_EQ(find_param(result, "sigma"), sigma);
    EXPECT_EQ(find_param(result, "eta"), eta);
    EXPECT_EQ(find_param(result, "rho"), rho);
    EXPECT_EQ(find_param(result, "r0"), r0);

    EngineModel* calibrated_model = engine_abi_create_model("HullWhite2F", result.optimal_params, result.n_params);
    EXPECT_NE(calibrated_model, nullptr) << last_error();
    engine_abi_free_model(calibrated_model);

    engine_abi_free_calibration_result(&result);
    engine_abi_free_calibrator(calibrator);
}

TEST(Abi, CalibrateRejectsNullMarket) {
    EngineCalibrator* calibrator = engine_abi_create_calibrator("HullWhite1F");
    ASSERT_NE(calibrator, nullptr) << last_error();

    EngineCalibrationResult result{};
    int rc = engine_abi_calibrate(calibrator, nullptr, nullptr, 0, &result);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(last_error().empty());

    engine_abi_free_calibrator(calibrator);
}

TEST(Abi, CreateUnknownCalibratorReturnsNull) {
    EngineCalibrator* calibrator = engine_abi_create_calibrator("NoExiste");
    EXPECT_EQ(calibrator, nullptr);
    EXPECT_FALSE(last_error().empty());
}
