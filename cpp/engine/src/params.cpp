#include "engine/params.hpp"

#include <stdexcept>

namespace engine {

double get_double(const Params& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end()) {
        throw std::out_of_range("Params: falta la clave requerida '" + key + "'");
    }
    if (const double* value = std::get_if<double>(&it->second)) {
        return *value;
    }
    throw std::invalid_argument("Params: la clave '" + key + "' no es un double");
}

double get_double(const Params& params, const std::string& key, double default_value) {
    auto it = params.find(key);
    if (it == params.end()) {
        return default_value;
    }
    if (const double* value = std::get_if<double>(&it->second)) {
        return *value;
    }
    throw std::invalid_argument("Params: la clave '" + key + "' no es un double");
}

bool get_bool(const Params& params, const std::string& key, bool default_value) {
    auto it = params.find(key);
    if (it == params.end()) {
        return default_value;
    }
    if (const bool* value = std::get_if<bool>(&it->second)) {
        return *value;
    }
    throw std::invalid_argument("Params: la clave '" + key + "' no es un bool");
}

const std::vector<double>& get_vector(const Params& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end()) {
        throw std::out_of_range("Params: falta la clave requerida '" + key + "'");
    }
    if (const std::vector<double>* value = std::get_if<std::vector<double>>(&it->second)) {
        return *value;
    }
    throw std::invalid_argument("Params: la clave '" + key + "' no es un vector<double>");
}

const std::string& get_string(const Params& params, const std::string& key) {
    auto it = params.find(key);
    if (it == params.end()) {
        throw std::out_of_range("Params: falta la clave requerida '" + key + "'");
    }
    if (const std::string* value = std::get_if<std::string>(&it->second)) {
        return *value;
    }
    throw std::invalid_argument("Params: la clave '" + key + "' no es un string");
}

std::string get_string(const Params& params, const std::string& key, std::string default_value) {
    auto it = params.find(key);
    if (it == params.end()) {
        return default_value;
    }
    if (const std::string* value = std::get_if<std::string>(&it->second)) {
        return *value;
    }
    throw std::invalid_argument("Params: la clave '" + key + "' no es un string");
}

bool contains(const Params& params, const std::string& key) {
    return params.find(key) != params.end();
}

} // namespace engine
