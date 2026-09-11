#include "engine/calc.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace engine {

namespace {

enum class Field { Scalar, Primary, Secondary };

struct ResolvedMeasure {
    std::string registered_type;
    Field field;
};

// Los únicos dos nombres de ENGINE.CALC que no coinciden con un tipo registrado en
// `Registry<IMeasure>` (PLAN_REAPI.md §6 Fase 3): "ExpectedExposure"/"PFE95" heredados de
// PLAN.md §7.15, que exponen `.primary`/`.secondary` de la misma `ExposureProfileMeasure`
// bajo dos nombres de cara al usuario. Cualquier otro nombre se resuelve directamente contra
// el registry (ver resolve_measure_name) -- p.ej. pedir "ExposureProfile" a secas ya funciona
// hoy y devuelve el `MeasureResult` completo (times + primary=EE + secondary=PFE95), sin
// recortar campos.
const std::unordered_map<std::string, ResolvedMeasure>& legacy_field_aliases() {
    static const std::unordered_map<std::string, ResolvedMeasure> aliases = {
        {"ExpectedExposure", {"ExposureProfile", Field::Primary}},
        {"PFE95", {"ExposureProfile", Field::Secondary}},
    };
    return aliases;
}

// Recorta el MeasureResult de la medida registrada subyacente al campo que le corresponde a
// un nombre concreto de CALC -- PFE95 expone su serie en `.primary` de su propio resultado
// (no en `.secondary`), para que cada entrada nombrada del lote tenga la misma forma sin que
// el consumidor tenga que saber de qué campo interno viene. Field::Scalar es identidad: los
// nombres resueltos directamente contra el registry (el caso general) devuelven el
// MeasureResult completo tal cual lo calculó la medida.
MeasureResult extract_field(const MeasureResult& source, Field field) {
    switch (field) {
        case Field::Scalar:
            return source;
        case Field::Primary: {
            MeasureResult out;
            out.times = source.times;
            out.primary = source.primary;
            return out;
        }
        case Field::Secondary: {
            MeasureResult out;
            out.times = source.times;
            out.primary = source.secondary;
            return out;
        }
    }
    throw std::logic_error("engine::calc: Field desconocido"); // inalcanzable
}

// Resuelve un nombre de MeasureSpec a (tipo registrado, campo a extraer) para `calc()`
// (PLAN_REAPI.md §6 Fase 3): los dos alias heredados primero, cualquier otro nombre se pasa
// tal cual al registry -- `calc()` ya no está limitado a una tabla curada cerrada.
ResolvedMeasure resolve_measure_name(const std::string& name) {
    const auto& aliases = legacy_field_aliases();
    auto it = aliases.find(name);
    if (it != aliases.end()) return it->second;
    return ResolvedMeasure{name, Field::Scalar};
}

// Serialización determinista de un Params para usarlo como parte de una clave de caché
// (PLAN_REAPI.md §6 Fase 3): dos MeasureSpec con el mismo nombre pero distinta configuración
// (p.ej. DV01(bump=0.0001) y DV01(bump=0.0002) en el mismo calc()) deben evaluarse por
// separado -- antes de MeasureSpec, "computed" solo se indexaba por nombre porque los
// measure_names no llevaban configuración.
std::string params_cache_key(const Params& params) {
    std::vector<std::string> parts;
    parts.reserve(params.size());
    for (const auto& [key, value] : params) {
        std::string part = key;
        part += '=';
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, double>) {
                    part += std::to_string(v);
                } else if constexpr (std::is_same_v<T, bool>) {
                    part += v ? "true" : "false";
                } else if constexpr (std::is_same_v<T, std::string>) {
                    part += v;
                } else {
                    for (double d : v) {
                        part += std::to_string(d);
                        part += ',';
                    }
                }
            },
            value
        );
        parts.push_back(std::move(part));
    }
    std::sort(parts.begin(), parts.end());
    std::string key;
    for (const std::string& part : parts) {
        key += part;
        key += '|';
    }
    return key;
}

// Valida que `products` sea un lote homogéneo válido para `calc_batch` (PLAN.md §7.19): mismo
// tipo registrado, todos `IrSwapProduct`, mismo calendario, sin `use_par_rate`. Devuelve el
// lote ya reinterpretado como `IrSwapProduct*` para que el resto de `calc_batch` no repita el
// `dynamic_cast`.
std::vector<const IrSwapProduct*> require_homogeneous_irs_batch(const std::vector<const IProduct*>& products) {
    if (products.empty()) {
        throw std::invalid_argument("engine::calc_batch: el lote no puede estar vacío");
    }

    std::vector<const IrSwapProduct*> irs_products;
    irs_products.reserve(products.size());
    const std::string& type_name = products.front()->type_name();
    for (const IProduct* p : products) {
        if (p->type_name() != type_name) {
            throw std::invalid_argument(
                "engine::calc_batch: todos los trades del lote deben ser del mismo tipo de producto ('" +
                type_name + "' vs '" + p->type_name() + "')"
            );
        }
        const auto* irs = dynamic_cast<const IrSwapProduct*>(p);
        if (!irs) {
            throw std::invalid_argument("engine::calc_batch: tipo de producto no soportado para lote: " + type_name);
        }
        if (irs->use_par_rate()) {
            throw std::invalid_argument(
                "engine::calc_batch: los trades del lote deben traer fixed_rate explícito (use_par_rate no soportado en lote)"
            );
        }
        irs_products.push_back(irs);
    }

    const IrSwapProduct& first = *irs_products.front();
    for (const auto* irs : irs_products) {
        if (irs->start() != first.start() || irs->payment_times() != first.payment_times() ||
            irs->accruals() != first.accruals()) {
            throw std::invalid_argument(
                "engine::calc_batch: todos los trades del lote deben compartir calendario (start/payment_times/accruals)"
            );
        }
    }
    return irs_products;
}

// A diferencia de `calc()`, el lote sigue limitado a las medidas que sabe evaluar
// `evaluate_batch_registered_measure` (ver comentario de `calc_batch` en calc.hpp) -- valida
// contra este conjunto cerrado en vez de contra `registries.measures` (que sí acepta medidas
// sin equivalente de lote todavía).
const std::unordered_set<std::string>& known_batch_registered_types() {
    static const std::unordered_set<std::string> known = {"PV", "DV01", "UnilateralCVA", "ExposureProfile"};
    return known;
}

ResolvedMeasure resolve_batch_measure_name(const std::string& name) {
    ResolvedMeasure resolved = resolve_measure_name(name);
    if (known_batch_registered_types().find(resolved.registered_type) == known_batch_registered_types().end()) {
        throw std::invalid_argument("engine::calc_batch: medida desconocida o no soportada en lote: '" + name + "'");
    }
    return resolved;
}

// Evalúa una medida registrada subyacente (PV/DV01/ExposureProfile/UnilateralCVA) sobre el
// lote entero de una vez -- equivalente de lote de `IMeasure::evaluate`, pero llamando
// directamente a los `compute_*_batch` de measure.hpp en vez de pasar por
// `Registry<IMeasure>`: con un único producto real no hay genericidad que ganar todavía con un
// método virtual `evaluate_batch` (mismo argumento que ya justificó no generalizar la C ABI de
// calibración hasta el segundo calibrador, PLAN.md §7.18). `params` (PLAN_REAPI.md §6 Fase 3):
// solo "DV01" lo consume ("bump", default 0.0001, igual que Dv01Measure::evaluate).
std::vector<MeasureResult> evaluate_batch_registered_measure(
    const std::string& registered_type,
    const Params& params,
    const IModel& model,
    const std::vector<const IrSwapProduct*>& irs_products,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    if (registered_type == "ExposureProfile") {
        std::vector<ExposureProfile> profiles = compute_exposure_profile_batch(model, irs_products, pricing, execution);
        std::vector<MeasureResult> results;
        results.reserve(profiles.size());
        for (auto& profile : profiles) {
            MeasureResult r;
            r.times = std::move(profile.times);
            r.primary = std::move(profile.ee);
            r.secondary = std::move(profile.pfe_95);
            results.push_back(std::move(r));
        }
        return results;
    }
    if (registered_type == "UnilateralCVA") {
        std::vector<ExposureProfile> profiles = compute_exposure_profile_batch(model, irs_products, pricing, execution);
        std::vector<double> cvas =
            compute_cva_from_exposure_batch(model, execution, profiles, market.hazard_rate(), market.recovery_rate());
        std::vector<MeasureResult> results;
        results.reserve(cvas.size());
        for (double cva : cvas) {
            MeasureResult r;
            r.has_scalar = true;
            r.scalar = cva;
            results.push_back(r);
        }
        return results;
    }
    if (registered_type == "PV") {
        std::vector<double> pvs = compute_npv_batch(market, irs_products);
        std::vector<MeasureResult> results;
        results.reserve(pvs.size());
        for (double pv : pvs) {
            MeasureResult r;
            r.has_scalar = true;
            r.scalar = pv;
            results.push_back(r);
        }
        return results;
    }
    if (registered_type == "DV01") {
        double bump = get_double(params, "bump", 0.0001);
        // PLAN_REAPI.md §6 Fase 5: bucketed=true en el lote se comporta igual que en
        // Dv01Measure::evaluate -- un vector de deltas (uno por pillar) en vez de un escalar.
        if (get_bool(params, "bucketed", false)) {
            std::vector<std::vector<double>> deltas_per_trade = compute_dv01_bucketed_batch(market, irs_products, bump);
            std::vector<MeasureResult> results;
            results.reserve(deltas_per_trade.size());
            for (auto& deltas : deltas_per_trade) {
                MeasureResult r;
                r.times = market.pillars();
                r.primary = std::move(deltas);
                results.push_back(std::move(r));
            }
            return results;
        }
        std::vector<double> deltas = compute_dv01_batch(market, irs_products, bump);
        std::vector<MeasureResult> results;
        results.reserve(deltas.size());
        for (double delta : deltas) {
            MeasureResult r;
            r.has_scalar = true;
            r.scalar = delta;
            results.push_back(r);
        }
        return results;
    }
    throw std::logic_error("engine::calc_batch: registered_type desconocido: " + registered_type); // inalcanzable
}

// Clave de agrupación de calendario para `calc_many` (PLAN.md §7.19) -- mismo espíritu que la
// clave canónica de `HandleRegistry` en Excel (`clients/excel/src/handles.cpp`): un string
// formateado a partir de los campos que definen "mismo calendario", sin más maquinaria.
std::string calendar_group_key(const IrSwapProduct& irs) {
    std::string key = std::to_string(irs.start());
    key += '|';
    for (double t : irs.payment_times()) {
        key += std::to_string(t);
        key += ',';
    }
    key += '|';
    for (double t : irs.accruals()) {
        key += std::to_string(t);
        key += ',';
    }
    return key;
}

std::vector<MeasureSpec> to_measure_specs(const std::vector<std::string>& measure_names) {
    std::vector<MeasureSpec> measures;
    measures.reserve(measure_names.size());
    for (const std::string& name : measure_names) {
        measures.push_back(MeasureSpec{name, Params{}});
    }
    return measures;
}

} // namespace

std::vector<std::string> calc_measure_names(const Registries& registries) {
    std::vector<std::string> names = registries.measures.list();
    for (const auto& [alias, mapping] : legacy_field_aliases()) {
        names.push_back(alias);
    }
    return names;
}

CalcResult calc(
    const Registries& registries,
    const IProduct& product,
    const std::vector<MeasureSpec>& measures,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    // Falla rápido: resuelve y valida todas las specs antes de calcular nada.
    std::vector<ResolvedMeasure> resolved;
    resolved.reserve(measures.size());
    for (const MeasureSpec& spec : measures) {
        ResolvedMeasure r = resolve_measure_name(spec.name);
        if (!registries.measures.contains(r.registered_type)) {
            throw std::invalid_argument("engine::calc: medida desconocida: '" + spec.name + "'");
        }
        resolved.push_back(std::move(r));
    }

    // Evalúa cada (medida registrada, configuración) subyacente como mucho una vez, aunque
    // varios nombres de CALC la pidan con la misma configuración (p.ej. "ExpectedExposure"+
    // "PFE95" comparten "ExposureProfile" -- una sola simulación Monte Carlo para ambas, no
    // dos; dos DV01 con distinto "bump" sí se evalúan por separado).
    std::unordered_map<std::string, MeasureResult> computed;
    for (std::size_t i = 0; i < measures.size(); ++i) {
        const std::string cache_key = resolved[i].registered_type + "#" + params_cache_key(measures[i].params);
        if (computed.find(cache_key) == computed.end()) {
            auto measure = registries.measures.create(resolved[i].registered_type, measures[i].params);
            computed.emplace(cache_key, measure->evaluate(model, product, market, pricing, execution));
        }
    }

    CalcResult result;
    result.reserve(measures.size());
    for (std::size_t i = 0; i < measures.size(); ++i) {
        const std::string cache_key = resolved[i].registered_type + "#" + params_cache_key(measures[i].params);
        result.push_back(CalcResultEntry{measures[i].name, extract_field(computed.at(cache_key), resolved[i].field)});
    }
    return result;
}

CalcResult calc(
    const Registries& registries,
    const IProduct& product,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    return calc(registries, product, to_measure_specs(measure_names), model, market, pricing, execution);
}

CalcBatchResult calc_batch(
    const Registries&,
    const std::vector<const IProduct*>& products,
    const std::vector<MeasureSpec>& measures,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    std::vector<ResolvedMeasure> resolved;
    resolved.reserve(measures.size());
    for (const MeasureSpec& spec : measures) {
        resolved.push_back(resolve_batch_measure_name(spec.name));
    }

    std::vector<const IrSwapProduct*> irs_products = require_homogeneous_irs_batch(products);

    // Evalúa cada (medida registrada, configuración) subyacente una única vez para TODO el
    // lote, igual que `calc()` la evalúa una única vez por trade.
    std::unordered_map<std::string, std::vector<MeasureResult>> computed;
    for (std::size_t i = 0; i < measures.size(); ++i) {
        const std::string cache_key = resolved[i].registered_type + "#" + params_cache_key(measures[i].params);
        if (computed.find(cache_key) == computed.end()) {
            computed.emplace(
                cache_key,
                evaluate_batch_registered_measure(
                    resolved[i].registered_type, measures[i].params, model, irs_products, market, pricing, execution
                )
            );
        }
    }

    CalcBatchResult result;
    result.reserve(irs_products.size());
    for (std::size_t i = 0; i < irs_products.size(); ++i) {
        CalcResult row;
        row.reserve(measures.size());
        for (std::size_t j = 0; j < measures.size(); ++j) {
            const std::string cache_key = resolved[j].registered_type + "#" + params_cache_key(measures[j].params);
            row.push_back(CalcResultEntry{measures[j].name, extract_field(computed.at(cache_key)[i], resolved[j].field)});
        }
        result.push_back(CalcBatchResultEntry{i, std::move(row)});
    }
    return result;
}

CalcBatchResult calc_batch(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    return calc_batch(registries, products, to_measure_specs(measure_names), model, market, pricing, execution);
}

CalcBatchResult calc_many(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<MeasureSpec>& measures,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    if (products.empty()) {
        throw std::invalid_argument("engine::calc_many: la lista de trades no puede estar vacía");
    }

    // Agrupa por (tipo de producto, calendario) -- Nivel 2 de PLAN.md §7.17: cada grupo se
    // resuelve con calc_batch, incluidos los grupos de tamaño 1 (sin caso especial). Con un
    // único tipo de producto real (IrSwapProduct) hoy, la única fuente de heterogeneidad real
    // es el calendario -- añadir un segundo producto no exigiría tocar esta función.
    std::unordered_map<std::string, std::vector<std::size_t>> groups;
    std::vector<std::string> group_order; // primera aparición, para un orden de evaluación determinista
    for (std::size_t i = 0; i < products.size(); ++i) {
        const IProduct& p = *products[i];
        std::string key = p.type_name();
        if (const auto* irs = dynamic_cast<const IrSwapProduct*>(&p)) {
            key += '#';
            key += calendar_group_key(*irs);
        }
        auto [it, inserted] = groups.try_emplace(key);
        if (inserted) group_order.push_back(key);
        it->second.push_back(i);
    }

    CalcBatchResult result(products.size());
    for (const std::string& key : group_order) {
        const std::vector<std::size_t>& indices = groups.at(key);
        std::vector<const IProduct*> group_products;
        group_products.reserve(indices.size());
        for (std::size_t idx : indices) group_products.push_back(products[idx]);

        CalcBatchResult group_result = calc_batch(registries, group_products, measures, model, market, pricing, execution);
        for (std::size_t j = 0; j < indices.size(); ++j) {
            result[indices[j]] = CalcBatchResultEntry{indices[j], std::move(group_result[j].measures)};
        }
    }
    return result;
}

CalcBatchResult calc_many(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    return calc_many(registries, products, to_measure_specs(measure_names), model, market, pricing, execution);
}

CalcGridResult calc_grid(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<MeasureSpec>& measures,
    const std::vector<const IModel*>& models,
    const std::vector<MarketSnapshot>& markets,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    if (models.empty() || markets.empty()) {
        throw std::invalid_argument("engine::calc_grid: models y markets no pueden estar vacíos");
    }

    CalcGridResult result;
    result.reserve(products.size() * models.size() * markets.size());
    for (std::size_t m = 0; m < models.size(); ++m) {
        for (std::size_t k = 0; k < markets.size(); ++k) {
            CalcBatchResult many_result = calc_many(registries, products, measures, *models[m], markets[k], pricing, execution);
            for (auto& entry : many_result) {
                result.push_back(CalcGridResultEntry{entry.trade_index, m, k, std::move(entry.measures)});
            }
        }
    }
    return result;
}

CalcGridResult calc_grid(
    const Registries& registries,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const std::vector<const IModel*>& models,
    const std::vector<MarketSnapshot>& markets,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    return calc_grid(registries, products, to_measure_specs(measure_names), models, markets, pricing, execution);
}

} // namespace engine
