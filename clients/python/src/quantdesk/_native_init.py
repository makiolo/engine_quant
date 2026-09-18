"""Lazy native facade imports for :mod:`quantdesk`.

Kept separate so importing the pure REST SDK does not require the optional nanobind
``engine`` extension.  ``quantdesk.__getattr__`` loads this module only on demand.
"""

import engine as _native

from quantdesk import greeks
from quantdesk.context import ExecutionContext, PricingContext
from quantdesk.engine import BatchRow, Engine, GridRow, PriceResult
from quantdesk.greeks import Greek
from quantdesk.market import Market
from quantdesk.measure import DV01, PV, ExposureProfile, Measure, UnilateralCVA
from quantdesk.model import Gbm, GbmBasket, GbmP, HullWhite1F, HullWhite2F, ModelSpec
from quantdesk.payoff import (
    Contract,
    PayoffProduct,
    Predicate,
    ScalarExpr,
    abs,
    add,
    after,
    all_of,
    any_of,
    average,
    before,
    between,
    both,
    call_leg,
    cashflow,
    clamp,
    constant,
    current,
    custom_strategy,
    discount_factor,
    div,
    eq,
    european_call,
    european_put,
    event_occurred,
    event_time,
    event_value,
    exercise,
    exp,
    fixing,
    fx_conversion,
    fx_forward,
    give,
    greater,
    greater_equal,
    if_,
    irs,
    less,
    less_equal,
    log,
    maximum,
    minimum,
    mul,
    neg,
    negate,
    parameter,
    pow,
    put_leg,
    running_max,
    running_min,
    scale,
    sub,
    trigger,
    when,
    zero,
)
from quantdesk.trade import IRSwap, PAR, TradeSpec

# Reexport directo del binding nanobind, conservando la API histórica.
Portfolio = _native.Portfolio
