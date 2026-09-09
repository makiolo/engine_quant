#include "handles.hpp"

#include <stdexcept>

#include "xloper.hpp"

namespace xlbridge {

HandleRegistry::HandleRegistry() { engine::register_builtins(registries_); }

std::vector<std::string> HandleRegistry::list_models() const { return registries_.models.list(); }
std::vector<std::string> HandleRegistry::list_products() const { return registries_.products.list(); }
std::vector<std::string> HandleRegistry::list_measures() const { return registries_.measures.list(); }

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

std::string HandleRegistry::create_measure(const std::string& name) {
    std::string handle = "measure:" + name;
    if (measures_.find(handle) == measures_.end()) {
        measures_.emplace(handle, registries_.measures.create(name));
    }
    return handle;
}

engine::MeasureResult HandleRegistry::evaluate(
    const std::string& measure_handle,
    const std::string& model_handle,
    const std::string& product_handle,
    const XLOPER12& params_arg
) const {
    auto measure_it = measures_.find(measure_handle);
    if (measure_it == measures_.end()) {
        throw std::out_of_range("xlbridge: handle de medida desconocido: " + measure_handle);
    }
    auto model_it = models_.find(model_handle);
    if (model_it == models_.end()) {
        throw std::out_of_range("xlbridge: handle de modelo desconocido: " + model_handle);
    }
    auto product_it = products_.find(product_handle);
    if (product_it == products_.end()) {
        throw std::out_of_range("xlbridge: handle de producto desconocido: " + product_handle);
    }

    ParsedParams parsed = table_to_params(params_arg);
    return measure_it->second->evaluate(*model_it->second, *product_it->second, parsed.params);
}

void HandleRegistry::clear() {
    models_.clear();
    products_.clear();
    measures_.clear();
}

HandleRegistry& shared() {
    static HandleRegistry instance;
    return instance;
}

} // namespace xlbridge
