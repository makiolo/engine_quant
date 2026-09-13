#pragma once

#include <unordered_map>
#include <vector>

#include "engine/payoff/errors.hpp"
#include "engine/payoff/ids.hpp"

namespace engine {
namespace payoff {

// Ruta de mercado completamente conocida (PLAN_PRODUCTS.md §5.2, ADR-P0-08): fixings de
// observables, discount factors y tipos de cambio para una única trayectoria determinista.
// Una ausencia es error explícito (`EvaluationError`), nunca 0.0 silencioso (§3.1). Las
// búsquedas por tiempo usan `time_equal` (ADR-P0-01), nunca `==` de `double`.
class MarketPath {
public:
    void set_fixing(ObservableId observable, TimePoint time, double value);
    bool has_fixing(const ObservableId& observable, TimePoint time) const;
    double fixing(const ObservableId& observable, TimePoint time, const NodePath& path) const;

    // Todos los valores registrados para `observable` con tiempo <= `cutoff` (inclusive,
    // ADR-P0-01), en el orden en que se registraron. Usado por `RunningMin`/`RunningMax`
    // (ADR-P0-08: cálculo puro sobre la ruta completa conocida, sin acumulador incremental).
    std::vector<double> fixings_up_to(const ObservableId& observable, TimePoint cutoff) const;

    void set_discount_factor(CurveId curve, TimePoint from, TimePoint to, double value);
    double discount_factor(const CurveId& curve, TimePoint from, TimePoint to, const NodePath& path) const;

    void set_fx_rate(Currency from_currency, Currency to_currency, TimePoint time, double value);
    double fx_rate(
        const Currency& from_currency, const Currency& to_currency, TimePoint time, const NodePath& path
    ) const;

    // Copia con `curve` desplazada en paralelo por `zero_rate_bump` (tipo cero, PLAN_PRODUCTS.md
    // §12 Fase 4 "bump-and-reval generico por observable/curva", mismo bump de zero_rate que
    // `bump_curve`/`compute_dv01` en measure.cpp): cada entrada registrada de `curve` se
    // reescala por exp(-zero_rate_bump * (to - from)). Curvas distintas y todos los
    // fixings/fx_rates quedan intactos.
    MarketPath with_curve_bump(const CurveId& curve, double zero_rate_bump) const;

    // Copia con todos los fixings registrados de `observable` desplazados aditivamente por
    // `bump` (Delta de un observable de spot/subyacente, PLAN_PRODUCTS.md §12 Fase 4).
    MarketPath with_fixing_bump(const ObservableId& observable, double bump) const;

private:
    struct FixingEntry {
        TimePoint time;
        double value;
    };
    struct DiscountEntry {
        TimePoint from;
        TimePoint to;
        double value;
    };
    struct FxEntry {
        Currency from_currency;
        Currency to_currency;
        TimePoint time;
        double value;
    };

    std::unordered_map<ObservableId, std::vector<FixingEntry>> fixings_;
    std::unordered_map<CurveId, std::vector<DiscountEntry>> discount_factors_;
    std::vector<FxEntry> fx_rates_;
};

// Fixings históricos (§5.2 `EvaluationContext::historical_fixings`). Misma forma que la parte
// de fixings de `MarketPath`; la precedencia entre ambos (histórico gana sobre simulado para
// el mismo `(observable, time)`, ADR-P0-08) la aplica `ScenarioEvaluator`, no esta clase.
class FixingStore {
public:
    void set(ObservableId observable, TimePoint time, double value);
    bool has(const ObservableId& observable, TimePoint time) const;
    double get(const ObservableId& observable, TimePoint time, const NodePath& path) const;

private:
    struct Entry {
        TimePoint time;
        double value;
    };
    std::unordered_map<ObservableId, std::vector<Entry>> values_;
};

} // namespace payoff
} // namespace engine
