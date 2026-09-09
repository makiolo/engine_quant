#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace engine {

// Bag de parámetros genérico para las factories del registry (PLAN.md §5.4): permite que
// `Registry<Interface>::create(name, params)` tenga una firma uniforme sin importar qué
// modelo/producto/medida concreto se está construyendo.
using ParamValue = std::variant<double, std::vector<double>, bool>;
using Params = std::unordered_map<std::string, ParamValue>;

// Lanza std::out_of_range si falta la clave, std::invalid_argument si el valor presente no
// es un double.
double get_double(const Params& params, const std::string& key);

// Igual que la anterior pero sin lanzar si falta la clave: devuelve default_value.
double get_double(const Params& params, const std::string& key, double default_value);

// Igual que get_double(params, key, default_value): sin lanzar si falta la clave.
bool get_bool(const Params& params, const std::string& key, bool default_value);

// Lanza std::out_of_range si falta la clave, std::invalid_argument si el valor presente no
// es un vector<double>.
const std::vector<double>& get_vector(const Params& params, const std::string& key);

bool contains(const Params& params, const std::string& key);

} // namespace engine
