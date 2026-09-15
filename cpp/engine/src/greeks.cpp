#include "engine/greeks.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <variant>

namespace engine {
namespace greeks {

namespace {

// Alias "amigable" <-> clave real de `GbmModel::to_params()` (PLAN_GREEKS.md §3.1: la tabla de
// strings de RiskFactor usa "model.spot"/"model.rate"/"model.dividend_yield"/"model.volatility"
// para Gbm, pero el propio modelo se construye/serializa con "s0"/"r"/"q"/"sigma" -- mismos
// nombres que ya usa `PayoffSensitivityQMeasure::greek_`, criterio de aceptación de Fase 1: "Delta/
// Vega/Rho ... coinciden ... con PayoffSensitivityQ existente"). Ningún otro modelo de esta fase
// (HullWhite1F/2F, GBM_P) necesita esta traducción: sus claves de `to_params()` YA son el nombre
// de RiskFactor que se quiere exponer (a/b/sigma/r0/eta/rho; s0/mu/sigma).
const std::map<std::string, std::string>& gbm_risk_factor_to_param_key() {
    static const std::map<std::string, std::string> aliases = {
        {"spot", "s0"}, {"rate", "r"}, {"dividend_yield", "q"}, {"volatility", "sigma"}
    };
    return aliases;
}

const std::map<std::string, std::string>& gbm_param_key_to_risk_factor() {
    static const std::map<std::string, std::string> aliases = {
        {"s0", "spot"}, {"r", "rate"}, {"q", "dividend_yield"}, {"sigma", "volatility"}
    };
    return aliases;
}

// Traduce `risk_factor.name` a la clave real que usa `model.to_params()`/el constructor del
// modelo (PLAN_GREEKS.md §4.2). Lanza std::invalid_argument (nombrando el modelo, el nombre
// pedido y las alternativas válidas) si el modelo no declara ese parámetro.
std::string resolve_model_parameter_key(const std::string& model_type, const std::string& risk_factor_name) {
    if (model_type == "GBM") {
        const auto& aliases = gbm_risk_factor_to_param_key();
        auto it = aliases.find(risk_factor_name);
        if (it == aliases.end()) {
            throw std::invalid_argument(
                "compute_greek: el modelo 'GBM' no tiene el parametro de riesgo 'model." + risk_factor_name +
                "' (validos: model.spot, model.rate, model.dividend_yield, model.volatility)"
            );
        }
        return it->second;
    }
    return risk_factor_name;
}

// Inversa de resolve_model_parameter_key, usada por compute_all_greeks para enumerar candidatos
// (PLAN_GREEKS.md §8.5, punto 1): una clave `double` de `to_params()` que no tenga alias
// declarado se expone tal cual (caso HullWhite1F/2F, GBM_P).
std::string risk_factor_name_for_param_key(const std::string& model_type, const std::string& param_key) {
    if (model_type == "GBM") {
        const auto& aliases = gbm_param_key_to_risk_factor();
        auto it = aliases.find(param_key);
        if (it != aliases.end()) return it->second;
    }
    return param_key;
}

// Política de bump por defecto (PLAN_GREEKS.md §4.3): relativo con piso absoluto para parámetros
// de modelo. `GreekRequest::bump_override` la sustituye cuando está presente.
double default_model_parameter_bump(double base_value) {
    return std::max(1e-2 * std::abs(base_value), 1e-4);
}

// Heurística de trazabilidad Q/P (PLAN_GREEKS.md §3.4/§6), documentada como limitación de Fase 1
// en el doc-comment de GreekResult::measure en greeks.hpp: se infiere del sufijo Q/P que ya sigue
// toda medida de payoff registrada en measure.hpp -- "PV"/"DV01"/"ExposureProfile"/"UnilateralCVA"
// no terminan en Q ni P y caen en DeterministicScenario.
payoff::ProbabilityMeasure infer_probability_measure(const std::string& metric_name) {
    if (!metric_name.empty() && metric_name.back() == 'Q') return payoff::ProbabilityMeasure::RiskNeutralQ;
    if (!metric_name.empty() && metric_name.back() == 'P') return payoff::ProbabilityMeasure::PhysicalP;
    return payoff::ProbabilityMeasure::DeterministicScenario;
}

} // namespace

RiskFactor parse_risk_factor(const std::string& text) {
    auto dot = text.find('.');
    if (dot == std::string::npos) {
        throw std::invalid_argument(
            "RiskFactor: '" + text + "' no tiene el formato '<scope>.<nombre>' esperado "
            "(scopes validos: model, curve, credit, time)"
        );
    }
    std::string scope = text.substr(0, dot);
    std::string rest = text.substr(dot + 1);

    if (scope == "model") {
        if (rest.empty()) {
            throw std::invalid_argument("RiskFactor: 'model.' requiere un nombre de parametro, p.ej. 'model.spot'");
        }
        return RiskFactor{RiskFactorKind::ModelParameter, "model", rest, std::nullopt};
    }
    if (scope == "curve") {
        if (rest == "parallel") {
            return RiskFactor{RiskFactorKind::CurveParallel, "curve", "", std::nullopt};
        }
        const std::string prefix = "pillar:";
        if (rest.rfind(prefix, 0) == 0) {
            const std::string idx_str = rest.substr(prefix.size());
            try {
                std::size_t consumed = 0;
                long idx = std::stol(idx_str, &consumed);
                if (idx < 0 || consumed != idx_str.size()) throw std::invalid_argument("");
                return RiskFactor{RiskFactorKind::CurvePillar, "curve", "", static_cast<std::size_t>(idx)};
            } catch (...) {
                throw std::invalid_argument(
                    "RiskFactor: 'curve.pillar:<i>' requiere un indice entero no negativo, recibido 'curve." + rest + "'"
                );
            }
        }
        throw std::invalid_argument(
            "RiskFactor: 'curve." + rest + "' desconocido (validos: 'curve.parallel', 'curve.pillar:<i>')"
        );
    }
    if (scope == "credit") {
        if (rest == "hazard_rate" || rest == "recovery_rate") {
            return RiskFactor{RiskFactorKind::CreditParameter, "credit", rest, std::nullopt};
        }
        throw std::invalid_argument(
            "RiskFactor: 'credit." + rest + "' desconocido (validos: 'credit.hazard_rate', 'credit.recovery_rate')"
        );
    }
    if (scope == "time") {
        if (rest == "theta") {
            return RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt};
        }
        throw std::invalid_argument("RiskFactor: 'time." + rest + "' desconocido (valido: 'time.theta')");
    }
    throw std::invalid_argument(
        "RiskFactor: scope '" + scope + "' desconocido (validos: 'model', 'curve', 'credit', 'time')"
    );
}

std::string to_string(const RiskFactor& risk_factor) {
    switch (risk_factor.kind) {
        case RiskFactorKind::ModelParameter:
            return "model." + risk_factor.name;
        case RiskFactorKind::CurveParallel:
            return "curve.parallel";
        case RiskFactorKind::CurvePillar:
            return "curve.pillar:" + std::to_string(risk_factor.pillar_index.value_or(0));
        case RiskFactorKind::CreditParameter:
            return "credit." + risk_factor.name;
        case RiskFactorKind::TimeShift:
            return "time.theta";
    }
    throw std::logic_error("engine::greeks::to_string(RiskFactor): RiskFactorKind desconocido"); // inalcanzable
}

GreekMethod parse_greek_method(const std::string& text) {
    if (text == "auto") return GreekMethod::Auto;
    if (text == "bump_and_reval") return GreekMethod::BumpAndReval;
    if (text == "pathwise") return GreekMethod::Pathwise;
    if (text == "aad") return GreekMethod::AadReverse;
    throw std::invalid_argument(
        "GreekMethod: '" + text + "' desconocido (validos: 'auto', 'bump_and_reval', 'pathwise', 'aad')"
    );
}

std::string to_string(GreekMethod method) {
    switch (method) {
        case GreekMethod::Auto: return "auto";
        case GreekMethod::BumpAndReval: return "bump_and_reval";
        case GreekMethod::Pathwise: return "pathwise";
        case GreekMethod::AadReverse: return "aad";
    }
    throw std::logic_error("engine::greeks::to_string(GreekMethod): GreekMethod desconocido"); // inalcanzable
}

GreekResult compute_greek(
    const Registries& registries, const GreekRequest& request, const IModel& model,
    const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
    const ExecutionContext& execution
) {
    if (request.order.order != 1 || request.order.cross_factor.has_value()) {
        throw std::invalid_argument(
            "compute_greek: Fase 1 solo soporta order=1 sin cross_factor (pedido: order=" +
            std::to_string(request.order.order) + ", cross_factor=" +
            (request.order.cross_factor.has_value() ? to_string(*request.order.cross_factor) : "ninguno") + ")"
        );
    }
    if (request.risk_factor.kind != RiskFactorKind::ModelParameter) {
        throw std::invalid_argument(
            "compute_greek: Fase 1 solo soporta RiskFactorKind::ModelParameter (pedido: '" +
            to_string(request.risk_factor) + "'); curva/credito/tiempo llegan en Fases 3-5"
        );
    }
    if (request.method == GreekMethod::Pathwise || request.method == GreekMethod::AadReverse) {
        throw std::invalid_argument(
            "compute_greek: metodo '" + to_string(request.method) +
            "' no soportado todavia (Fase 1 solo implementa BumpAndReval; pedir 'auto' o "
            "'bump_and_reval')"
        );
    }

    const std::string param_key = resolve_model_parameter_key(model.type_name(), request.risk_factor.name);
    Params base_params = model.to_params();
    auto it = base_params.find(param_key);
    if (it == base_params.end() || !std::holds_alternative<double>(it->second)) {
        throw std::invalid_argument(
            "compute_greek: el modelo '" + model.type_name() + "' no tiene el parametro de riesgo '" +
            to_string(request.risk_factor) + "'"
        );
    }
    const double base_value = std::get<double>(it->second);
    const double h = request.bump_override.value_or(default_model_parameter_bump(base_value));

    Params up_params = base_params;
    up_params[param_key] = base_value + h;
    Params down_params = base_params;
    down_params[param_key] = base_value - h;

    auto model_up = registries.models.create(model.type_name(), up_params);
    auto model_down = registries.models.create(model.type_name(), down_params);
    auto metric = registries.measures.create(request.metric_name, request.metric_params);

    MeasureResult up = metric->evaluate(*model_up, product, market, pricing, execution);
    MeasureResult down = metric->evaluate(*model_down, product, market, pricing, execution);
    if (!up.has_scalar || !down.has_scalar) {
        throw std::invalid_argument(
            "compute_greek: la medida '" + request.metric_name +
            "' no produce un resultado escalar; Fase 1 todavia no soporta Greeks de perfiles temporales"
        );
    }

    GreekResult result;
    result.value = (up.scalar - down.scalar) / (2.0 * h);
    result.order = request.order;
    result.risk_factor = request.risk_factor;
    result.method_used = GreekMethod::BumpAndReval;
    result.measure = infer_probability_measure(request.metric_name);
    result.bump_used = h;
    return result;
}

GreeksReport compute_all_greeks(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    bool include_curve_buckets, bool include_second_order
) {
    // Fase 1: solo se enumera RiskFactorKind::ModelParameter (§8.5, "version minima"). Curva
    // (Fase 3), credito (Fase 4), tiempo (Fase 5) y segundo orden (Fase 6) se ignoran por ahora,
    // sin fallar -- documentado en el doc-comment de esta funcion en greeks.hpp.
    (void)include_curve_buckets;
    (void)include_second_order;

    GreeksReport report;
    const Params model_params = model.to_params();

    // Candidatos ordenados por clave de `to_params()` para que la enumeracion sea determinista
    // (PLAN_GREEKS.md §8.5: "determinista, sin heuristicas ocultas") -- unordered_map no
    // garantiza orden de iteracion.
    std::vector<std::string> param_keys;
    param_keys.reserve(model_params.size());
    for (const auto& [key, value] : model_params) {
        if (std::holds_alternative<double>(value)) param_keys.push_back(key);
    }
    std::sort(param_keys.begin(), param_keys.end());

    for (const std::string& param_key : param_keys) {
        RiskFactor risk_factor{
            RiskFactorKind::ModelParameter, "model", risk_factor_name_for_param_key(model.type_name(), param_key),
            std::nullopt
        };
        GreekRequest request;
        request.metric_name = metric_name;
        request.metric_params = metric_params;
        request.risk_factor = risk_factor;
        request.order = GreekOrder{1, std::nullopt};
        request.method = GreekMethod::Auto;
        try {
            report.greeks.push_back(compute_greek(registries, request, model, product, market, pricing, execution));
        } catch (const std::exception& e) {
            report.skipped.push_back(to_string(risk_factor) + ": " + e.what());
        }
    }
    return report;
}

} // namespace greeks

GreekMeasure::GreekMeasure(const Params& params, const Registries& registries) : registries_(registries) {
    request_.metric_name = get_string(params, "metric");

    Params inner_metric_params;
    const std::string prefix = "metric.";
    for (const auto& [key, value] : params) {
        if (key.rfind(prefix, 0) == 0) inner_metric_params[key.substr(prefix.size())] = value;
    }
    request_.metric_params = std::move(inner_metric_params);

    request_.risk_factor = greeks::parse_risk_factor(get_string(params, "risk_factor"));
    request_.order = greeks::GreekOrder{static_cast<int>(get_double(params, "order", 1.0)), std::nullopt};
    request_.method = greeks::parse_greek_method(get_string(params, "method", "auto"));
    if (contains(params, "bump")) {
        request_.bump_override = get_double(params, "bump");
    }
}

MeasureResult GreekMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot& market, const PricingContext& pricing,
    const ExecutionContext& execution
) const {
    greeks::GreekResult out = greeks::compute_greek(registries_, request_, model, product, market, pricing, execution);
    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.value;
    return result;
}

} // namespace engine
