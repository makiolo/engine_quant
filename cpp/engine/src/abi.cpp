// Implementación de la API universal en C ABI (PLAN.md Fase 6/§7.15, ver engine/abi.h para
// el contrato). Traduce entre los tipos C planos del header y engine::Registries/Registry<T>/
// engine::price ya existentes (PLAN.md §5.4, §7.15) -- no reimplementa lógica de negocio, solo
// la frontera: construir/leer engine::Params desde EngineParam[], convertir
// engine::MarketSnapshot/PricingContext/ExecutionContext desde sus structs C planos, convertir
// engine::PriceResult a EnginePriceResultEntry[] con arrays owned por esta librería, y atrapar
// toda excepción de C++ (ninguna puede cruzar a un lenguaje sin soporte de excepciones, PLAN.md
// §5.5).

#include "engine/abi.h"

#include "engine/price.hpp"
#include "engine/engine.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// Registro único de proceso (igual patrón que xlbridge::shared() en clients/excel/src/
// handles.hpp): register_builtins corre una sola vez, en la primera llamada que lo necesite.
engine::Registries& registries() {
    static engine::Registries instance = [] {
        engine::Registries r;
        engine::register_builtins(r);
        return r;
    }();
    return instance;
}

// Última excepción atrapada en este hilo (engine_abi_last_error, PLAN.md §5.5: "ninguna
// excepción cruza esta frontera"). thread_local porque distintos hilos consumidores no deben
// pisarse el error entre sí.
thread_local std::string g_last_error;

void set_last_error(const std::exception& e) { g_last_error = e.what(); }
void clear_last_error() { g_last_error.clear(); }

// Copia `src` a `buffer` (hasta buffer_len-1 caracteres + NUL) y devuelve strlen(src), misma
// convención que snprintf: usada por engine_abi_last_error.
std::size_t copy_to_buffer(const std::string& src, char* buffer, std::size_t buffer_len) {
    if (buffer && buffer_len > 0) {
        std::size_t to_copy = std::min(src.size(), buffer_len - 1);
        std::memcpy(buffer, src.data(), to_copy);
        buffer[to_copy] = '\0';
    }
    return src.size();
}

engine::Params to_params(const EngineParam* params, std::size_t n_params) {
    engine::Params result;
    for (std::size_t i = 0; i < n_params; ++i) {
        const EngineParam& p = params[i];
        switch (p.kind) {
            case ENGINE_PARAM_DOUBLE:
                result.emplace(p.key, p.scalar);
                break;
            case ENGINE_PARAM_BOOL:
                result.emplace(p.key, p.scalar != 0.0);
                break;
            case ENGINE_PARAM_VECTOR:
                result.emplace(p.key, std::vector<double>(p.values, p.values + p.count));
                break;
            case ENGINE_PARAM_STRING:
                result.emplace(p.key, std::string(p.string_value ? p.string_value : ""));
                break;
        }
    }
    return result;
}

// Inversa de to_params (PLAN.md §7.18): convierte un engine::Params (salida de
// ICalibrator::calibrate) a un array de EngineParam owned por esta libreria, mismo bag que ya
// consume engine_abi_create_model -- para que EngineCalibrationResult::optimal_params se
// pueda pasar directamente ahi sin traduccion adicional del lado del consumidor.
EngineParam* export_params(const engine::Params& params, std::size_t* out_count) {
    if (params.empty()) {
        *out_count = 0;
        return nullptr;
    }
    auto* array = new EngineParam[params.size()]{};
    std::size_t i = 0;
    for (const auto& [key, value] : params) {
        char* key_copy = new char[key.size() + 1];
        std::memcpy(key_copy, key.data(), key.size() + 1);
        array[i].key = key_copy;

        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, double>) {
                    array[i].kind = ENGINE_PARAM_DOUBLE;
                    array[i].scalar = v;
                } else if constexpr (std::is_same_v<T, bool>) {
                    array[i].kind = ENGINE_PARAM_BOOL;
                    array[i].scalar = v ? 1.0 : 0.0;
                } else if constexpr (std::is_same_v<T, std::vector<double>>) {
                    array[i].kind = ENGINE_PARAM_VECTOR;
                    array[i].count = v.size();
                    if (!v.empty()) {
                        auto* values_copy = new double[v.size()];
                        std::memcpy(values_copy, v.data(), v.size() * sizeof(double));
                        array[i].values = values_copy;
                    }
                } else {
                    // std::string: sin representacion en EngineParam (PLAN.md §5.5: solo
                    // double/vector<double>/bool) -- ningun ICalibrator produce hoy un
                    // optimal_params con claves de texto (ExecutionContext es el unico
                    // consumidor de "backend"/"precision", ajeno a calibracion).
                    array[i].kind = ENGINE_PARAM_DOUBLE;
                    array[i].scalar = 0.0;
                }
            },
            value
        );
        ++i;
    }
    *out_count = params.size();
    return array;
}

// Convierte un engine::PriceResult (un trade) a un array EnginePriceResultEntry owned por esta
// libreria -- extraido de engine_abi_price para reutilizarlo tal cual en engine_abi_price_batch/
// _many/_grid (PLAN.md §7.19), donde el mismo PriceResult se repite una vez por fila.
EnginePriceResultEntry* export_calc_result(const engine::PriceResult& result) {
    auto* entries = new EnginePriceResultEntry[result.size()]{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const engine::PriceResultEntry& src = result[i];

        char* name_copy = new char[src.measure_name.size() + 1];
        std::memcpy(name_copy, src.measure_name.data(), src.measure_name.size() + 1);
        entries[i].measure_name = name_copy;

        EngineMeasureResult& mr = entries[i].result;
        mr.len = src.result.times.size();
        if (mr.len > 0 && src.result.times.size() == mr.len) {
            mr.times = new double[mr.len];
            std::memcpy(mr.times, src.result.times.data(), mr.len * sizeof(double));
        }
        if (mr.len > 0 && src.result.primary.size() == mr.len) {
            mr.primary = new double[mr.len];
            std::memcpy(mr.primary, src.result.primary.data(), mr.len * sizeof(double));
        }
        if (mr.len > 0 && src.result.secondary.size() == mr.len) {
            mr.secondary = new double[mr.len];
            std::memcpy(mr.secondary, src.result.secondary.data(), mr.len * sizeof(double));
        }
        mr.has_scalar = src.result.has_scalar ? 1 : 0;
        mr.scalar = src.result.scalar;
    }
    return entries;
}

std::vector<std::string> to_string_vector(const char* const* names, std::size_t count) {
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.emplace_back(names[i]);
    return out;
}

engine::MarketSnapshot to_market(const EngineMarketSnapshot& m) {
    return engine::MarketSnapshot(
        std::vector<double>(m.pillars, m.pillars + m.count),
        std::vector<double>(m.zero_rates, m.zero_rates + m.count),
        m.hazard_rate, m.recovery_rate
    );
}

std::vector<engine::MarketSnapshot> to_market_vector(const EngineMarketSnapshot* markets, std::size_t n_markets) {
    std::vector<engine::MarketSnapshot> out;
    out.reserve(n_markets);
    for (std::size_t i = 0; i < n_markets; ++i) out.push_back(to_market(markets[i]));
    return out;
}

engine::PricingContext to_pricing_context(const EnginePricingContext& p) {
    engine::Params params{
        {"pricing_date", p.pricing_date},
        {"n_paths", static_cast<double>(p.n_paths)},
        {"n_steps", static_cast<double>(p.n_steps)},
        {"seed", static_cast<double>(p.seed)},
    };
    return engine::PricingContext(params);
}

engine::ExecutionContext to_execution_context(const EngineExecutionContext& e) {
    engine::Params params{
        {"backend", std::string(e.backend ? e.backend : "")},
        {"precision", std::string(e.precision ? e.precision : "fp64")},
    };
    return engine::ExecutionContext(params);
}

// engine_abi_list_models/products/measures comparten esta implementación: copia `names` (que
// vive solo mientras dura la llamada) a un array en el heap que el consumidor libera con
// engine_abi_free_string_list.
std::size_t export_string_list(const std::vector<std::string>& names, const char*** out_names) {
    if (names.empty()) {
        *out_names = nullptr;
        return 0;
    }
    auto* array = new const char*[names.size()];
    for (std::size_t i = 0; i < names.size(); ++i) {
        char* copy = new char[names[i].size() + 1];
        std::memcpy(copy, names[i].data(), names[i].size() + 1);
        array[i] = copy;
    }
    *out_names = array;
    return names.size();
}

} // namespace

// EngineModel/EngineProduct son opacos en el header (PLAN.md §5.5: "un lenguaje sin binding
// dedicado" no necesita saber qué hay dentro) -- aquí, cada uno envuelve el mismo puntero que
// ya devuelve Registry<T>::create.
struct EngineModel {
    std::unique_ptr<engine::IModel> ptr;
};
struct EngineProduct {
    std::unique_ptr<engine::IProduct> ptr;
};
struct EngineCalibrator {
    std::unique_ptr<engine::ICalibrator> ptr;
};

namespace {

// Arrays de handles opacos (PLAN.md §7.19, patrón nuevo en esta ABI -- hasta ahora un handle
// se pasaba de uno en uno): construyen los vectores de punteros crudos que engine::price_batch/
// _many/_grid ya esperan, sin copiar ningún IProduct/IModel. Definidos aquí (no junto al resto
// de conversion helpers) porque necesitan el tipo completo de EngineProduct/EngineModel.
std::vector<const engine::IProduct*> to_product_vector(const EngineProduct* const* products, std::size_t n_products) {
    std::vector<const engine::IProduct*> out;
    out.reserve(n_products);
    for (std::size_t i = 0; i < n_products; ++i) {
        if (!products[i]) throw std::invalid_argument("products[] no puede contener NULL");
        out.push_back(products[i]->ptr.get());
    }
    return out;
}

std::vector<const engine::IModel*> to_model_vector(const EngineModel* const* models, std::size_t n_models) {
    std::vector<const engine::IModel*> out;
    out.reserve(n_models);
    for (std::size_t i = 0; i < n_models; ++i) {
        if (!models[i]) throw std::invalid_argument("models[] no puede contener NULL");
        out.push_back(models[i]->ptr.get());
    }
    return out;
}

} // namespace

extern "C" {

int engine_abi_version(void) { return 3; }

std::size_t engine_abi_list_models(const char*** out_names) {
    return export_string_list(registries().models.list(), out_names);
}

std::size_t engine_abi_list_products(const char*** out_names) {
    return export_string_list(registries().products.list(), out_names);
}

std::size_t engine_abi_list_measures(const char*** out_names) {
    return export_string_list(engine::price_measure_names(registries()), out_names);
}

std::size_t engine_abi_list_calibrators(const char*** out_names) {
    return export_string_list(registries().calibrators.list(), out_names);
}

void engine_abi_free_string_list(const char** names, std::size_t count) {
    if (!names) return;
    for (std::size_t i = 0; i < count; ++i) delete[] names[i];
    delete[] names;
}

EngineModel* engine_abi_create_model(const char* name, const EngineParam* params, std::size_t n_params) {
    try {
        auto model = registries().models.create(name, to_params(params, n_params));
        clear_last_error();
        return new EngineModel{std::move(model)};
    } catch (const std::exception& e) {
        set_last_error(e);
        return nullptr;
    }
}

EngineProduct* engine_abi_create_product(const char* name, const EngineParam* params, std::size_t n_params) {
    try {
        auto product = registries().products.create(name, to_params(params, n_params));
        clear_last_error();
        return new EngineProduct{std::move(product)};
    } catch (const std::exception& e) {
        set_last_error(e);
        return nullptr;
    }
}

EngineCalibrator* engine_abi_create_calibrator(const char* name) {
    try {
        auto calibrator = registries().calibrators.create(name);
        clear_last_error();
        return new EngineCalibrator{std::move(calibrator)};
    } catch (const std::exception& e) {
        set_last_error(e);
        return nullptr;
    }
}

void engine_abi_free_model(EngineModel* model) { delete model; }
void engine_abi_free_product(EngineProduct* product) { delete product; }
void engine_abi_free_calibrator(EngineCalibrator* calibrator) { delete calibrator; }

int engine_abi_calibrate(
    const EngineCalibrator* calibrator,
    const EngineMarketSnapshot* market,
    const EngineParam* initial_guess,
    std::size_t n_initial_guess,
    EngineCalibrationResult* out_result
) {
    *out_result = EngineCalibrationResult{};
    try {
        if (!calibrator) {
            throw std::invalid_argument("engine_abi_calibrate: calibrator no puede ser NULL");
        }
        if (!market) {
            throw std::invalid_argument("engine_abi_calibrate: market no puede ser NULL");
        }
        engine::CalibrationResult result =
            calibrator->ptr->calibrate(to_market(*market), to_params(initial_guess, n_initial_guess));

        out_result->optimal_params = export_params(result.optimal_params, &out_result->n_params);
        out_result->rmse = result.rmse;
        out_result->iterations = result.iterations;
        out_result->converged = result.converged ? 1 : 0;

        clear_last_error();
        return 0;
    } catch (const std::exception& e) {
        set_last_error(e);
        *out_result = EngineCalibrationResult{};
        return 1;
    }
}

void engine_abi_free_calibration_result(EngineCalibrationResult* result) {
    if (!result || !result->optimal_params) return;
    for (std::size_t i = 0; i < result->n_params; ++i) {
        delete[] result->optimal_params[i].key;
        delete[] result->optimal_params[i].values;
    }
    delete[] result->optimal_params;
    result->optimal_params = nullptr;
    result->n_params = 0;
}

int engine_abi_price(
    const EngineProduct* product,
    const char** measure_names,
    std::size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceResultEntry** out_entries,
    std::size_t* out_count
) {
    *out_entries = nullptr;
    *out_count = 0;
    try {
        if (!product || !model || !market || !pricing || !execution) {
            throw std::invalid_argument(
                "engine_abi_price: product/model/market/pricing/execution no pueden ser NULL");
        }
        engine::PriceResult result = engine::price(
            registries(), *product->ptr, to_string_vector(measure_names, n_measure_names), *model->ptr,
            to_market(*market), to_pricing_context(*pricing), to_execution_context(*execution)
        );

        *out_entries = export_calc_result(result);
        *out_count = result.size();
        clear_last_error();
        return 0;
    } catch (const std::exception& e) {
        set_last_error(e);
        *out_entries = nullptr;
        *out_count = 0;
        return 1;
    }
}

void engine_abi_free_price_results(EnginePriceResultEntry* entries, std::size_t count) {
    if (!entries) return;
    for (std::size_t i = 0; i < count; ++i) {
        delete[] entries[i].measure_name;
        delete[] entries[i].result.times;
        delete[] entries[i].result.primary;
        delete[] entries[i].result.secondary;
    }
    delete[] entries;
}

int engine_abi_price_batch(
    const EngineProduct** products,
    std::size_t n_products,
    const char** measure_names,
    std::size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceBatchResultEntry** out_entries,
    std::size_t* out_count
) {
    *out_entries = nullptr;
    *out_count = 0;
    try {
        if (!products || !model || !market || !pricing || !execution) {
            throw std::invalid_argument(
                "engine_abi_price_batch: products/model/market/pricing/execution no pueden ser NULL");
        }
        engine::PriceBatchResult result = engine::price_batch(
            registries(), to_product_vector(products, n_products), to_string_vector(measure_names, n_measure_names),
            *model->ptr, to_market(*market), to_pricing_context(*pricing), to_execution_context(*execution)
        );

        auto* entries = new EnginePriceBatchResultEntry[result.size()]{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            entries[i].trade_index = result[i].trade_index;
            entries[i].measures = export_calc_result(result[i].measures);
            entries[i].n_measures = result[i].measures.size();
        }
        *out_entries = entries;
        *out_count = result.size();
        clear_last_error();
        return 0;
    } catch (const std::exception& e) {
        set_last_error(e);
        *out_entries = nullptr;
        *out_count = 0;
        return 1;
    }
}

int engine_abi_price_many(
    const EngineProduct** products,
    std::size_t n_products,
    const char** measure_names,
    std::size_t n_measure_names,
    const EngineModel* model,
    const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceBatchResultEntry** out_entries,
    std::size_t* out_count
) {
    *out_entries = nullptr;
    *out_count = 0;
    try {
        if (!products || !model || !market || !pricing || !execution) {
            throw std::invalid_argument(
                "engine_abi_price_many: products/model/market/pricing/execution no pueden ser NULL");
        }
        engine::PriceBatchResult result = engine::price_many(
            registries(), to_product_vector(products, n_products), to_string_vector(measure_names, n_measure_names),
            *model->ptr, to_market(*market), to_pricing_context(*pricing), to_execution_context(*execution)
        );

        auto* entries = new EnginePriceBatchResultEntry[result.size()]{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            entries[i].trade_index = result[i].trade_index;
            entries[i].measures = export_calc_result(result[i].measures);
            entries[i].n_measures = result[i].measures.size();
        }
        *out_entries = entries;
        *out_count = result.size();
        clear_last_error();
        return 0;
    } catch (const std::exception& e) {
        set_last_error(e);
        *out_entries = nullptr;
        *out_count = 0;
        return 1;
    }
}

void engine_abi_free_price_batch_results(EnginePriceBatchResultEntry* entries, std::size_t count) {
    if (!entries) return;
    for (std::size_t i = 0; i < count; ++i) {
        engine_abi_free_price_results(entries[i].measures, entries[i].n_measures);
    }
    delete[] entries;
}

int engine_abi_price_grid(
    const EngineProduct** products,
    std::size_t n_products,
    const char** measure_names,
    std::size_t n_measure_names,
    const EngineModel** models,
    std::size_t n_models,
    const EngineMarketSnapshot* markets,
    std::size_t n_markets,
    const EnginePricingContext* pricing,
    const EngineExecutionContext* execution,
    EnginePriceGridResultEntry** out_entries,
    std::size_t* out_count
) {
    *out_entries = nullptr;
    *out_count = 0;
    try {
        if (!products || !models || !markets || !pricing || !execution) {
            throw std::invalid_argument(
                "engine_abi_price_grid: products/models/markets/pricing/execution no pueden ser NULL");
        }
        engine::PriceGridResult result = engine::price_grid(
            registries(), to_product_vector(products, n_products), to_string_vector(measure_names, n_measure_names),
            to_model_vector(models, n_models), to_market_vector(markets, n_markets),
            to_pricing_context(*pricing), to_execution_context(*execution)
        );

        auto* entries = new EnginePriceGridResultEntry[result.size()]{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            entries[i].trade_index = result[i].trade_index;
            entries[i].model_index = result[i].model_index;
            entries[i].market_index = result[i].market_index;
            entries[i].measures = export_calc_result(result[i].measures);
            entries[i].n_measures = result[i].measures.size();
        }
        *out_entries = entries;
        *out_count = result.size();
        clear_last_error();
        return 0;
    } catch (const std::exception& e) {
        set_last_error(e);
        *out_entries = nullptr;
        *out_count = 0;
        return 1;
    }
}

void engine_abi_free_price_grid_results(EnginePriceGridResultEntry* entries, std::size_t count) {
    if (!entries) return;
    for (std::size_t i = 0; i < count; ++i) {
        engine_abi_free_price_results(entries[i].measures, entries[i].n_measures);
    }
    delete[] entries;
}

int engine_abi_is_gpu_backend_available(void) { return engine::is_gpu_backend_available() ? 1 : 0; }

std::size_t engine_abi_last_error(char* buffer, std::size_t buffer_len) {
    return copy_to_buffer(g_last_error, buffer, buffer_len);
}

} // extern "C"
