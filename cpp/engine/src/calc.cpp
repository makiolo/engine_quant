#include "engine/calc.hpp"

#include <stdexcept>
#include <unordered_map>

namespace engine {

namespace {

enum class Field { Scalar, Primary, Secondary };

struct MeasureNameMapping {
    std::string registered_type;
    Field field;
};

// Único lugar que traduce el vocabulario de ENGINE.CALC al `Registry<IMeasure>` subyacente
// (PLAN.md §7.15). Añadir una medida nueva a CALC es una fila más aquí -- no hace falta
// tocar Python/Excel/la C ABI, que solo llaman a `engine::calc`.
const std::unordered_map<std::string, MeasureNameMapping>& calc_measure_name_mappings() {
    static const std::unordered_map<std::string, MeasureNameMapping> mappings = {
        {"PV", {"PV", Field::Scalar}},
        {"DV01", {"DV01", Field::Scalar}},
        {"ExpectedExposure", {"ExposureProfile", Field::Primary}},
        {"PFE95", {"ExposureProfile", Field::Secondary}},
        {"UnilateralCVA", {"UnilateralCVA", Field::Scalar}},
    };
    return mappings;
}

// Recorta el MeasureResult de la medida registrada subyacente al campo que le corresponde a
// un nombre concreto de CALC -- PFE95 expone su serie en `.primary` de su propio resultado
// (no en `.secondary`), para que cada entrada nombrada del lote tenga la misma forma sin que
// el consumidor tenga que saber de qué campo interno viene.
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

// Evalúa una medida registrada subyacente (PV/DV01/ExposureProfile/UnilateralCVA) sobre el
// lote entero de una vez -- equivalente de lote de `IMeasure::evaluate`, pero llamando
// directamente a los `compute_*_batch` de measure.hpp en vez de pasar por
// `Registry<IMeasure>`: con un único producto real no hay genericidad que ganar todavía con un
// método virtual `evaluate_batch` (mismo argumento que ya justificó no generalizar la C ABI de
// calibración hasta el segundo calibrador, PLAN.md §7.18).
std::vector<MeasureResult> evaluate_batch_registered_measure(
    const std::string& registered_type,
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
        std::vector<double> pvs = compute_npv_batch(model, irs_products);
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
        std::vector<double> deltas = compute_npv_delta_r0_batch(model, irs_products);
        std::vector<MeasureResult> results;
        results.reserve(deltas.size());
        for (double delta : deltas) {
            MeasureResult r;
            r.has_scalar = true;
            r.scalar = delta * 0.0001; // 1 punto básico, igual que Dv01Measure::evaluate
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

} // namespace

std::vector<std::string> calc_measure_names() {
    std::vector<std::string> names;
    names.reserve(calc_measure_name_mappings().size());
    for (const auto& [name, mapping] : calc_measure_name_mappings()) {
        names.push_back(name);
    }
    return names;
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
    const auto& mappings = calc_measure_name_mappings();

    // Falla rápido: valida todos los nombres antes de calcular nada.
    for (const std::string& name : measure_names) {
        if (mappings.find(name) == mappings.end()) {
            throw std::invalid_argument("engine::calc: medida desconocida: '" + name + "'");
        }
    }

    // Evalúa cada medida registrada subyacente como mucho una vez, aunque varios nombres de
    // CALC la pidan (p.ej. "ExpectedExposure"+"PFE95" comparten "ExposureProfile" -- una sola
    // simulación Monte Carlo para ambas, no dos).
    std::unordered_map<std::string, MeasureResult> computed;
    for (const std::string& name : measure_names) {
        const std::string& registered_type = mappings.at(name).registered_type;
        if (computed.find(registered_type) == computed.end()) {
            auto measure = registries.measures.create(registered_type);
            computed.emplace(registered_type, measure->evaluate(model, product, market, pricing, execution));
        }
    }

    CalcResult result;
    result.reserve(measure_names.size());
    for (const std::string& name : measure_names) {
        const MeasureNameMapping& mapping = mappings.at(name);
        result.push_back(CalcResultEntry{name, extract_field(computed.at(mapping.registered_type), mapping.field)});
    }
    return result;
}

CalcBatchResult calc_batch(
    const Registries&,
    const std::vector<const IProduct*>& products,
    const std::vector<std::string>& measure_names,
    const IModel& model,
    const MarketSnapshot& market,
    const PricingContext& pricing,
    const ExecutionContext& execution
) {
    const auto& mappings = calc_measure_name_mappings();
    for (const std::string& name : measure_names) {
        if (mappings.find(name) == mappings.end()) {
            throw std::invalid_argument("engine::calc_batch: medida desconocida: '" + name + "'");
        }
    }

    std::vector<const IrSwapProduct*> irs_products = require_homogeneous_irs_batch(products);

    // Evalúa cada medida registrada subyacente una única vez para TODO el lote, igual que
    // `calc()` la evalúa una única vez por trade -- "ExpectedExposure"+"PFE95" comparten una
    // sola simulación Monte Carlo del lote entero, no una por trade ni una por nombre.
    std::unordered_map<std::string, std::vector<MeasureResult>> computed;
    for (const std::string& name : measure_names) {
        const std::string& registered_type = mappings.at(name).registered_type;
        if (computed.find(registered_type) == computed.end()) {
            computed.emplace(
                registered_type, evaluate_batch_registered_measure(registered_type, model, irs_products, market, pricing, execution)
            );
        }
    }

    CalcBatchResult result;
    result.reserve(irs_products.size());
    for (std::size_t i = 0; i < irs_products.size(); ++i) {
        CalcResult measures;
        measures.reserve(measure_names.size());
        for (const std::string& name : measure_names) {
            const MeasureNameMapping& mapping = mappings.at(name);
            measures.push_back(CalcResultEntry{name, extract_field(computed.at(mapping.registered_type)[i], mapping.field)});
        }
        result.push_back(CalcBatchResultEntry{i, std::move(measures)});
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

        CalcBatchResult group_result = calc_batch(registries, group_products, measure_names, model, market, pricing, execution);
        for (std::size_t j = 0; j < indices.size(); ++j) {
            result[indices[j]] = CalcBatchResultEntry{indices[j], std::move(group_result[j].measures)};
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
    if (models.empty() || markets.empty()) {
        throw std::invalid_argument("engine::calc_grid: models y markets no pueden estar vacíos");
    }

    CalcGridResult result;
    result.reserve(products.size() * models.size() * markets.size());
    for (std::size_t m = 0; m < models.size(); ++m) {
        for (std::size_t k = 0; k < markets.size(); ++k) {
            CalcBatchResult many_result = calc_many(registries, products, measure_names, *models[m], markets[k], pricing, execution);
            for (auto& entry : many_result) {
                result.push_back(CalcGridResultEntry{entry.trade_index, m, k, std::move(entry.measures)});
            }
        }
    }
    return result;
}

} // namespace engine
