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

} // namespace engine
