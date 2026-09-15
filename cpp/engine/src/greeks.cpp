#include "engine/greeks.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
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

// Política de bump por defecto para curva/crédito (PLAN_GREEKS.md §4.3): un punto básico
// absoluto, mismo default histórico de `Dv01Measure::bump_`.
double default_curve_bump() { return 0.0001; }

// Política de bump por defecto para Theta (PLAN_GREEKS.md §4.3): un día, absoluto.
double default_time_shift_bump() { return 1.0 / 365.0; }

// Metricas que ya honran `PricingContext::pricing_date()` como `valuation_time` (Fase 5): "PV"
// (IrSwapProduct via `compute_npv`, PayoffProduct via `present_value_from_market_snapshot`) y
// "PayoffPriceQ" (Monte Carlo GBM via `risk_neutral_price_gbm`). Cualquier otra metrica lo
// ignora todavia -- pedir `time.theta` sobre ella daria un Theta silenciosamente nulo (las dos
// evaluaciones +dt/base serian identicas), asi que `compute_greek` lo rechaza explicito en vez de
// aproximar (mismo principio que PLAN_GREEKS.md §4.5 aplica a pathwise sobre metricas
// indicador). Lista cerrada, ampliada a mano en cada fase que cablee una metrica nueva -- nunca
// inferida por reflexion (mismo criterio que la tabla de capacidades de §5.4).
bool metric_supports_time_shift(const std::string& metric_name) {
    return metric_name == "PV" || metric_name == "PayoffPriceQ";
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

// RiskFactorKind aceptados por el estencil generico de segundo orden/derivadas cruzadas (Fase 6):
// `ModelParameter`/`CurveParallel`/`CurvePillar`/`CreditParameter` -- exactamente los que
// `bump_state` sabe reconstruir. `TimeShift` queda fuera (PLAN_GREEKS.md §15: "AAD de segundo
// orden... Gamma/cruzadas se sirven por bump-and-reval", pero Theta de segundo orden/cruzada no
// esta implementada en esta fase -- solo Theta de primer orden puro, §7).
bool is_second_order_capable_kind(RiskFactorKind kind) {
    return kind == RiskFactorKind::ModelParameter || kind == RiskFactorKind::CurveParallel ||
           kind == RiskFactorKind::CurvePillar || kind == RiskFactorKind::CreditParameter;
}

// Tamaño de bump para `risk_factor` (PLAN_GREEKS.md §4.3): valida que el factor exista (modelo
// que no declara ese parámetro, o pillar fuera de rango) y aplica la política de defaults por
// tipo, o `override_h` si está presente. Compartido por el bump simple (orden 1) y por CADA lado
// de un estencil de orden 2/cruzado -- `override_h` solo se aplica al factor PRIMARIO
// (`GreekRequest::bump_override` no tiene un campo separado para el `cross_factor`, ver
// `compute_greek`); el `cross_factor` siempre usa su propio default.
double resolve_bump(const IModel& model, const MarketSnapshot& market, const RiskFactor& risk_factor, std::optional<double> override_h) {
    if (risk_factor.kind == RiskFactorKind::ModelParameter) {
        const std::string param_key = resolve_model_parameter_key(model.type_name(), risk_factor.name);
        Params params = model.to_params();
        auto it = params.find(param_key);
        if (it == params.end() || !std::holds_alternative<double>(it->second)) {
            throw std::invalid_argument(
                "compute_greek: el modelo '" + model.type_name() + "' no tiene el parametro de riesgo '" +
                to_string(risk_factor) + "'"
            );
        }
        return override_h.value_or(default_model_parameter_bump(std::get<double>(it->second)));
    }
    if (risk_factor.kind == RiskFactorKind::CurvePillar) {
        const std::size_t pillar_index = risk_factor.pillar_index.value_or(0);
        if (pillar_index >= market.pillars().size()) {
            throw std::invalid_argument(
                "compute_greek: 'curve.pillar:" + std::to_string(pillar_index) + "' fuera de rango (la curva "
                "tiene " + std::to_string(market.pillars().size()) + " pillars)"
            );
        }
    }
    if (risk_factor.kind == RiskFactorKind::CurveParallel || risk_factor.kind == RiskFactorKind::CurvePillar ||
        risk_factor.kind == RiskFactorKind::CreditParameter) {
        return override_h.value_or(default_curve_bump());
    }
    throw std::invalid_argument(
        "compute_greek: RiskFactorKind de '" + to_string(risk_factor) + "' no soportado para bump-and-reval"
    );
}

// Copia de (model, market) con `risk_factor` desplazado en `h` (PLAN_GREEKS.md §4.2): dueño de un
// `IModel` reconstruido si el factor es `ModelParameter` (`owned_model`/`model` apuntan a la copia
// nueva), o `market` reconstruida si es `CurveParallel`/`CurvePillar`/`CreditParameter` (`model`
// sigue apuntando al original). Componible: pasar `state.market`/`*state.model` como base de una
// SEGUNDA llamada aplica el segundo bump ENCIMA del primero -- así se arma el estencil de 4 puntos
// de una derivada cruzada (Vanna, cross-gamma) sin duplicar la lógica de reconstrucción.
struct BumpedState {
    std::unique_ptr<IModel> owned_model;
    const IModel* model;
    MarketSnapshot market;

    BumpedState(const IModel& base_model, MarketSnapshot base_market) : model(&base_model), market(std::move(base_market)) {}
};

BumpedState bump_state(
    const Registries& registries, const IModel& model, const MarketSnapshot& market, const RiskFactor& risk_factor,
    double h
) {
    BumpedState state(model, market);
    switch (risk_factor.kind) {
        case RiskFactorKind::ModelParameter: {
            const std::string param_key = resolve_model_parameter_key(model.type_name(), risk_factor.name);
            Params params = model.to_params();
            auto it = params.find(param_key);
            if (it == params.end() || !std::holds_alternative<double>(it->second)) {
                throw std::invalid_argument(
                    "compute_greek: el modelo '" + model.type_name() + "' no tiene el parametro de riesgo '" +
                    to_string(risk_factor) + "'"
                );
            }
            params[param_key] = std::get<double>(it->second) + h;
            state.owned_model = registries.models.create(model.type_name(), params);
            state.model = state.owned_model.get();
            break;
        }
        case RiskFactorKind::CurveParallel:
            state.market = bump_market_parallel(market, h);
            break;
        case RiskFactorKind::CurvePillar:
            state.market = bump_market_pillar(market, *risk_factor.pillar_index, h);
            break;
        case RiskFactorKind::CreditParameter:
            state.market = bump_market_credit(market, risk_factor.name, h);
            break;
        default:
            throw std::invalid_argument(
                "compute_greek: RiskFactorKind de '" + to_string(risk_factor) + "' no soportado para bump-and-reval"
            );
    }
    return state;
}

// Forma compartida entre dos (o más) evaluaciones bumpeadas de la MISMA métrica (PLAN_GREEKS.md
// §11 Fase 2, generalizado en Fase 6 a más de dos evaluaciones): una discrepancia indica que
// `metric_params` cambia el tamaño del perfil de forma no determinista, lo que no debería ocurrir
// nunca -- error explícito, nunca tolerado en silencio.
void require_same_shape(const MeasureResult& a, const MeasureResult& b, const std::string& metric_name) {
    if (a.has_scalar != b.has_scalar) {
        throw std::invalid_argument(
            "compute_greek: la medida '" + metric_name +
            "' devolvio formas incompatibles (has_scalar difiere) entre evaluaciones bumpeadas"
        );
    }
    if (a.times.size() != b.times.size() || a.primary.size() != b.primary.size() || a.secondary.size() != b.secondary.size()) {
        throw std::invalid_argument(
            "compute_greek: la medida '" + metric_name +
            "' devolvio perfiles de tamanos distintos entre evaluaciones bumpeadas"
        );
    }
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
    if (request.order.order < 1 || request.order.order > 2) {
        throw std::invalid_argument(
            "compute_greek: order debe ser 1 o 2 (pedido: order=" + std::to_string(request.order.order) + ")"
        );
    }
    if (request.order.order == 2 && request.order.cross_factor.has_value()) {
        throw std::invalid_argument(
            "compute_greek: order=2 con cross_factor no soportado (tercera derivada, fuera de alcance -- "
            "PLAN_GREEKS.md §3.2/§15)"
        );
    }
    const bool is_time_factor = request.risk_factor.kind == RiskFactorKind::TimeShift;
    if (is_time_factor) {
        if (request.order.order != 1 || request.order.cross_factor.has_value()) {
            throw std::invalid_argument(
                "compute_greek: 'time.theta' solo soporta order=1 sin cross_factor (Theta de segundo orden/"
                "cruzada fuera de alcance -- PLAN_GREEKS.md §15)"
            );
        }
        if (!metric_supports_time_shift(request.metric_name)) {
            throw std::invalid_argument(
                "compute_greek: 'time.theta' todavia no esta cableado para la metrica '" + request.metric_name +
                "' (solo honra PricingContext::pricing_date() en 'PV' y 'PayoffPriceQ'; pedirlo sobre otra "
                "metrica daria un Theta silenciosamente nulo, asi que se rechaza explicito en vez de aproximar)"
            );
        }
    } else {
        if (!is_second_order_capable_kind(request.risk_factor.kind)) {
            throw std::invalid_argument(
                "compute_greek: RiskFactorKind de '" + to_string(request.risk_factor) +
                "' no soportado todavia (soporta model.*/curve.parallel/curve.pillar:<i>/"
                "credit.hazard_rate/credit.recovery_rate, y time.theta solo con order=1 sin cross_factor)"
            );
        }
        if (request.order.cross_factor.has_value() && !is_second_order_capable_kind(request.order.cross_factor->kind)) {
            throw std::invalid_argument(
                "compute_greek: cross_factor '" + to_string(*request.order.cross_factor) +
                "' tiene un RiskFactorKind no soportado para derivadas cruzadas (time.theta excluido, "
                "PLAN_GREEKS.md §15)"
            );
        }
    }
    if (request.method == GreekMethod::Pathwise || request.method == GreekMethod::AadReverse) {
        throw std::invalid_argument(
            "compute_greek: metodo '" + to_string(request.method) +
            "' no soportado todavia (Fase 1 solo implementa BumpAndReval; pedir 'auto' o "
            "'bump_and_reval')"
        );
    }

    auto metric = registries.measures.create(request.metric_name, request.metric_params);

    if (is_time_factor) {
        // Theta puro (PLAN_GREEKS.md §7.1): diferencia UNIDIRECCIONAL Metric(t+dt) - Metric(t),
        // nunca centrada (el tiempo no "retrocede") -- misma convencion que `Dv01Measure`
        // (`bumped - base`, sin dividir por `h`, ver measure.cpp). `market`/`model` quedan
        // intactos; solo se desplaza `PricingContext::pricing_date()`, la unica via por la que
        // las metricas cableadas (`metric_supports_time_shift`) leen `valuation_time`.
        const double h = request.bump_override.value_or(default_time_shift_bump());
        PricingContext pricing_shifted(Params{
            {"pricing_date", pricing.pricing_date() + h},
            {"n_paths", static_cast<double>(pricing.n_paths())},
            {"n_steps", static_cast<double>(pricing.n_steps())},
            {"seed", static_cast<double>(pricing.seed())},
        });
        MeasureResult up = metric->evaluate(model, product, market, pricing_shifted, execution);
        MeasureResult down = metric->evaluate(model, product, market, pricing, execution);
        require_same_shape(up, down, request.metric_name);
        if (!up.has_scalar && up.times.empty() && up.primary.empty() && up.secondary.empty()) {
            throw std::invalid_argument(
                "compute_greek: la medida '" + request.metric_name + "' no produce ningun resultado (ni escalar ni perfil)"
            );
        }

        GreekResult result;
        result.has_scalar = up.has_scalar;
        if (up.has_scalar) result.value = up.scalar - down.scalar; // §7.1: sin normalizar por h
        result.times = up.times;
        result.primary.reserve(up.primary.size());
        for (std::size_t i = 0; i < up.primary.size(); ++i) result.primary.push_back(up.primary[i] - down.primary[i]);
        result.secondary.reserve(up.secondary.size());
        for (std::size_t i = 0; i < up.secondary.size(); ++i) result.secondary.push_back(up.secondary[i] - down.secondary[i]);
        result.order = request.order;
        result.risk_factor = request.risk_factor;
        result.method_used = GreekMethod::BumpAndReval;
        result.measure = infer_probability_measure(request.metric_name);
        result.bump_used = h;
        return result;
    }

    const double h1 = resolve_bump(model, market, request.risk_factor, request.bump_override);

    if (request.order.cross_factor.has_value()) {
        // Derivada cruzada de primer orden (Vanna, cross-gamma -- PLAN_GREEKS.md §3.2/§11 Fase 6):
        // estencil de 4 puntos, cada uno compone DOS bumps (el segundo aplicado ENCIMA del
        // primero via `bump_state`, ver su doc-comment) -- `(up_up - up_down - down_up +
        // down_down) / (4*h1*h2)`. El `cross_factor` siempre usa su propio bump por defecto (sin
        // `bump_override` dedicado, ver `resolve_bump`); se documenta en `warnings` para que el
        // resultado siga siendo auto-explicativo (§13) aunque `GreekResult` solo tenga un campo
        // `bump_used`.
        const RiskFactor& cross = *request.order.cross_factor;
        const double h2 = resolve_bump(model, market, cross, std::nullopt);

        BumpedState s_up = bump_state(registries, model, market, request.risk_factor, h1);
        BumpedState s_down = bump_state(registries, model, market, request.risk_factor, -h1);
        BumpedState s_up_up = bump_state(registries, *s_up.model, s_up.market, cross, h2);
        BumpedState s_up_down = bump_state(registries, *s_up.model, s_up.market, cross, -h2);
        BumpedState s_down_up = bump_state(registries, *s_down.model, s_down.market, cross, h2);
        BumpedState s_down_down = bump_state(registries, *s_down.model, s_down.market, cross, -h2);

        MeasureResult up_up = metric->evaluate(*s_up_up.model, product, s_up_up.market, pricing, execution);
        MeasureResult up_down = metric->evaluate(*s_up_down.model, product, s_up_down.market, pricing, execution);
        MeasureResult down_up = metric->evaluate(*s_down_up.model, product, s_down_up.market, pricing, execution);
        MeasureResult down_down = metric->evaluate(*s_down_down.model, product, s_down_down.market, pricing, execution);
        require_same_shape(up_up, up_down, request.metric_name);
        require_same_shape(up_up, down_up, request.metric_name);
        require_same_shape(up_up, down_down, request.metric_name);
        if (!up_up.has_scalar && up_up.times.empty() && up_up.primary.empty() && up_up.secondary.empty()) {
            throw std::invalid_argument(
                "compute_greek: la medida '" + request.metric_name + "' no produce ningun resultado (ni escalar ni perfil)"
            );
        }

        const double denominator = 4.0 * h1 * h2;
        GreekResult result;
        result.has_scalar = up_up.has_scalar;
        if (up_up.has_scalar) {
            result.value = (up_up.scalar - up_down.scalar - down_up.scalar + down_down.scalar) / denominator;
        }
        result.times = up_up.times;
        result.primary.reserve(up_up.primary.size());
        for (std::size_t i = 0; i < up_up.primary.size(); ++i) {
            result.primary.push_back(
                (up_up.primary[i] - up_down.primary[i] - down_up.primary[i] + down_down.primary[i]) / denominator
            );
        }
        result.secondary.reserve(up_up.secondary.size());
        for (std::size_t i = 0; i < up_up.secondary.size(); ++i) {
            result.secondary.push_back(
                (up_up.secondary[i] - up_down.secondary[i] - down_up.secondary[i] + down_down.secondary[i]) / denominator
            );
        }
        result.order = request.order;
        result.risk_factor = request.risk_factor;
        result.method_used = GreekMethod::BumpAndReval;
        result.measure = infer_probability_measure(request.metric_name);
        result.bump_used = h1;
        result.warnings.push_back("cross_factor '" + to_string(cross) + "' bumpeado en +-" + std::to_string(h2));
        return result;
    }

    BumpedState s_up = bump_state(registries, model, market, request.risk_factor, h1);
    BumpedState s_down = bump_state(registries, model, market, request.risk_factor, -h1);
    MeasureResult up = metric->evaluate(*s_up.model, product, s_up.market, pricing, execution);
    MeasureResult down = metric->evaluate(*s_down.model, product, s_down.market, pricing, execution);
    require_same_shape(up, down, request.metric_name);

    MeasureResult base;
    if (request.order.order == 2) {
        // Gamma/Volga (estencil de 3 puntos, PLAN_GREEKS.md §11 Fase 6): reutiliza `up`/`down` ya
        // computados para orden 1 y añade una única evaluación extra en el punto base -- "da Gamma
        // 'gratis' con una unica evaluacion extra" (§4.1).
        base = metric->evaluate(model, product, market, pricing, execution);
        require_same_shape(up, base, request.metric_name);
    }
    if (!up.has_scalar && up.times.empty() && up.primary.empty() && up.secondary.empty()) {
        throw std::invalid_argument(
            "compute_greek: la medida '" + request.metric_name + "' no produce ningun resultado (ni escalar ni perfil)"
        );
    }

    const bool is_gamma = request.order.order == 2;
    const double denominator = is_gamma ? (h1 * h1) : (2.0 * h1);

    GreekResult result;
    result.has_scalar = up.has_scalar;
    if (up.has_scalar) {
        result.value = is_gamma ? (up.scalar - 2.0 * base.scalar + down.scalar) / denominator
                                 : (up.scalar - down.scalar) / denominator;
    }
    result.times = up.times; // mismo eje temporal que la metrica base (no depende del parametro bumpeado)
    result.primary.reserve(up.primary.size());
    for (std::size_t i = 0; i < up.primary.size(); ++i) {
        result.primary.push_back(
            is_gamma ? (up.primary[i] - 2.0 * base.primary[i] + down.primary[i]) / denominator
                     : (up.primary[i] - down.primary[i]) / denominator
        );
    }
    result.secondary.reserve(up.secondary.size());
    for (std::size_t i = 0; i < up.secondary.size(); ++i) {
        result.secondary.push_back(
            is_gamma ? (up.secondary[i] - 2.0 * base.secondary[i] + down.secondary[i]) / denominator
                     : (up.secondary[i] - down.secondary[i]) / denominator
        );
    }
    result.order = request.order;
    result.risk_factor = request.risk_factor;
    result.method_used = GreekMethod::BumpAndReval;
    result.measure = infer_probability_measure(request.metric_name);
    result.bump_used = h1;
    return result;
}

GreeksReport compute_all_greeks(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    bool include_curve_buckets, bool include_second_order
) {
    GreeksReport report;
    const Params model_params = model.to_params();

    auto try_request = [&](GreekRequest request) {
        const RiskFactor risk_factor = request.risk_factor; // copia: request se mueve a compute_greek
        try {
            report.greeks.push_back(compute_greek(registries, request, model, product, market, pricing, execution));
        } catch (const std::exception& e) {
            report.skipped.push_back(to_string(risk_factor) + " (order=" + std::to_string(request.order.order) + "): " + e.what());
        }
    };

    auto try_candidate = [&](const RiskFactor& risk_factor) {
        GreekRequest request;
        request.metric_name = metric_name;
        request.metric_params = metric_params;
        request.risk_factor = risk_factor;
        request.order = GreekOrder{1, std::nullopt};
        request.method = GreekMethod::Auto;
        try_request(request);
    };

    // Politica de enumeracion de §8.5, punto 5 (Fase 6): si `include_second_order`, la Gamma pura
    // (order=2, sin cross_factor) de cada factor de MODELO (punto 1) -- NUNCA de curva/credito
    // (fuera del alcance exacto que fija §8.5) ni derivadas cruzadas (Vanna/cross-gamma nunca se
    // enumeran automaticamente, se piden una a una via compute_greek/GreekOrder::cross_factor).
    auto try_gamma_candidate = [&](const RiskFactor& risk_factor) {
        if (!include_second_order) return;
        GreekRequest request;
        request.metric_name = metric_name;
        request.metric_params = metric_params;
        request.risk_factor = risk_factor;
        request.order = GreekOrder{2, std::nullopt};
        request.method = GreekMethod::Auto;
        try_request(request);
    };

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
        try_candidate(risk_factor);
        try_gamma_candidate(risk_factor);
    }

    // Politica de enumeracion de §8.5, punto 2 (Fase 3): "curve.parallel" siempre; "curve.pillar:i"
    // por cada pillar de `market` solo si `include_curve_buckets`.
    try_candidate(RiskFactor{RiskFactorKind::CurveParallel, "curve", "", std::nullopt});
    if (include_curve_buckets) {
        for (std::size_t i = 0; i < market.pillars().size(); ++i) {
            try_candidate(RiskFactor{RiskFactorKind::CurvePillar, "curve", "", i});
        }
    }

    // Politica de enumeracion de §8.5, punto 3 (Fase 4): "credit.hazard_rate"/
    // "credit.recovery_rate" siempre -- una derivada nula (metrica que no consume credito) es una
    // respuesta valida, distinta de "no aplica", y se reporta como tal en vez de omitirse.
    try_candidate(RiskFactor{RiskFactorKind::CreditParameter, "credit", "hazard_rate", std::nullopt});
    try_candidate(RiskFactor{RiskFactorKind::CreditParameter, "credit", "recovery_rate", std::nullopt});

    // Politica de enumeracion de §8.5, punto 4 (Fase 5): "time.theta" siempre -- si la metrica no
    // esta cableada todavia (`metric_supports_time_shift`) o `dt` cruza un instante sin historico,
    // `compute_greek` lanza y el candidato cae en `skipped`, nunca se omite en silencio.
    try_candidate(RiskFactor{RiskFactorKind::TimeShift, "time", "", std::nullopt});

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
    result.has_scalar = out.has_scalar;
    result.scalar = out.value;
    result.times = out.times;
    result.primary = out.primary;
    result.secondary = out.secondary;
    return result;
}

} // namespace engine
