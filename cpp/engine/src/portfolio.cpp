#include "engine/portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace engine {

void Portfolio::add(std::shared_ptr<const IProduct> trade) { trades_.push_back(std::move(trade)); }

std::size_t Portfolio::size() const { return trades_.size(); }

const std::vector<std::shared_ptr<const IProduct>>& Portfolio::trades() const { return trades_; }

PriceBatchResult Portfolio::price(
    const Registries& registries, const std::vector<std::string>& measures, const IModel& model,
    const MarketSnapshot& market, const PricingContext& pricing, const ExecutionContext& execution
) const {
    std::vector<const IProduct*> ptrs;
    ptrs.reserve(trades_.size());
    for (const auto& trade : trades_) ptrs.push_back(trade.get());
    return price_many(registries, ptrs, measures, model, market, pricing, execution);
}

namespace {

// Compara dos RiskFactor por contenido -- mismo criterio estructural que
// `engine::greeks::same_risk_factor` (greeks.cpp, PLAN_BACKWARD.md §9 Fase 1-3), replicado aqui
// (funcion pequeña) en vez de exponerlo en greeks.hpp: en greeks.cpp vive en el namespace
// `engine::greeks` sin declaracion en el header, asi que no es alcanzable desde otra unidad de
// traduccion sin anadir una declaracion nueva al header solo para este uso.
bool same_risk_factor(const greeks::RiskFactor& lhs, const greeks::RiskFactor& rhs) {
    return lhs.kind == rhs.kind && lhs.scope == rhs.scope && lhs.name == rhs.name && lhs.pillar_index == rhs.pillar_index;
}

const greeks::HessianEntry* find_hessian_entry(
    const std::vector<greeks::HessianEntry>& entries, const greeks::RiskFactor& fi, const greeks::RiskFactor& fj
) {
    for (const greeks::HessianEntry& entry : entries) {
        if ((same_risk_factor(entry.factor_i, fi) && same_risk_factor(entry.factor_j, fj)) ||
            (same_risk_factor(entry.factor_i, fj) && same_risk_factor(entry.factor_j, fi))) {
            return &entry;
        }
    }
    return nullptr;
}

const greeks::HvpComponent* find_hvp_component(
    const std::vector<greeks::HvpComponent>& components, const greeks::RiskFactor& factor
) {
    for (const greeks::HvpComponent& component : components) {
        if (same_risk_factor(component.factor, factor)) return &component;
    }
    return nullptr;
}

std::string index_list(const std::vector<std::size_t>& indices) {
    std::string out;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(indices[i]);
    }
    return out;
}

} // namespace

greeks::HessianReport Portfolio::hessian(
    const Registries& registries, const std::string& metric_name, const Params& metric_params, const IModel& model,
    const MarketSnapshot& market, const PricingContext& pricing, const ExecutionContext& execution,
    const std::vector<greeks::RiskFactor>& factors
) const {
    greeks::HessianReport report;
    if (trades_.empty()) return report;

    // Un HessianReport por trade (PLAN_BACKWARD.md §9 Fase 6, paso 1) -- mismos `factors` para
    // todos, mismo model/market/pricing/execution compartidos (alcance de esta fase).
    std::vector<greeks::HessianReport> per_trade;
    per_trade.reserve(trades_.size());
    for (const std::shared_ptr<const IProduct>& trade : trades_) {
        per_trade.push_back(greeks::compute_hessian(
            registries, metric_name, metric_params, model, *trade, market, pricing, execution, factors
        ));
    }

    // Union de todos los pares (factor_i,factor_j) que aparecen en AL MENOS un report_k --
    // examinada abajo para quedarse solo con los que aparecen en TODOS (interseccion real, paso
    // 2 del plan); mantener la union primero permite diagnosticar exactamente que trade(s) faltan
    // para cada par, en vez de solo saber que "algun par no estaba en todos".
    struct PairKey {
        greeks::RiskFactor fi;
        greeks::RiskFactor fj;
    };
    std::vector<PairKey> all_pairs;
    for (const greeks::HessianReport& rep : per_trade) {
        for (const greeks::HessianEntry& entry : rep.entries) {
            bool seen = std::any_of(all_pairs.begin(), all_pairs.end(), [&](const PairKey& p) {
                return (same_risk_factor(p.fi, entry.factor_i) && same_risk_factor(p.fj, entry.factor_j)) ||
                       (same_risk_factor(p.fi, entry.factor_j) && same_risk_factor(p.fj, entry.factor_i));
            });
            if (!seen) all_pairs.push_back(PairKey{entry.factor_i, entry.factor_j});
        }
    }

    for (const PairKey& pair : all_pairs) {
        std::vector<const greeks::HessianEntry*> contributions;
        std::vector<std::size_t> missing_trades;
        for (std::size_t k = 0; k < per_trade.size(); ++k) {
            const greeks::HessianEntry* entry = find_hessian_entry(per_trade[k].entries, pair.fi, pair.fj);
            if (entry) {
                contributions.push_back(entry);
            } else {
                missing_trades.push_back(k);
            }
        }

        if (!missing_trades.empty()) {
            // Paso 4 del plan: NUNCA sumar como si el trade que falta aportara 0.0 -- se omite de
            // `entries` y se documenta el par y los indices de trade exactos donde falto.
            report.skipped.push_back(
                "Portfolio::hessian: el par (" + greeks::to_string(pair.fi) + ", " + greeks::to_string(pair.fj) +
                ") no aparece en HessianReport::entries de los trade(s) [" + index_list(missing_trades) +
                "] (indice dentro de este Portfolio) -- omitido de la suma; ver HessianReport::skipped de "
                "compute_hessian sobre ese/esos trade(s) para el motivo exacto"
            );
            continue;
        }

        // Paso 3 del plan: sumar `.value`; `method_used`/`measure` del primer trade que la
        // aporto (deberian coincidir entre trades en una cartera homogenea bajo el mismo
        // modelo/metrica, no se verifica explicitamente); `std_error` en cuadratura SOLO si TODOS
        // los trades que aportan esta entrada lo tienen (Monte Carlo/likelihood ratio) --
        // asumiendo independencia entre las simulaciones de trades distintos (documentado en
        // portfolio.hpp); ausente si CUALQUIERA no lo tiene (formula cerrada, Hull-White): mezclar
        // MC con cerrado en la misma entrada no tiene una propagacion de error limpia.
        double sum = 0.0;
        double variance_sum = 0.0;
        bool all_have_std_error = true;
        for (const greeks::HessianEntry* entry : contributions) {
            sum += entry->value;
            if (entry->std_error.has_value()) {
                variance_sum += (*entry->std_error) * (*entry->std_error);
            } else {
                all_have_std_error = false;
            }
        }

        greeks::HessianEntry aggregated;
        aggregated.factor_i = pair.fi;
        aggregated.factor_j = pair.fj;
        aggregated.value = sum;
        aggregated.method_used = contributions.front()->method_used;
        aggregated.measure = contributions.front()->measure;
        if (all_have_std_error) aggregated.std_error = std::sqrt(variance_sum);
        report.entries.push_back(aggregated);
    }

    // Los motivos de `skipped` que YA trae cada compute_hessian individual (combinacion no
    // soportada, factor pedido fuera de los soportados, ...) se conservan tal cual, prefijados
    // con el indice del trade -- diagnosticable sin tener que repetir las llamadas trade a trade.
    for (std::size_t k = 0; k < per_trade.size(); ++k) {
        for (const std::string& reason : per_trade[k].skipped) {
            report.skipped.push_back("Portfolio::hessian: trade " + std::to_string(k) + ": " + reason);
        }
    }

    return report;
}

greeks::HvpReport Portfolio::hvp(
    const Registries& registries, const std::string& metric_name, const Params& metric_params, const IModel& model,
    const MarketSnapshot& market, const PricingContext& pricing, const ExecutionContext& execution,
    const std::vector<greeks::RiskFactor>& factors, const std::vector<double>& direction
) const {
    greeks::HvpReport report;
    if (trades_.empty()) return report;

    std::vector<greeks::HvpReport> per_trade;
    per_trade.reserve(trades_.size());
    for (const std::shared_ptr<const IProduct>& trade : trades_) {
        per_trade.push_back(greeks::compute_hvp(
            registries, metric_name, metric_params, model, *trade, market, pricing, execution, factors, direction
        ));
    }

    std::vector<greeks::RiskFactor> all_factors;
    for (const greeks::HvpReport& rep : per_trade) {
        for (const greeks::HvpComponent& component : rep.components) {
            bool seen = std::any_of(all_factors.begin(), all_factors.end(), [&](const greeks::RiskFactor& f) {
                return same_risk_factor(f, component.factor);
            });
            if (!seen) all_factors.push_back(component.factor);
        }
    }

    for (const greeks::RiskFactor& factor : all_factors) {
        std::vector<const greeks::HvpComponent*> contributions;
        std::vector<std::size_t> missing_trades;
        for (std::size_t k = 0; k < per_trade.size(); ++k) {
            const greeks::HvpComponent* component = find_hvp_component(per_trade[k].components, factor);
            if (component) {
                contributions.push_back(component);
            } else {
                missing_trades.push_back(k);
            }
        }

        if (!missing_trades.empty()) {
            report.skipped.push_back(
                "Portfolio::hvp: el factor '" + greeks::to_string(factor) + "' no aparece en HvpReport::components de "
                "los trade(s) [" + index_list(missing_trades) + "] (indice dentro de este Portfolio) -- omitido de la "
                "suma; ver HvpReport::skipped de compute_hvp sobre ese/esos trade(s) para el motivo exacto"
            );
            continue;
        }

        double sum = 0.0;
        for (const greeks::HvpComponent* component : contributions) sum += component->value;
        report.components.push_back(greeks::HvpComponent{factor, sum, contributions.front()->method_used});
    }

    for (std::size_t k = 0; k < per_trade.size(); ++k) {
        for (const std::string& reason : per_trade[k].skipped) {
            report.skipped.push_back("Portfolio::hvp: trade " + std::to_string(k) + ": " + reason);
        }
    }

    return report;
}

} // namespace engine
