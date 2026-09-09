#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/params.hpp"

namespace engine {

// Registry genérico por interfaz (PLAN.md §5.4): un registry independiente por cada una de
// IModel/IProduct/IMeasure, sin registry monolítico. Registro explícito centralizado (ver
// bootstrap.hpp), no auto-registro estático.
template <typename Interface>
class Registry {
public:
    using Factory = std::function<std::unique_ptr<Interface>(const Params&)>;

    void register_factory(std::string name, Factory factory) {
        factories_[std::move(name)] = std::move(factory);
    }

    template <typename Concrete>
    void register_type(std::string name) {
        register_factory(std::move(name), [](const Params& params) {
            return std::make_unique<Concrete>(params);
        });
    }

    std::unique_ptr<Interface> create(const std::string& name, const Params& params = {}) const {
        auto it = factories_.find(name);
        if (it == factories_.end()) {
            throw std::out_of_range("Registry: tipo no registrado: " + name);
        }
        return it->second(params);
    }

    bool contains(const std::string& name) const { return factories_.count(name) > 0; }

    std::vector<std::string> list() const {
        std::vector<std::string> names;
        names.reserve(factories_.size());
        for (const auto& [name, factory] : factories_) names.push_back(name);
        return names;
    }

private:
    std::unordered_map<std::string, Factory> factories_;
};

} // namespace engine
