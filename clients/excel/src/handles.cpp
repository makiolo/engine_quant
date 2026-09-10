#include "handles.hpp"

#include <stdexcept>

#include "xloper.hpp"

namespace xlbridge {

HandleRegistry::HandleRegistry() { engine::register_builtins(registries_); }

std::vector<std::string> HandleRegistry::list_models() const { return registries_.models.list(); }
std::vector<std::string> HandleRegistry::list_products() const { return registries_.products.list(); }
std::vector<std::string> HandleRegistry::list_measures() const { return engine::calc_measure_names(); }
std::vector<std::string> HandleRegistry::list_calibrators() const { return registries_.calibrators.list(); }

std::string HandleRegistry::create_model(const std::string& name, const XLOPER12& params_arg) {
    ParsedParams parsed = table_to_params(params_arg);
    std::string handle = "model:" + name + "#" + parsed.canonical;
    if (models_.find(handle) == models_.end()) {
        models_.emplace(handle, registries_.models.create(name, parsed.params));
    }
    return handle;
}

std::string HandleRegistry::create_product(const std::string& name, const XLOPER12& params_arg) {
    ParsedParams parsed = table_to_params(params_arg);
    std::string handle = "product:" + name + "#" + parsed.canonical;
    if (products_.find(handle) == products_.end()) {
        products_.emplace(handle, registries_.products.create(name, parsed.params));
    }
    return handle;
}

std::string HandleRegistry::create_market(const XLOPER12& params_arg) {
    ParsedParams parsed = table_to_params(params_arg);
    std::string handle = "market:" + parsed.canonical;
    if (markets_.find(handle) == markets_.end()) {
        markets_.emplace(handle, engine::MarketSnapshot(parsed.params));
    }
    return handle;
}

std::string HandleRegistry::create_context(const XLOPER12& params_arg) {
    ParsedParams parsed = table_to_params(params_arg);
    std::string handle = "pricing:" + parsed.canonical;
    if (pricing_contexts_.find(handle) == pricing_contexts_.end()) {
        pricing_contexts_.emplace(handle, engine::PricingContext(parsed.params));
    }
    return handle;
}

std::string HandleRegistry::create_execution(const XLOPER12& params_arg) {
    ParsedParams parsed = table_to_params(params_arg);
    std::string handle = "execution:" + parsed.canonical;
    if (execution_contexts_.find(handle) == execution_contexts_.end()) {
        execution_contexts_.emplace(handle, engine::ExecutionContext(parsed.params));
    }
    return handle;
}

std::string HandleRegistry::create_calibrator(const std::string& name) {
    std::string handle = "calibrator:" + name;
    if (calibrators_.find(handle) == calibrators_.end()) {
        calibrators_.emplace(handle, registries_.calibrators.create(name));
    }
    return handle;
}

engine::CalcResult HandleRegistry::calc(
    const std::string& product_handle,
    const std::vector<std::string>& measure_names,
    const std::string& model_handle,
    const std::string& market_handle,
    const std::string& pricing_handle,
    const std::string& execution_handle
) const {
    auto product_it = products_.find(product_handle);
    if (product_it == products_.end()) {
        throw std::out_of_range("xlbridge: handle de producto desconocido: " + product_handle);
    }
    auto model_it = models_.find(model_handle);
    if (model_it == models_.end()) {
        throw std::out_of_range("xlbridge: handle de modelo desconocido: " + model_handle);
    }
    auto market_it = markets_.find(market_handle);
    if (market_it == markets_.end()) {
        throw std::out_of_range("xlbridge: handle de mercado desconocido: " + market_handle);
    }
    auto pricing_it = pricing_contexts_.find(pricing_handle);
    if (pricing_it == pricing_contexts_.end()) {
        throw std::out_of_range("xlbridge: handle de contexto de valoracion desconocido: " + pricing_handle);
    }
    auto execution_it = execution_contexts_.find(execution_handle);
    if (execution_it == execution_contexts_.end()) {
        throw std::out_of_range("xlbridge: handle de contexto de ejecucion desconocido: " + execution_handle);
    }

    return engine::calc(
        registries_, *product_it->second, measure_names, *model_it->second,
        market_it->second, pricing_it->second, execution_it->second
    );
}

engine::CalibrationResult HandleRegistry::calibrate(
    const std::string& calibrator_handle, const std::string& market_handle, const XLOPER12& initial_guess_arg
) const {
    auto calibrator_it = calibrators_.find(calibrator_handle);
    if (calibrator_it == calibrators_.end()) {
        throw std::out_of_range("xlbridge: handle de calibrador desconocido: " + calibrator_handle);
    }
    auto market_it = markets_.find(market_handle);
    if (market_it == markets_.end()) {
        throw std::out_of_range("xlbridge: handle de mercado desconocido: " + market_handle);
    }

    ParsedParams initial_guess = table_to_params(initial_guess_arg);
    return calibrator_it->second->calibrate(market_it->second, initial_guess.params);
}

void HandleRegistry::clear() {
    models_.clear();
    products_.clear();
    markets_.clear();
    pricing_contexts_.clear();
    execution_contexts_.clear();
    calibrators_.clear();
}

HandleRegistry& shared() {
    static HandleRegistry instance;
    return instance;
}

} // namespace xlbridge
