// Implementación de la API universal en C ABI (PLAN.md Fase 6, §5.5, ver engine/abi.h para
// el contrato). Traduce entre los tipos C planos del header y engine::Registries/Registry<T>/
// IMeasure ya existentes (PLAN.md §5.4) -- no reimplementa lógica de negocio, solo la
// frontera: construir/leer engine::Params desde EngineParam[], convertir engine::MeasureResult
// a EngineMeasureResult con arrays owned por esta librería, y atrapar toda excepción de C++
// (ninguna puede cruzar a un lenguaje sin soporte de excepciones de C++, PLAN.md §5.5).

#include "engine/abi.h"

#include "engine/bootstrap.hpp"
#include "engine/engine.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
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
// convención que snprintf: usada por engine_abi_last_error/engine_abi_get_compute_backend.
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
        }
    }
    return result;
}

// engine_abi_list_models/products/measures comparten esta implementación: copia `names` (que
// vive solo mientras dura la llamada a Registry<T>::list()) a un array en el heap que el
// consumidor libera con engine_abi_free_string_list.
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

// EngineModel/EngineProduct/EngineMeasure son opacos en el header (PLAN.md §5.5: "un
// lenguaje sin binding dedicado" no necesita saber qué hay dentro) -- aquí, cada uno envuelve
// el mismo puntero que ya devuelve Registry<T>::create/Registry<IMeasure>::create.
struct EngineModel {
    std::unique_ptr<engine::IModel> ptr;
};
struct EngineProduct {
    std::unique_ptr<engine::IProduct> ptr;
};
struct EngineMeasure {
    std::unique_ptr<engine::IMeasure> ptr;
};

extern "C" {

int engine_abi_version(void) { return 1; }

std::size_t engine_abi_list_models(const char*** out_names) {
    return export_string_list(registries().models.list(), out_names);
}

std::size_t engine_abi_list_products(const char*** out_names) {
    return export_string_list(registries().products.list(), out_names);
}

std::size_t engine_abi_list_measures(const char*** out_names) {
    return export_string_list(registries().measures.list(), out_names);
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

EngineMeasure* engine_abi_create_measure(const char* name) {
    try {
        auto measure = registries().measures.create(name);
        clear_last_error();
        return new EngineMeasure{std::move(measure)};
    } catch (const std::exception& e) {
        set_last_error(e);
        return nullptr;
    }
}

void engine_abi_free_model(EngineModel* model) { delete model; }
void engine_abi_free_product(EngineProduct* product) { delete product; }
void engine_abi_free_measure(EngineMeasure* measure) { delete measure; }

int engine_abi_evaluate(
    const EngineMeasure* measure,
    const EngineModel* model,
    const EngineProduct* product,
    const EngineParam* params,
    std::size_t n_params,
    EngineMeasureResult* out_result
) {
    *out_result = EngineMeasureResult{};
    try {
        if (!measure || !model || !product) {
            throw std::invalid_argument("engine_abi_evaluate: measure/model/product no puede ser NULL");
        }
        engine::MeasureResult result =
            measure->ptr->evaluate(*model->ptr, *product->ptr, to_params(params, n_params));

        out_result->len = result.times.size();
        if (out_result->len > 0) {
            out_result->times = new double[out_result->len];
            out_result->primary = new double[out_result->len];
            out_result->secondary = new double[out_result->len];
            std::memcpy(out_result->times, result.times.data(), out_result->len * sizeof(double));
            std::memcpy(out_result->primary, result.primary.data(), out_result->len * sizeof(double));
            std::memcpy(out_result->secondary, result.secondary.data(), out_result->len * sizeof(double));
        }
        out_result->has_scalar = result.has_scalar ? 1 : 0;
        out_result->scalar = result.scalar;

        clear_last_error();
        return 0;
    } catch (const std::exception& e) {
        set_last_error(e);
        *out_result = EngineMeasureResult{};
        return 1;
    }
}

void engine_abi_free_measure_result(EngineMeasureResult* result) {
    if (!result) return;
    delete[] result->times;
    delete[] result->primary;
    delete[] result->secondary;
    *result = EngineMeasureResult{};
}

int engine_abi_set_compute_backend(const char* name) {
    return engine::set_compute_backend(name) ? 1 : 0;
}

std::size_t engine_abi_get_compute_backend(char* buffer, std::size_t buffer_len) {
    return copy_to_buffer(engine::compute_backend_name(), buffer, buffer_len);
}

int engine_abi_is_gpu_backend_available(void) { return engine::is_gpu_backend_available() ? 1 : 0; }

std::size_t engine_abi_last_error(char* buffer, std::size_t buffer_len) {
    return copy_to_buffer(g_last_error, buffer, buffer_len);
}

} // extern "C"
