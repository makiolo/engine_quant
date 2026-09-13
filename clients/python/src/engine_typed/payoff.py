"""`engine_typed.payoff`: AST de payoff tipado (PLAN_PRODUCTS.md §7.3, adelantado de Fase 10).

Modelos Pydantic discriminados por `type`, mismos nombres de campo snake_case que
`docs/schema/engine.payoff/v1.schema.json` (fuente de verdad) y mismos nombres de builder que
`cpp/engine/include/engine/payoff/{expression,predicate,contract}.hpp`. `PayoffProduct` es un
`TradeSpec` más: `to_params()` produce `{"spec": json_canonico}`, que `Engine.create_product`
ya consume genéricamente (SS7.2) -- ningún cambio en `engine_py_ext.cpp` hizo falta.

    import engine, engine_typed as q

    call = q.when(
        1.0,
        q.cashflow("USD", 1_000 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100, 0)),
    )
    trade = q.PayoffProduct(id="AAPL_CALL_100", contract=call)
    native = eng.create_product(trade.product_type, trade.to_params())

No serializa a JSON canónico byte a byte igual que `CanonicalVisitor` de C++ (eso no hace
falta: el hash canónico se calcula en C++ sobre el AST ya parseado, no sobre el texto que
Python produjo) -- solo necesita ser JSON válido conforme al schema.
"""

from __future__ import annotations

import json
from typing import Annotated, ClassVar, List, Literal, Union

from pydantic import BaseModel, ConfigDict, Field

from engine_typed.trade import TradeSpec


def _as_scalar(x: object) -> "ScalarExprBase":
    if isinstance(x, ScalarExprBase):
        return x
    if isinstance(x, (int, float)) and not isinstance(x, bool):
        return Constant(value=float(x))
    raise TypeError(f"no se puede promocionar {x!r} a ScalarExpr")


# ---------------------------------------------------------------------------------------------
# ScalarExpr (PLAN_PRODUCTS.md §3.2, 23 nodos)
# ---------------------------------------------------------------------------------------------


class ScalarExprBase(BaseModel):
    model_config = ConfigDict(frozen=True, populate_by_name=True)

    # Azúcar de operadores (§7.1: "los nodos reales siguen siendo tipos explícitos
    # visitables"), reproduce `operator+/-/*//,unario-` de `ScalarExprPtr` en C++.
    def __add__(self, other: object) -> "Add":
        return add(self, other)

    def __radd__(self, other: object) -> "Add":
        return add(other, self)

    def __sub__(self, other: object) -> "Sub":
        return sub(self, other)

    def __rsub__(self, other: object) -> "Sub":
        return sub(other, self)

    def __mul__(self, other: object) -> "Mul":
        return mul(self, other)

    def __rmul__(self, other: object) -> "Mul":
        return mul(other, self)

    def __truediv__(self, other: object) -> "Div":
        return div(self, other)

    def __rtruediv__(self, other: object) -> "Div":
        return div(other, self)

    def __neg__(self) -> "Neg":
        return neg(self)


class Constant(ScalarExprBase):
    type: Literal["constant"] = "constant"
    value: float


class Parameter(ScalarExprBase):
    type: Literal["parameter"] = "parameter"
    name: str


class Fixing(ScalarExprBase):
    type: Literal["fixing"] = "fixing"
    observable: str
    time: float


class Current(ScalarExprBase):
    type: Literal["current"] = "current"
    observable: str


class Add(ScalarExprBase):
    type: Literal["add"] = "add"
    left: ScalarExpr
    right: ScalarExpr


class Sub(ScalarExprBase):
    type: Literal["sub"] = "sub"
    left: ScalarExpr
    right: ScalarExpr


class Mul(ScalarExprBase):
    type: Literal["mul"] = "mul"
    left: ScalarExpr
    right: ScalarExpr


class Div(ScalarExprBase):
    type: Literal["div"] = "div"
    left: ScalarExpr
    right: ScalarExpr


class Neg(ScalarExprBase):
    type: Literal["neg"] = "neg"
    operand: ScalarExpr


class Abs(ScalarExprBase):
    type: Literal["abs"] = "abs"
    operand: ScalarExpr


class Exp(ScalarExprBase):
    type: Literal["exp"] = "exp"
    operand: ScalarExpr


class Log(ScalarExprBase):
    type: Literal["log"] = "log"
    operand: ScalarExpr


class Pow(ScalarExprBase):
    type: Literal["pow"] = "pow"
    base: ScalarExpr
    exponent: ScalarExpr


class Min(ScalarExprBase):
    type: Literal["min"] = "min"
    left: ScalarExpr
    right: ScalarExpr


class Max(ScalarExprBase):
    type: Literal["max"] = "max"
    left: ScalarExpr
    right: ScalarExpr


class Clamp(ScalarExprBase):
    type: Literal["clamp"] = "clamp"
    value: ScalarExpr
    low: ScalarExpr
    high: ScalarExpr


class Average(ScalarExprBase):
    type: Literal["average"] = "average"
    observable: str
    schedule: List[float]
    weights: List[float]


class RunningMin(ScalarExprBase):
    type: Literal["running_min"] = "running_min"
    observable: str


class RunningMax(ScalarExprBase):
    type: Literal["running_max"] = "running_max"
    observable: str


class EventTime(ScalarExprBase):
    type: Literal["event_time"] = "event_time"
    event: str


class EventValue(ScalarExprBase):
    type: Literal["event_value"] = "event_value"
    event: str
    observable: str


class DiscountFactor(ScalarExprBase):
    type: Literal["discount_factor"] = "discount_factor"
    curve: str
    # "from" es palabra reservada en Python: alias al nombre de campo del schema JSON
    # (docs/schema/engine.payoff/v1.schema.json), `populate_by_name=True` permite construir
    # con `from_=...` y `model_dump(by_alias=True)` serializa como "from".
    from_: float = Field(alias="from")
    to: float


class FxConversion(ScalarExprBase):
    type: Literal["fx_conversion"] = "fx_conversion"
    from_currency: str
    to_currency: str
    time: float


ScalarExpr = Annotated[
    Union[
        Constant, Parameter, Fixing, Current, Add, Sub, Mul, Div, Neg, Abs, Exp, Log, Pow,
        Min, Max, Clamp, Average, RunningMin, RunningMax, EventTime, EventValue,
        DiscountFactor, FxConversion,
    ],
    Field(discriminator="type"),
]


# ---------------------------------------------------------------------------------------------
# Predicate (PLAN_PRODUCTS.md §3.3, 12 nodos)
# ---------------------------------------------------------------------------------------------


class PredicateBase(BaseModel):
    model_config = ConfigDict(frozen=True)


class Greater(PredicateBase):
    type: Literal["greater"] = "greater"
    left: ScalarExpr
    right: ScalarExpr


class Less(PredicateBase):
    type: Literal["less"] = "less"
    left: ScalarExpr
    right: ScalarExpr


class GreaterEqual(PredicateBase):
    type: Literal["greater_equal"] = "greater_equal"
    left: ScalarExpr
    right: ScalarExpr


class LessEqual(PredicateBase):
    type: Literal["less_equal"] = "less_equal"
    left: ScalarExpr
    right: ScalarExpr


class Eq(PredicateBase):
    type: Literal["eq"] = "eq"
    left: ScalarExpr
    right: ScalarExpr
    tolerance: float


class All(PredicateBase):
    type: Literal["all"] = "all"
    operands: List[Predicate]


class Any(PredicateBase):  # nombre del nodo ("any", SS3.3); no colisiona con typing.Any (no se importa aqui)
    type: Literal["any"] = "any"
    operands: List[Predicate]


class Not(PredicateBase):
    type: Literal["not"] = "not"
    operand: Predicate


class Between(PredicateBase):
    type: Literal["between"] = "between"
    value: ScalarExpr
    low: ScalarExpr
    high: ScalarExpr
    low_inclusive: bool
    high_inclusive: bool


class EventOccurred(PredicateBase):
    type: Literal["event_occurred"] = "event_occurred"
    event: str


class Before(PredicateBase):
    type: Literal["before"] = "before"
    time: float


class After(PredicateBase):
    type: Literal["after"] = "after"
    time: float


Predicate = Annotated[
    Union[Greater, Less, GreaterEqual, LessEqual, Eq, All, Any, Not, Between, EventOccurred, Before, After],
    Field(discriminator="type"),
]


# ---------------------------------------------------------------------------------------------
# Contract (PLAN_PRODUCTS.md §3.4, 9 nodos)
# ---------------------------------------------------------------------------------------------


class ContractBase(BaseModel):
    model_config = ConfigDict(frozen=True)


class Zero(ContractBase):
    type: Literal["zero"] = "zero"


class Cashflow(ContractBase):
    type: Literal["cashflow"] = "cashflow"
    currency: str
    amount: ScalarExpr


class Give(ContractBase):
    type: Literal["give"] = "give"
    child: Contract


class Both(ContractBase):
    type: Literal["both"] = "both"
    children: List[Contract]


class Scale(ContractBase):
    type: Literal["scale"] = "scale"
    factor: ScalarExpr
    child: Contract


class If(ContractBase):
    type: Literal["if"] = "if"
    condition: Predicate
    if_true: Contract
    if_false: Contract


class When(ContractBase):
    type: Literal["when"] = "when"
    time: float
    child: Contract


class Trigger(ContractBase):
    type: Literal["trigger"] = "trigger"
    id: str
    monitoring_times: List[float]
    condition: Predicate
    monitoring: Literal["discrete", "continuous_approximation"]
    settlement: Literal["at_hit", "at_scheduled_payment"]
    priority: int
    latch: bool
    on_hit: Contract
    on_miss: Contract


class Exercise(ContractBase):
    type: Literal["exercise"] = "exercise"
    id: str
    dates: List[float]
    exercise_value: ScalarExpr
    continuation: Contract


Contract = Annotated[
    Union[Zero, Cashflow, Give, Both, Scale, If, When, Trigger, Exercise],
    Field(discriminator="type"),
]


# ---------------------------------------------------------------------------------------------
# Builders (mismos nombres que cpp/engine/include/engine/payoff/{expression,predicate,contract}.hpp)
# ---------------------------------------------------------------------------------------------


def constant(value: float) -> Constant:
    return Constant(value=float(value))


def parameter(name: str) -> Parameter:
    return Parameter(name=name)


def fixing(observable: str, time: float) -> Fixing:
    return Fixing(observable=observable, time=float(time))


def current(observable: str) -> Current:
    return Current(observable=observable)


def add(left: object, right: object) -> Add:
    return Add(left=_as_scalar(left), right=_as_scalar(right))


def sub(left: object, right: object) -> Sub:
    return Sub(left=_as_scalar(left), right=_as_scalar(right))


def mul(left: object, right: object) -> Mul:
    return Mul(left=_as_scalar(left), right=_as_scalar(right))


def div(left: object, right: object) -> Div:
    return Div(left=_as_scalar(left), right=_as_scalar(right))


def neg(operand: object) -> Neg:
    return Neg(operand=_as_scalar(operand))


def abs(operand: object) -> Abs:  # sombrea el builtin intencionalmente, solo dentro de q.
    return Abs(operand=_as_scalar(operand))


def exp(operand: object) -> Exp:
    return Exp(operand=_as_scalar(operand))


def log(operand: object) -> Log:
    return Log(operand=_as_scalar(operand))


def pow(base: object, exponent: object) -> Pow:  # sombrea el builtin intencionalmente
    return Pow(base=_as_scalar(base), exponent=_as_scalar(exponent))


def minimum(left: object, right: object) -> Min:
    return Min(left=_as_scalar(left), right=_as_scalar(right))


def maximum(left: object, right: object) -> Max:
    return Max(left=_as_scalar(left), right=_as_scalar(right))


def clamp(value: object, low: object, high: object) -> Clamp:
    return Clamp(value=_as_scalar(value), low=_as_scalar(low), high=_as_scalar(high))


def average(observable: str, schedule: List[float], weights: List[float]) -> Average:
    return Average(observable=observable, schedule=list(schedule), weights=list(weights))


def running_min(observable: str) -> RunningMin:
    return RunningMin(observable=observable)


def running_max(observable: str) -> RunningMax:
    return RunningMax(observable=observable)


def event_time(event: str) -> EventTime:
    return EventTime(event=event)


def event_value(event: str, observable: str) -> EventValue:
    return EventValue(event=event, observable=observable)


def discount_factor(curve: str, from_: float, to: float) -> DiscountFactor:
    return DiscountFactor(curve=curve, from_=float(from_), to=float(to))


def fx_conversion(from_currency: str, to_currency: str, time: float) -> FxConversion:
    return FxConversion(from_currency=from_currency, to_currency=to_currency, time=float(time))


def greater(left: object, right: object) -> Greater:
    return Greater(left=_as_scalar(left), right=_as_scalar(right))


def less(left: object, right: object) -> Less:
    return Less(left=_as_scalar(left), right=_as_scalar(right))


def greater_equal(left: object, right: object) -> GreaterEqual:
    return GreaterEqual(left=_as_scalar(left), right=_as_scalar(right))


def less_equal(left: object, right: object) -> LessEqual:
    return LessEqual(left=_as_scalar(left), right=_as_scalar(right))


def eq(left: object, right: object, tolerance: float) -> Eq:
    return Eq(left=_as_scalar(left), right=_as_scalar(right), tolerance=float(tolerance))


def all_of(operands: List["Predicate"]) -> All:
    return All(operands=list(operands))


def any_of(operands: List["Predicate"]) -> Any:
    return Any(operands=list(operands))


def negate(operand: "Predicate") -> Not:
    return Not(operand=operand)


def between(value: object, low: object, high: object, low_inclusive: bool, high_inclusive: bool) -> Between:
    return Between(
        value=_as_scalar(value), low=_as_scalar(low), high=_as_scalar(high),
        low_inclusive=low_inclusive, high_inclusive=high_inclusive,
    )


def event_occurred(event: str) -> EventOccurred:
    return EventOccurred(event=event)


def before(time: float) -> Before:
    return Before(time=float(time))


def after(time: float) -> After:
    return After(time=float(time))


def zero() -> Zero:
    return Zero()


def cashflow(currency: str, amount: object) -> Cashflow:
    return Cashflow(currency=currency, amount=_as_scalar(amount))


def give(child: "Contract") -> Give:
    return Give(child=child)


def both(children: List["Contract"]) -> Both:
    return Both(children=list(children))


def scale(factor: object, child: "Contract") -> Scale:
    return Scale(factor=_as_scalar(factor), child=child)


def if_(condition: "Predicate", if_true: "Contract", if_false: "Contract") -> If:
    return If(condition=condition, if_true=if_true, if_false=if_false)


def when(time: float, child: "Contract") -> When:
    return When(time=float(time), child=child)


def trigger(
    id: str,
    monitoring_times: List[float],
    condition: "Predicate",
    monitoring: str,
    settlement: str,
    priority: int,
    latch: bool,
    on_hit: "Contract",
    on_miss: "Contract",
) -> Trigger:
    return Trigger(
        id=id, monitoring_times=list(monitoring_times), condition=condition, monitoring=monitoring,
        settlement=settlement, priority=priority, latch=latch, on_hit=on_hit, on_miss=on_miss,
    )


def exercise(id: str, dates: List[float], exercise_value: object, continuation: "Contract") -> Exercise:
    return Exercise(id=id, dates=list(dates), exercise_value=_as_scalar(exercise_value), continuation=continuation)


# ---------------------------------------------------------------------------------------------
# PayoffProduct (PLAN_PRODUCTS.md §7.2, §7.3)
# ---------------------------------------------------------------------------------------------


class PayoffProduct(TradeSpec):
    product_type: ClassVar[str] = "Payoff"

    id: str
    contract: Contract

    def to_params(self) -> dict:
        document = {
            "schema": "engine.payoff/v1",
            "id": self.id,
            "contract": self.contract.model_dump(mode="json", by_alias=True),
        }
        return {"spec": json.dumps(document, separators=(",", ":"), allow_nan=False)}


# Resuelve las forward-refs de las uniones discriminadas anidadas (pydantic v2 + `from
# __future__ import annotations`): idempotente, seguro sobre cualquier BaseModel del módulo
# (incluida `TradeSpec`, importada, ya construida en su propio módulo).
for _name, _cls in list(globals().items()):
    if isinstance(_cls, type) and issubclass(_cls, BaseModel) and _cls is not BaseModel:
        _cls.model_rebuild()
del _name, _cls
