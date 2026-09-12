#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace engine {
namespace payoff {

// Identificador namespaced de un observable de mercado, p.ej. "EQ.SPOT.AAPL",
// "FX.SPOT.EURUSD", "IR.DF.EUR.OIS" (PLAN_PRODUCTS.md §3.1). Una ausencia en el mercado es
// error explícito (ver market_path.hpp), nunca 0.0 silencioso.
struct ObservableId {
    std::string value;
};

inline bool operator==(const ObservableId& a, const ObservableId& b) { return a.value == b.value; }
inline bool operator!=(const ObservableId& a, const ObservableId& b) { return !(a == b); }
inline bool operator<(const ObservableId& a, const ObservableId& b) { return a.value < b.value; }

// Los ids de curva de descuento comparten el mismo espacio de nombres que los observables
// (PLAN_PRODUCTS.md §3.2, `DiscountFactor(curve_id, from, to)`).
using CurveId = ObservableId;

// Identificador de un evento (`Trigger`), único dentro de un contrato (PLAN_PRODUCTS.md §4.1).
// El orden lexicográfico de `value` es el desempate canónico de ADR-P0-03 cuando dos triggers
// disparan en la misma fecha con la misma `priority`.
struct EventId {
    std::string value;
};

inline bool operator==(const EventId& a, const EventId& b) { return a.value == b.value; }
inline bool operator!=(const EventId& a, const EventId& b) { return !(a == b); }
inline bool operator<(const EventId& a, const EventId& b) { return a.value < b.value; }

// Código de moneda (ISO 4217, p.ej. "USD", "EUR"). Vacío es error de validación (§3.1).
struct Currency {
    std::string code;
};

inline bool operator==(const Currency& a, const Currency& b) { return a.code == b.code; }
inline bool operator!=(const Currency& a, const Currency& b) { return !(a == b); }
inline bool operator<(const Currency& a, const Currency& b) { return a.code < b.code; }

// Instante contractual como fracción de año desde una fecha de valoración implícita
// (PLAN_PRODUCTS.md §3.1). Comparar dos `TimePoint` con `==`/`<` de `double` está prohibido en
// el motor de payoff fuera de este archivo: usar siempre `time_equal`/`time_less` (ADR-P0-01).
struct TimePoint {
    double year_fraction;
};

// Identificador denso de nodo, asignado en construcción para diagnósticos/procedencia
// (PLAN_PRODUCTS.md §3.1, §3.5 `Cashflow::source_node`). Único en todo el árbol (compartido
// entre `ScalarExpr`/`Predicate`/`Contract`), no implica orden topológico.
struct NodeId {
    std::uint32_t value;
};

inline bool operator==(const NodeId& a, const NodeId& b) { return a.value == b.value; }
inline bool operator!=(const NodeId& a, const NodeId& b) { return !(a == b); }

// Asigna un `NodeId` nuevo (contador atómico global, PLAN_PRODUCTS.md §3.1). Cada nodo del
// AST (de cualquiera de las tres jerarquías) llama a esto una vez en su constructor.
NodeId allocate_node_id();

// Tolerancia absoluta (en fracción de año) para comparar `TimePoint` (ADR-P0-01): seis
// órdenes de magnitud por debajo de la malla de monitorización diaria más fina (~2.7e-3) y
// tres por encima del error de redondeo acumulado típico de una cadena de aritmética de
// docenas de operaciones sobre magnitudes O(1-50) (~1e-12).
inline constexpr double kTimeToleranceYearFraction = 1e-9;

inline bool time_equal(TimePoint a, TimePoint b, double tolerance = kTimeToleranceYearFraction) {
    double diff = a.year_fraction - b.year_fraction;
    return diff < tolerance && diff > -tolerance;
}

inline bool time_less(TimePoint a, TimePoint b, double tolerance = kTimeToleranceYearFraction) {
    return b.year_fraction - a.year_fraction > tolerance;
}

// Comparador para contenedores ordenados de `TimePoint` (p.ej. `std::set<TimePoint,
// TimePointLess>` en `DependencyReport`) que respeta la tolerancia centralizada en vez de
// comparar `double` directamente (ADR-P0-01).
struct TimePointLess {
    bool operator()(TimePoint a, TimePoint b) const noexcept { return time_less(a, b); }
};

} // namespace payoff
} // namespace engine

namespace std {

template <>
struct hash<engine::payoff::ObservableId> {
    std::size_t operator()(const engine::payoff::ObservableId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

template <>
struct hash<engine::payoff::EventId> {
    std::size_t operator()(const engine::payoff::EventId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

template <>
struct hash<engine::payoff::Currency> {
    std::size_t operator()(const engine::payoff::Currency& c) const noexcept {
        return std::hash<std::string>{}(c.code);
    }
};

} // namespace std
