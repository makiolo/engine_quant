#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/payoff/ids.hpp"
#include "engine/payoff/visitor.hpp"

namespace engine {
namespace payoff {

// Nodo base de expresión escalar (PLAN_PRODUCTS.md §3.2). Los nodos no contienen lógica de
// modelo ni consultan singletons globales; solo exponen su forma y `accept(ScalarVisitor&)`
// (doble dispatch, ADR-P0-06).
class ScalarExpr {
public:
    explicit ScalarExpr(NodeId id) : node_id_(id) {}
    virtual ~ScalarExpr() = default;
    virtual void accept(ScalarVisitor& visitor) const = 0;
    NodeId node_id() const { return node_id_; }

private:
    NodeId node_id_;
};

// Puntero inmutable compartido (ADR-P0-07): representación de hijos en todo el AST.
using ScalarExprPtr = std::shared_ptr<const ScalarExpr>;

class Constant : public ScalarExpr {
public:
    explicit Constant(double value);
    double value() const { return value_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    double value_;
};

// Parámetro contractual inmutable (strike, nocional, barrera, ...): su valor lo resuelve el
// contexto de construcción del trade, no el AST -- el nodo solo guarda el nombre.
class Parameter : public ScalarExpr {
public:
    explicit Parameter(std::string name);
    const std::string& name() const { return name_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    std::string name_;
};

// Valor histórico/fijado de un observable en una fecha concreta.
class Fixing : public ScalarExpr {
public:
    Fixing(ObservableId observable, TimePoint time);
    const ObservableId& observable() const { return observable_; }
    TimePoint time() const { return time_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ObservableId observable_;
    TimePoint time_;
};

// Valor de un observable en el instante de evaluación del nodo (cursor de "instante activo",
// ADR-P0-08), a diferencia de `Fixing`, que fija una fecha explícita.
class Current : public ScalarExpr {
public:
    explicit Current(ObservableId observable);
    const ObservableId& observable() const { return observable_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ObservableId observable_;
};

class Add : public ScalarExpr {
public:
    Add(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class Sub : public ScalarExpr {
public:
    Sub(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class Mul : public ScalarExpr {
public:
    Mul(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

// División por cero es error de ruta (§3.2), detectado por `ScenarioEvaluator` en tiempo de
// evaluación (no por `ValidationVisitor`, que no conoce el valor del denominador).
class Div : public ScalarExpr {
public:
    Div(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class Neg : public ScalarExpr {
public:
    explicit Neg(ScalarExprPtr operand);
    const ScalarExprPtr& operand() const { return operand_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr operand_;
};

class Abs : public ScalarExpr {
public:
    explicit Abs(ScalarExprPtr operand);
    const ScalarExprPtr& operand() const { return operand_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr operand_;
};

class Exp : public ScalarExpr {
public:
    explicit Exp(ScalarExprPtr operand);
    const ScalarExprPtr& operand() const { return operand_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr operand_;
};

// `log` es error de ruta si el operando evalúa a <= 0 (mismo tratamiento que división por
// cero); lo detecta `ScenarioEvaluator`, no `ValidationVisitor`.
class Log : public ScalarExpr {
public:
    explicit Log(ScalarExprPtr operand);
    const ScalarExprPtr& operand() const { return operand_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr operand_;
};

class Pow : public ScalarExpr {
public:
    Pow(ScalarExprPtr base, ScalarExprPtr exponent);
    const ScalarExprPtr& base() const { return base_; }
    const ScalarExprPtr& exponent() const { return exponent_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr base_;
    ScalarExprPtr exponent_;
};

class Min : public ScalarExpr {
public:
    Min(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class Max : public ScalarExpr {
public:
    Max(ScalarExprPtr left, ScalarExprPtr right);
    const ScalarExprPtr& left() const { return left_; }
    const ScalarExprPtr& right() const { return right_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr left_;
    ScalarExprPtr right_;
};

class Clamp : public ScalarExpr {
public:
    Clamp(ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high);
    const ScalarExprPtr& value() const { return value_; }
    const ScalarExprPtr& low() const { return low_; }
    const ScalarExprPtr& high() const { return high_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ScalarExprPtr value_;
    ScalarExprPtr low_;
    ScalarExprPtr high_;
};

// Media ponderada de un observable sobre un schedule de fechas (asiáticas, §3.2). `schedule` y
// `weights` deben tener la misma longitud -- lo comprueba `ValidationVisitor`, no el
// constructor (ADR-P0-05).
class Average : public ScalarExpr {
public:
    Average(ObservableId observable, std::vector<TimePoint> schedule, std::vector<double> weights);
    const ObservableId& observable() const { return observable_; }
    const std::vector<TimePoint>& schedule() const { return schedule_; }
    const std::vector<double>& weights() const { return weights_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ObservableId observable_;
    std::vector<TimePoint> schedule_;
    std::vector<double> weights_;
};

// Mínimo observado del observable hasta el instante de evaluación actual (ADR-P0-08: en
// Fases 1-2 se calcula de forma pura sobre la `MarketPath` completa, sin acumulador mutable).
class RunningMin : public ScalarExpr {
public:
    explicit RunningMin(ObservableId observable);
    const ObservableId& observable() const { return observable_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ObservableId observable_;
};

class RunningMax : public ScalarExpr {
public:
    explicit RunningMax(ObservableId observable);
    const ObservableId& observable() const { return observable_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    ObservableId observable_;
};

// Fecha del primer hit de un evento (`EventState::first_hit_time`, §4.1). Error de evaluación
// si el evento no ha ocurrido en la ruta.
class EventTime : public ScalarExpr {
public:
    explicit EventTime(EventId event);
    const EventId& event() const { return event_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    EventId event_;
};

// Fixing capturado al dispararse un evento (`EventState::captured_values`, §4.1). Error de
// evaluación si el evento no ha ocurrido o no capturó ese observable.
class EventValue : public ScalarExpr {
public:
    EventValue(EventId event, ObservableId observable);
    const EventId& event() const { return event_; }
    const ObservableId& observable() const { return observable_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    EventId event_;
    ObservableId observable_;
};

class DiscountFactor : public ScalarExpr {
public:
    DiscountFactor(CurveId curve, TimePoint from, TimePoint to);
    const CurveId& curve() const { return curve_; }
    TimePoint from() const { return from_; }
    TimePoint to() const { return to_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    CurveId curve_;
    TimePoint from_;
    TimePoint to_;
};

// Conversión explícita de moneda; nunca implícita (§3.2).
class FxConversion : public ScalarExpr {
public:
    FxConversion(Currency from_currency, Currency to_currency, TimePoint time);
    const Currency& from_currency() const { return from_currency_; }
    const Currency& to_currency() const { return to_currency_; }
    TimePoint time() const { return time_; }
    void accept(ScalarVisitor& visitor) const override;

private:
    Currency from_currency_;
    Currency to_currency_;
    TimePoint time_;
};

// Builders libres (PLAN_PRODUCTS.md §7.1): azúcar sintáctico sobre los constructores. `minimum`/
// `maximum` (no `min`/`max`) evita colisión con las macros `min`/`max` de <windows.h> y con
// `std::min`/`std::max` en sitios de llamada que hagan `using namespace std`.
ScalarExprPtr constant(double value);
ScalarExprPtr parameter(std::string name);
ScalarExprPtr fixing(ObservableId observable, TimePoint time);
ScalarExprPtr current(ObservableId observable);
ScalarExprPtr add(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr sub(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr mul(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr div(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr neg(ScalarExprPtr operand);
ScalarExprPtr abs(ScalarExprPtr operand);
ScalarExprPtr exp(ScalarExprPtr operand);
ScalarExprPtr log(ScalarExprPtr operand);
ScalarExprPtr pow(ScalarExprPtr base, ScalarExprPtr exponent);
ScalarExprPtr minimum(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr maximum(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr clamp(ScalarExprPtr value, ScalarExprPtr low, ScalarExprPtr high);
ScalarExprPtr average(ObservableId observable, std::vector<TimePoint> schedule, std::vector<double> weights);
ScalarExprPtr running_min(ObservableId observable);
ScalarExprPtr running_max(ObservableId observable);
ScalarExprPtr event_time(EventId event);
ScalarExprPtr event_value(EventId event, ObservableId observable);
ScalarExprPtr discount_factor(CurveId curve, TimePoint from, TimePoint to);
ScalarExprPtr fx_conversion(Currency from_currency, Currency to_currency, TimePoint time);

// Sugar de operadores (§7.1: "azúcar sintáctico; los nodos reales siguen siendo tipos
// explícitos visitables") sobre los builders de arriba.
ScalarExprPtr operator+(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr operator-(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr operator*(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr operator/(ScalarExprPtr left, ScalarExprPtr right);
ScalarExprPtr operator-(ScalarExprPtr operand);

} // namespace payoff
} // namespace engine
