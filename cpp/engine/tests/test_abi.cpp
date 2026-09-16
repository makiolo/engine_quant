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

// PLAN_PRODUCTS.md Fase 3: unico consumidor hoy es engine_abi_create_product("Payoff", ...).
EngineParam string_param(const char* key, const char* value) {
    EngineParam p{};
    p.key = key;
    p.kind = ENGINE_PARAM_STRING;
    p.string_value = value;
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
struct PriceResultsHandle {
    EnginePriceResultEntry* entries = nullptr;
    std::size_t count = 0;
    ~PriceResultsHandle() { engine_abi_free_price_results(entries, count); }

    const EnginePriceResultEntry* find(const char* name) const {
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

// Con fixed_rate explícito (PLAN.md §7.19): engine_abi_price_batch/_many no soportan
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

struct PriceBatchResultsHandle {
    EnginePriceBatchResultEntry* entries = nullptr;
    std::size_t count = 0;
    ~PriceBatchResultsHandle() { engine_abi_free_price_batch_results(entries, count); }
};

struct PriceGridResultsHandle {
    EnginePriceGridResultEntry* entries = nullptr;
    std::size_t count = 0;
    ~PriceGridResultsHandle() { engine_abi_free_price_grid_results(entries, count); }
};

const EnginePriceResultEntry* find_measure(const EnginePriceResultEntry* entries, std::size_t count, const char* name) {
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(entries[i].measure_name, name) == 0) return &entries[i];
    }
    return nullptr;
}

// PLAN_BACKWARD.md §9 Fase 6: RAII para EnginePortfolio y para los dos structs de salida que
// engine_abi_portfolio_hessian/_hvp comparten con engine_abi_hessian/_hvp de trade unico
// (EngineHessianEntry/EngineHvpComponent) -- mismo patron que ModelHandle/ProductHandle de
// arriba.
struct PortfolioHandle {
    EnginePortfolio* ptr;
    ~PortfolioHandle() { engine_abi_free_portfolio(ptr); }
};

struct HessianResultsHandle {
    EngineHessianEntry* entries = nullptr;
    std::size_t n_entries = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    ~HessianResultsHandle() { engine_abi_free_hessian(entries, n_entries, skipped, n_skipped); }

    const EngineHessianEntry* find(const std::string& i, const std::string& j) const {
        for (std::size_t k = 0; k < n_entries; ++k) {
            if ((entries[k].risk_factor_i == i && entries[k].risk_factor_j == j) ||
                (entries[k].risk_factor_i == j && entries[k].risk_factor_j == i)) {
                return &entries[k];
            }
        }
        return nullptr;
    }
};

struct HvpResultsHandle {
    EngineHvpComponent* components = nullptr;
    std::size_t n_components = 0;
    char** skipped = nullptr;
    std::size_t n_skipped = 0;
    ~HvpResultsHandle() { engine_abi_free_hvp(components, n_components, skipped, n_skipped); }

    const EngineHvpComponent* find(const std::string& factor) const {
        for (std::size_t k = 0; k < n_components; ++k) {
            if (components[k].risk_factor == factor) return &components[k];
        }
        return nullptr;
    }
};

} // namespace

TEST(Abi, VersionIsAtLeastTwo) {
    // Version 2 (PLAN.md §7.15): engine_abi_price sustituye a engine_abi_create_measure/
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

TEST(Abi, ListMeasuresIncludesTheFivePriceNames) {
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
    EnginePriceResultEntry* entries = nullptr;
    std::size_t count = 0;
    int rc = engine_abi_price(nullptr, names, 1, nullptr, nullptr, nullptr, nullptr, &entries, &count);
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
    EnginePriceResultEntry* entries = nullptr;
    std::size_t count = 0;
    int rc = engine_abi_price(
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
    PriceResultsHandle results;
    int rc = engine_abi_price(
        product.ptr, names, 5, model.ptr, &market, &pricing, &execution, &results.entries, &results.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(results.count, 5u);

    const EnginePriceResultEntry* pv = results.find("PV");
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->result.has_scalar);
    EXPECT_NEAR(pv->result.scalar, 0.0, 1e-6); // swap a la par: NPV ~ 0 en start

    const EnginePriceResultEntry* dv01 = results.find("DV01");
    ASSERT_NE(dv01, nullptr);
    EXPECT_TRUE(dv01->result.has_scalar);
    EXPECT_GT(dv01->result.scalar, 0.0); // swap pagador: sube de valor cuando suben los tipos

    // ExpectedExposure/PFE95 comparten una sola simulación (PLAN.md §7.15): 5 fechas de
    // reseteo auto-derivadas del swap (0,1,2,3,4), no una lista pedida a mano.
    const EnginePriceResultEntry* ee = results.find("ExpectedExposure");
    ASSERT_NE(ee, nullptr);
    ASSERT_EQ(ee->result.len, 5u);
    const EnginePriceResultEntry* pfe = results.find("PFE95");
    ASSERT_NE(pfe, nullptr);
    ASSERT_EQ(pfe->result.len, 5u);
    for (std::size_t i = 0; i < ee->result.len; ++i) {
        EXPECT_GE(ee->result.primary[i], 0.0);
        EXPECT_GE(pfe->result.primary[i], ee->result.primary[i]);
    }

    const EnginePriceResultEntry* cva = results.find("UnilateralCVA");
    ASSERT_NE(cva, nullptr);
    EXPECT_TRUE(cva->result.has_scalar);
    EXPECT_GT(cva->result.scalar, 0.0);
}

// Interfaz homogénea (PLAN.md §7.16) llevada hasta la ABI en C: mismo engine_abi_price, mismo
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
    PriceResultsHandle results;
    int rc = engine_abi_price(
        product.ptr, names, 5, model.ptr, &market, &pricing, &execution, &results.entries, &results.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(results.count, 5u);

    const EnginePriceResultEntry* pv = results.find("PV");
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->result.has_scalar);
    EXPECT_NEAR(pv->result.scalar, 0.0, 1e-6);

    const EnginePriceResultEntry* dv01 = results.find("DV01");
    ASSERT_NE(dv01, nullptr);
    EXPECT_TRUE(dv01->result.has_scalar);
    EXPECT_GT(dv01->result.scalar, 0.0);

    const EnginePriceResultEntry* ee = results.find("ExpectedExposure");
    ASSERT_NE(ee, nullptr);
    ASSERT_EQ(ee->result.len, 5u);
    const EnginePriceResultEntry* pfe = results.find("PFE95");
    ASSERT_NE(pfe, nullptr);
    ASSERT_EQ(pfe->result.len, 5u);
    for (std::size_t i = 0; i < ee->result.len; ++i) {
        EXPECT_GE(ee->result.primary[i], 0.0);
        EXPECT_GE(pfe->result.primary[i], ee->result.primary[i]);
    }

    const EnginePriceResultEntry* cva = results.find("UnilateralCVA");
    ASSERT_NE(cva, nullptr);
    EXPECT_TRUE(cva->result.has_scalar);
    EXPECT_GT(cva->result.scalar, 0.0);
}

// PLAN.md §7.19: engine_abi_price_batch (lote homogéneo) debe coincidir, trade a trade, con
// llamar a engine_abi_price una vez por trade.
TEST(Abi, PriceBatchMatchesALoopOfScalarCallsPerTrade) {
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

    PriceBatchResultsHandle batch;
    int rc = engine_abi_price_batch(
        products, 3, names, 5, model.ptr, &market, &pricing, &execution, &batch.entries, &batch.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(batch.count, 3u);

    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(batch.entries[i].trade_index, i);
        PriceResultsHandle scalar;
        int scalar_rc = engine_abi_price(
            products[i], names, 5, model.ptr, &market, &pricing, &execution, &scalar.entries, &scalar.count);
        ASSERT_EQ(scalar_rc, 0) << last_error();

        for (const char* name : names) {
            const EnginePriceResultEntry* from_batch = find_measure(batch.entries[i].measures, batch.entries[i].n_measures, name);
            const EnginePriceResultEntry* from_scalar = find_measure(scalar.entries, scalar.count, name);
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

TEST(Abi, PriceBatchRejectsMismatchedCalendars) {
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
    PriceBatchResultsHandle batch;
    int rc = engine_abi_price_batch(
        products, 2, names, 1, model.ptr, &market, &pricing, &execution, &batch.entries, &batch.count);
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(last_error().empty());
}

// PLAN.md §7.19, Nivel 2: engine_abi_price_many agrupa internamente por calendario y nunca
// falla por heterogeneidad (a diferencia de engine_abi_price_batch en el test anterior).
TEST(Abi, PriceManyGroupsHeterogeneousCalendars) {
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
    PriceBatchResultsHandle many;
    int rc = engine_abi_price_many(
        products, 2, names, 1, model.ptr, &market, &pricing, &execution, &many.entries, &many.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(many.count, 2u);
    EXPECT_EQ(many.entries[0].trade_index, 0u);
    EXPECT_EQ(many.entries[1].trade_index, 1u);
}

// PLAN.md §7.19: engine_abi_price_grid explota Trades x Models x Markets.
TEST(Abi, PriceGridComputesTradesTimesModelsTimesMarkets) {
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

    PriceGridResultsHandle grid;
    int rc = engine_abi_price_grid(
        products, 2, names, 2, models, 2, markets, 2, &pricing, &execution, &grid.entries, &grid.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(grid.count, 8u); // 2 trades x 2 models x 2 markets

    for (std::size_t i = 0; i < grid.count; ++i) {
        const auto& cell = grid.entries[i];
        EXPECT_LT(cell.trade_index, 2u);
        EXPECT_LT(cell.model_index, 2u);
        EXPECT_LT(cell.market_index, 2u);
        const EnginePriceResultEntry* pv = find_measure(cell.measures, cell.n_measures, "PV");
        ASSERT_NE(pv, nullptr);
        EXPECT_TRUE(pv->result.has_scalar);
    }
}

TEST(Abi, PriceGridRejectsEmptyModelsOrMarkets) {
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

    PriceGridResultsHandle grid;
    int rc = engine_abi_price_grid(
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

// PLAN_PRODUCTS.md Fase 3 (SS12): ENGINE_PARAM_STRING + engine_abi_create_product("Payoff",
// ...) -- confirma que el tercer consumidor generico (tras Python/Excel) tambien funciona.
TEST(Abi, CreateProductPayoffFromJsonSpec) {
    std::string spec = R"({"schema":"engine.payoff/v1","id":"AAPL_CALL_100","contract":{)"
                        R"("type":"when","time":1.0,"child":{)"
                        R"("type":"cashflow","currency":"USD","amount":{)"
                        R"("type":"constant","value":30000.0)"
                        R"(}}}})";
    EngineParam params[] = {string_param("spec", spec.c_str())};
    ProductHandle product{engine_abi_create_product("Payoff", params, 1)};
    ASSERT_NE(product.ptr, nullptr) << last_error();
}

TEST(Abi, CreateProductPayoffRejectsUnknownSchemaVersion) {
    std::string spec = R"({"schema":"engine.payoff/v2","id":"X","contract":{"type":"zero"}})";
    EngineParam params[] = {string_param("spec", spec.c_str())};
    ProductHandle product{engine_abi_create_product("Payoff", params, 1)};
    EXPECT_EQ(product.ptr, nullptr);
    EXPECT_FALSE(last_error().empty());
}

// PLAN_PRODUCTS.md Fase 10 (SS7.1): validate/explain sin registry, misma superficie que
// nanobind (engine.validate_payoff_spec/Product.explain) y Excel (ENGINE.VALIDATE_PAYOFF_SPEC/
// ENGINE.EXPLAIN_PRODUCT).

TEST(Abi, ValidatePayoffSpecReturnsZeroForValidSpec) {
    std::string spec = R"({"schema":"engine.payoff/v1","id":"X","contract":{)"
                        R"("type":"when","time":1.0,"child":{)"
                        R"("type":"cashflow","currency":"USD","amount":{"type":"constant","value":1.0})"
                        R"(}}})";
    EXPECT_EQ(engine_abi_validate_payoff_spec(spec.c_str()), 0);
}

TEST(Abi, ValidatePayoffSpecReportsAggregatedErrorsWithoutCreatingProduct) {
    std::string spec = R"({"schema":"engine.payoff/v1","id":"BAD","contract":{)"
                        R"("type":"both","children":[)"
                        R"({"type":"cashflow","currency":"USD","amount":{"type":"constant","value":1.0}},)"
                        R"({"type":"cashflow","currency":"USD","amount":{"type":"constant","value":2.0}})"
                        R"(]}})";
    EXPECT_NE(engine_abi_validate_payoff_spec(spec.c_str()), 0);
    std::string error = last_error();
    EXPECT_NE(error.find("children[0]"), std::string::npos);
    EXPECT_NE(error.find("children[1]"), std::string::npos);
}

TEST(Abi, ValidatePayoffSpecRejectsNullSpec) {
    EXPECT_NE(engine_abi_validate_payoff_spec(nullptr), 0);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, ExplainProductReturnsTreeForPayoffProduct) {
    std::string spec = R"({"schema":"engine.payoff/v1","id":"AAPL_CALL_100","contract":{)"
                        R"("type":"when","time":1.0,"child":{)"
                        R"("type":"cashflow","currency":"USD","amount":{"type":"constant","value":30000.0})"
                        R"(}}})";
    EngineParam params[] = {string_param("spec", spec.c_str())};
    ProductHandle product{engine_abi_create_product("Payoff", params, 1)};
    ASSERT_NE(product.ptr, nullptr) << last_error();

    std::size_t len = engine_abi_explain_product(product.ptr, nullptr, 0);
    std::string text(len, '\0');
    engine_abi_explain_product(product.ptr, text.data(), len + 1);

    EXPECT_NE(text.find("AAPL_CALL_100"), std::string::npos);
    EXPECT_NE(text.find("When"), std::string::npos);
}

TEST(Abi, ExplainProductDefaultsToTypeNameForLegacyProduct) {
    EngineParam params[] = {
        scalar_param("notional", 1'000'000.0),
        vector_param("payment_times", {1.0}),
        vector_param("accruals", {1.0}),
        scalar_param("fixed_rate", 0.03),
    };
    ProductHandle irs{engine_abi_create_product("IRSwap", params, 4)};
    ASSERT_NE(irs.ptr, nullptr) << last_error();

    std::size_t len = engine_abi_explain_product(irs.ptr, nullptr, 0);
    std::string text(len, '\0');
    engine_abi_explain_product(irs.ptr, text.data(), len + 1);
    EXPECT_EQ(text, "IRSwap");
}

TEST(Abi, ExplainProductRejectsNullProduct) {
    char buffer[16];
    std::size_t len = engine_abi_explain_product(nullptr, buffer, sizeof(buffer));
    EXPECT_EQ(len, 0u);
    EXPECT_FALSE(last_error().empty());
}

// --- ENGINE.PORTFOLIO (PLAN_BACKWARD.md §6.4/§9 Fase 6) -------------------------------------
// Mismo criterio "trade a trade, oraculo independiente" que el test C++ de aceptacion
// (cpp/engine/tests/test_portfolio.cpp), pero pasando exclusivamente por la superficie extern
// "C" de abi.h -- confirma que engine_abi_create_portfolio/_add_trade/_size/_free y
// engine_abi_portfolio_price/_hessian/_hvp traducen igual que engine::Portfolio subyacente.

TEST(Abi, PortfolioStartsEmptyAndSizeGrowsOneByOne) {
    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    ASSERT_NE(portfolio.ptr, nullptr);
    EXPECT_EQ(engine_abi_portfolio_size(portfolio.ptr), 0u);

    Irs5y irs_a{1'000'000.0, 0.02};
    Irs5y irs_b{2'000'000.0, 0.025};
    ProductHandle product_a = irs_a.create();
    ProductHandle product_b = irs_b.create();
    ASSERT_NE(product_a.ptr, nullptr) << last_error();
    ASSERT_NE(product_b.ptr, nullptr) << last_error();

    engine_abi_portfolio_add_trade(portfolio.ptr, product_a.ptr);
    EXPECT_EQ(engine_abi_portfolio_size(portfolio.ptr), 1u);
    engine_abi_portfolio_add_trade(portfolio.ptr, product_b.ptr);
    EXPECT_EQ(engine_abi_portfolio_size(portfolio.ptr), 2u);
}

TEST(Abi, PortfolioAddTradeRejectsNullHandlesWithoutCrashing) {
    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    ASSERT_NE(portfolio.ptr, nullptr);

    engine_abi_portfolio_add_trade(nullptr, nullptr);
    EXPECT_FALSE(last_error().empty());
    engine_abi_portfolio_add_trade(portfolio.ptr, nullptr);
    EXPECT_FALSE(last_error().empty());
    EXPECT_EQ(engine_abi_portfolio_size(portfolio.ptr), 0u);
    EXPECT_EQ(engine_abi_portfolio_size(nullptr), 0u);
}

// Ownership (PLAN_BACKWARD.md §9 Fase 6): anadir un EngineProduct a un EnginePortfolio no le
// roba la propiedad al handle original -- sigue siendo valido para engine_abi_price directamente
// DESPUES de anadirlo a un Portfolio, y liberar el EngineProduct original con
// engine_abi_free_product NO invalida al Portfolio (shared_ptr, no unique_ptr -- ver el
// comentario de EngineProduct en abi.cpp).
TEST(Abi, ProductSurvivesInsidePortfolioAfterItsOwnHandleIsFreed) {
    ModelHandle model = create_hull_white();
    Irs5y irs{1'000'000.0, 0.02};

    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    ASSERT_NE(portfolio.ptr, nullptr);
    {
        ProductHandle product = irs.create();
        ASSERT_NE(product.ptr, nullptr) << last_error();
        engine_abi_portfolio_add_trade(portfolio.ptr, product.ptr);
        EXPECT_EQ(engine_abi_portfolio_size(portfolio.ptr), 1u);
        // `product` sale de scope aqui y se libera (~ProductHandle -> engine_abi_free_product):
        // el trade dentro de `portfolio` debe seguir siendo valido despues.
    }

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{&pillar, &rate, 1, 0.0, 0.0};
    EnginePricingContext pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext execution{"cpu", "fp64"};
    const char* names[] = {"PV"};

    PriceBatchResultsHandle batch;
    int rc = engine_abi_portfolio_price(portfolio.ptr, names, 1, model.ptr, &market, &pricing, &execution, &batch.entries, &batch.count);
    ASSERT_EQ(rc, 0) << last_error();
    ASSERT_EQ(batch.count, 1u);
    const EnginePriceResultEntry* pv = find_measure(batch.entries[0].measures, batch.entries[0].n_measures, "PV");
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->result.has_scalar);
}

// engine_abi_portfolio_price es un envoltorio fino sobre Portfolio::price (-> price_many): debe
// coincidir EXACTAMENTE con engine_abi_price_batch sobre el mismo vector de trades (mismo caso
// que Abi.PriceBatchMatchesALoopOfScalarCallsPerTrade de arriba, pero via Portfolio).
TEST(Abi, PortfolioPriceMatchesPriceBatchOnTheSameTrades) {
    ModelHandle model = create_hull_white();
    Irs5y irs_a{1'000'000.0, 0.02};
    Irs5y irs_b{2'500'000.0, 0.015};
    Irs5y irs_c{500'000.0, 0.025};
    ProductHandle product_a = irs_a.create();
    ProductHandle product_b = irs_b.create();
    ProductHandle product_c = irs_c.create();
    ASSERT_NE(product_a.ptr, nullptr) << last_error();
    ASSERT_NE(product_b.ptr, nullptr) << last_error();
    ASSERT_NE(product_c.ptr, nullptr) << last_error();

    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    engine_abi_portfolio_add_trade(portfolio.ptr, product_a.ptr);
    engine_abi_portfolio_add_trade(portfolio.ptr, product_b.ptr);
    engine_abi_portfolio_add_trade(portfolio.ptr, product_c.ptr);
    ASSERT_EQ(engine_abi_portfolio_size(portfolio.ptr), 3u);

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{&pillar, &rate, 1, 0.0, 0.0};
    EnginePricingContext pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext execution{"cpu", "fp64"};
    const char* names[] = {"PV", "DV01"};
    const EngineProduct* products[] = {product_a.ptr, product_b.ptr, product_c.ptr};

    PriceBatchResultsHandle from_portfolio;
    int rc_portfolio = engine_abi_portfolio_price(
        portfolio.ptr, names, 2, model.ptr, &market, &pricing, &execution, &from_portfolio.entries, &from_portfolio.count
    );
    ASSERT_EQ(rc_portfolio, 0) << last_error();

    PriceBatchResultsHandle from_batch;
    int rc_batch = engine_abi_price_batch(
        products, 3, names, 2, model.ptr, &market, &pricing, &execution, &from_batch.entries, &from_batch.count
    );
    ASSERT_EQ(rc_batch, 0) << last_error();

    ASSERT_EQ(from_portfolio.count, from_batch.count);
    for (std::size_t i = 0; i < from_portfolio.count; ++i) {
        EXPECT_EQ(from_portfolio.entries[i].trade_index, from_batch.entries[i].trade_index);
        for (const char* name : names) {
            const EnginePriceResultEntry* a = find_measure(from_portfolio.entries[i].measures, from_portfolio.entries[i].n_measures, name);
            const EnginePriceResultEntry* b = find_measure(from_batch.entries[i].measures, from_batch.entries[i].n_measures, name);
            ASSERT_NE(a, nullptr);
            ASSERT_NE(b, nullptr);
            EXPECT_DOUBLE_EQ(a->result.scalar, b->result.scalar) << name;
        }
    }
}

TEST(Abi, PortfolioPriceRejectsNullHandles) {
    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{&pillar, &rate, 1, 0.0, 0.0};
    EnginePricingContext pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext execution{"cpu", "fp64"};
    const char* names[] = {"PV"};

    PriceBatchResultsHandle batch;
    int rc = engine_abi_portfolio_price(nullptr, names, 1, nullptr, &market, &pricing, &execution, &batch.entries, &batch.count);
    EXPECT_NE(rc, 0);
    EXPECT_EQ(batch.entries, nullptr);
    EXPECT_EQ(batch.count, 0u);
    EXPECT_FALSE(last_error().empty());
}

// Aceptacion EXACTA del plan (§13 DoD), via ABI: Portfolio::hessian() de 3 IrSwapProduct bajo el
// MISMO HullWhite1FModel coincide, entrada a entrada, con sumar A MANO los EngineHessianEntry de
// engine_abi_hessian llamado trade a trade -- identidad exacta.
TEST(Abi, PortfolioHessianMatchesManualSumOfPerTradeAbiHessianExactly) {
    ModelHandle model = create_hull_white();
    Irs5y irs_a{1'000'000.0, 0.02};
    Irs5y irs_b{2'500'000.0, 0.015};
    Irs5y irs_c{500'000.0, 0.025};
    ProductHandle product_a = irs_a.create();
    ProductHandle product_b = irs_b.create();
    ProductHandle product_c = irs_c.create();
    const EngineProduct* products[] = {product_a.ptr, product_b.ptr, product_c.ptr};

    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    for (const EngineProduct* p : products) engine_abi_portfolio_add_trade(portfolio.ptr, p);
    ASSERT_EQ(engine_abi_portfolio_size(portfolio.ptr), 3u);

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{&pillar, &rate, 1, 0.0, 0.0};
    EnginePricingContext pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext execution{"cpu", "fp64"};

    HessianResultsHandle portfolio_hessian;
    int rc = engine_abi_portfolio_hessian(
        portfolio.ptr, "HullWhiteModelNpv", nullptr, 0, model.ptr, &market, &pricing, &execution, nullptr, 0,
        &portfolio_hessian.entries, &portfolio_hessian.n_entries, &portfolio_hessian.skipped, &portfolio_hessian.n_skipped
    );
    ASSERT_EQ(rc, 0) << last_error();
    EXPECT_EQ(portfolio_hessian.n_skipped, 0u);
    ASSERT_EQ(portfolio_hessian.n_entries, 10u);

    // Oraculo: engine_abi_hessian llamado a mano, trade a trade, sumado manualmente.
    HessianResultsHandle per_trade[3];
    for (std::size_t k = 0; k < 3; ++k) {
        int rc_trade = engine_abi_hessian(
            products[k], "HullWhiteModelNpv", nullptr, 0, model.ptr, &market, &pricing, &execution, nullptr, 0,
            &per_trade[k].entries, &per_trade[k].n_entries, &per_trade[k].skipped, &per_trade[k].n_skipped
        );
        ASSERT_EQ(rc_trade, 0) << last_error();
        ASSERT_EQ(per_trade[k].n_skipped, 0u);
        ASSERT_EQ(per_trade[k].n_entries, 10u);
    }

    for (std::size_t i = 0; i < portfolio_hessian.n_entries; ++i) {
        const EngineHessianEntry& entry = portfolio_hessian.entries[i];
        double manual_sum = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            const EngineHessianEntry* found = per_trade[k].find(entry.risk_factor_i, entry.risk_factor_j);
            ASSERT_NE(found, nullptr) << entry.risk_factor_i << "/" << entry.risk_factor_j << " trade " << k;
            manual_sum += found->value;
        }
        EXPECT_NEAR(entry.value, manual_sum, 1e-9 * std::max(1.0, std::abs(manual_sum)))
            << entry.risk_factor_i << "/" << entry.risk_factor_j;
        // Hull-White (forward-over-forward) es formula cerrada: nunca lleva std_error.
        EXPECT_EQ(entry.has_std_error, 0);
    }
}

// Mismo criterio de aceptacion, para engine_abi_portfolio_hvp.
TEST(Abi, PortfolioHvpMatchesManualSumOfPerTradeAbiHvpExactly) {
    ModelHandle model = create_hull_white();
    Irs5y irs_a{1'000'000.0, 0.02};
    Irs5y irs_b{2'500'000.0, 0.015};
    Irs5y irs_c{500'000.0, 0.025};
    ProductHandle product_a = irs_a.create();
    ProductHandle product_b = irs_b.create();
    ProductHandle product_c = irs_c.create();
    const EngineProduct* products[] = {product_a.ptr, product_b.ptr, product_c.ptr};

    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    for (const EngineProduct* p : products) engine_abi_portfolio_add_trade(portfolio.ptr, p);

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{&pillar, &rate, 1, 0.0, 0.0};
    EnginePricingContext pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext execution{"cpu", "fp64"};

    const char* direction_factors[] = {"model.a", "model.b", "model.sigma", "model.r0"};
    const double direction_weights[] = {1.0, 0.5, -0.25, 2.0};

    HvpResultsHandle portfolio_hvp;
    int rc = engine_abi_portfolio_hvp(
        portfolio.ptr, "HullWhiteModelNpv", nullptr, 0, model.ptr, &market, &pricing, &execution, direction_factors,
        direction_weights, 4, &portfolio_hvp.components, &portfolio_hvp.n_components, &portfolio_hvp.skipped,
        &portfolio_hvp.n_skipped
    );
    ASSERT_EQ(rc, 0) << last_error();
    EXPECT_EQ(portfolio_hvp.n_skipped, 0u);
    ASSERT_EQ(portfolio_hvp.n_components, 4u);

    HvpResultsHandle per_trade[3];
    for (std::size_t k = 0; k < 3; ++k) {
        int rc_trade = engine_abi_hvp(
            products[k], "HullWhiteModelNpv", nullptr, 0, model.ptr, &market, &pricing, &execution, direction_factors,
            direction_weights, 4, &per_trade[k].components, &per_trade[k].n_components, &per_trade[k].skipped,
            &per_trade[k].n_skipped
        );
        ASSERT_EQ(rc_trade, 0) << last_error();
        ASSERT_EQ(per_trade[k].n_components, 4u);
    }

    for (std::size_t i = 0; i < portfolio_hvp.n_components; ++i) {
        const EngineHvpComponent& component = portfolio_hvp.components[i];
        double manual_sum = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            const EngineHvpComponent* found = per_trade[k].find(component.risk_factor);
            ASSERT_NE(found, nullptr) << component.risk_factor << " trade " << k;
            manual_sum += found->value;
        }
        EXPECT_NEAR(component.value, manual_sum, 1e-9 * std::max(1.0, std::abs(manual_sum))) << component.risk_factor;
    }
}

TEST(Abi, PortfolioHessianRejectsNullHandles) {
    HessianResultsHandle result;
    int rc = engine_abi_portfolio_hessian(
        nullptr, "HullWhiteModelNpv", nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
        &result.entries, &result.n_entries, &result.skipped, &result.n_skipped
    );
    EXPECT_NE(rc, 0);
    EXPECT_EQ(result.entries, nullptr);
    EXPECT_EQ(result.n_entries, 0u);
    EXPECT_FALSE(last_error().empty());
}

TEST(Abi, PortfolioHvpRejectsMissingDirection) {
    ModelHandle model = create_hull_white();
    PortfolioHandle portfolio{engine_abi_create_portfolio()};
    Irs5y irs{1'000'000.0, 0.02};
    ProductHandle product = irs.create();
    engine_abi_portfolio_add_trade(portfolio.ptr, product.ptr);

    double pillar = 1.0, rate = 0.02;
    EngineMarketSnapshot market{&pillar, &rate, 1, 0.0, 0.0};
    EnginePricingContext pricing{0.0, 1'000, 1, 7};
    EngineExecutionContext execution{"cpu", "fp64"};

    HvpResultsHandle result;
    int rc = engine_abi_portfolio_hvp(
        portfolio.ptr, "HullWhiteModelNpv", nullptr, 0, model.ptr, &market, &pricing, &execution, nullptr, nullptr, 0,
        &result.components, &result.n_components, &result.skipped, &result.n_skipped
    );
    EXPECT_NE(rc, 0);
    EXPECT_FALSE(last_error().empty());
}
