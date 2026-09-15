#include "engine/payoff/market_snapshot_bridge.hpp"

#include <stdexcept>

namespace engine {
namespace payoff {

namespace {

// Id de curva interno, invisible al llamante: `MarketSnapshot` solo trae una curva, así que no
// hace falta (ni tiene sentido) que el llamante elija un `CurveId` -- ver el doc-comment del
// header.
const CurveId& single_curve_id() {
    static const CurveId id{"MARKET_SNAPSHOT.DISCOUNT"};
    return id;
}

// Evalúa el ledger sobre un `MarketPath` vacío (sin fixings ni discount factors registrados):
// suficiente para un payoff dentro del alcance de este puente (ningún `Fixing`/`Current`/
// `Average`/`RunningMin`/`RunningMax`, ninguna decisión de `Trigger`/`If` que dependa de un
// observable) -- cualquier referencia a algo que `MarketSnapshot` no modela lanza
// `EvaluationError` aquí, antes de intentar descontar nada.
CashflowLedger evaluate_ledger_currency_probe(const ContractPtr& root) {
    MarketPath empty_path;
    FixingStore empty_fixings;
    RuntimeState state;
    EvaluationContext context{empty_path, empty_fixings, state};
    return evaluate_cashflows(root, context);
}

// Única moneda usada por `ledger`, o `std::invalid_argument` si hay más de una o ninguna --
// alcance de este puente, ver el doc-comment del header.
Currency single_currency_of(const CashflowLedger& ledger) {
    if (ledger.empty()) {
        throw std::invalid_argument(
            "market_snapshot_bridge: el contrato no genera ningún cashflow, no hay moneda que resolver"
        );
    }
    const Currency& currency = ledger.front().currency;
    for (const LedgerEntry& entry : ledger) {
        if (entry.currency != currency) {
            throw std::invalid_argument(
                "market_snapshot_bridge: el contrato usa más de una moneda ('" + currency.code + "', '" +
                entry.currency.code +
                "'); MarketSnapshot solo tiene una curva -- fuera del alcance de este puente (ver "
                "PLAN_FXFORWARD.md §3.2 para el caso multi-moneda)"
            );
        }
    }
    return currency;
}

// `MarketPath` poblado con el discount factor de `market` en cada fecha de pago de `ledger`
// (más t=0, por si algún cashflow se paga ahí) bajo `single_curve_id()`.
MarketPath market_path_from_snapshot(const MarketSnapshot& market, const CashflowLedger& ledger) {
    MarketPath path;
    const TimePoint origin{0.0};
    path.set_discount_factor(single_curve_id(), origin, origin, market.discount_factor(0.0));
    for (const LedgerEntry& entry : ledger) {
        path.set_discount_factor(single_curve_id(), origin, entry.payment_time, market.discount_factor(entry.payment_time.year_fraction));
    }
    return path;
}

} // namespace

ValuationResult present_value_from_market_snapshot(const ContractPtr& root, const MarketSnapshot& market) {
    CashflowLedger probe_ledger = evaluate_ledger_currency_probe(root);
    Currency currency = single_currency_of(probe_ledger);

    MarketPath path = market_path_from_snapshot(market, probe_ledger);
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    DiscountingPolicy discounting(currency, single_curve_id());

    return present_value(root, context, discounting, currency);
}

double bump_and_reval_from_market_snapshot(const ContractPtr& root, const MarketSnapshot& market, double zero_rate_bump) {
    double base = present_value_from_market_snapshot(root, market).present_value;
    double bumped = present_value_from_market_snapshot(root, bump_market_parallel(market, zero_rate_bump)).present_value;
    return bumped - base;
}

double bump_and_reval_pillar_from_market_snapshot(
    const ContractPtr& root, const MarketSnapshot& market, std::size_t pillar_index, double zero_rate_bump
) {
    double base = present_value_from_market_snapshot(root, market).present_value;
    double bumped =
        present_value_from_market_snapshot(root, bump_market_pillar(market, pillar_index, zero_rate_bump)).present_value;
    return bumped - base;
}

} // namespace payoff
} // namespace engine
