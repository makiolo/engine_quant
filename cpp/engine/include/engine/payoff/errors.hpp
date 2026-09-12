#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace engine {
namespace payoff {

// Ruta de nodo para diagnósticos (PLAN_PRODUCTS.md §7.1): identifica dónde, dentro del árbol,
// ocurrió un error de validación o evaluación, p.ej. "root.children[1].condition.left".
class NodePath {
public:
    static NodePath root() { return NodePath("root"); }

    NodePath child(const std::string& field) const { return NodePath(repr_ + "." + field); }

    NodePath child(std::size_t index, const std::string& field) const {
        return NodePath(repr_ + "." + field + "[" + std::to_string(index) + "]");
    }

    const std::string& to_string() const { return repr_; }

private:
    explicit NodePath(std::string repr) : repr_(std::move(repr)) {}
    std::string repr_;
};

// Excepción base del motor de payoff: siempre lleva la ruta del nodo que la originó.
class PayoffError : public std::runtime_error {
public:
    PayoffError(const std::string& message, NodePath path)
        : std::runtime_error(path.to_string() + ": " + message), path_(std::move(path)) {}

    const NodePath& path() const { return path_; }

private:
    NodePath path_;
};

// Error detectado por `ValidationVisitor` (tipos, schedules, ids, monedas, ciclos --
// PLAN_PRODUCTS.md §3.1, ADR-P0-05). Se agregan todos los que aparezcan en el árbol; nunca se
// lanza el primero y se detiene la validación a mitad de camino.
class ValidationError : public PayoffError {
public:
    using PayoffError::PayoffError;
};

// Error detectado por `ScenarioEvaluator` en tiempo de evaluación (fixing/moneda ausente,
// instante activo indefinido, división por cero -- ADR-P0-05, ADR-P0-08). A diferencia de
// `ValidationError`, depende de los datos de mercado de una ruta concreta, no solo del árbol.
class EvaluationError : public PayoffError {
public:
    using PayoffError::PayoffError;
};

} // namespace payoff
} // namespace engine
