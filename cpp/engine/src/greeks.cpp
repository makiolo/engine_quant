#include "engine/greeks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <variant>
#include <vector>

#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"

namespace engine {
namespace greeks {

namespace {

// Alias "amigable" <-> clave real de `GbmModel::to_params()` (PLAN_GREEKS.md §3.1: la tabla de
// strings de RiskFactor usa "model.spot"/"model.rate"/"model.dividend_yield"/"model.volatility"
// para Gbm, pero el propio modelo se construye/serializa con "s0"/"r"/"q"/"sigma" -- mismos
// nombres que ya usa `PayoffSensitivityQMeasure::greek_`, criterio de aceptación de Fase 1: "Delta/
// Vega/Rho ... coinciden ... con PayoffSensitivityQ existente"). HullWhite1F/2F no necesita esta
// traducción: sus claves de `to_params()` YA son el nombre de RiskFactor que se quiere exponer
// (a/b/sigma/r0/eta/rho).
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

// Equivalente de `gbm_risk_factor_to_param_key`/`gbm_param_key_to_risk_factor` para `GbmPModel`
// (PLAN_GREEKS.md §11 Fase 7): mismo nombre "amigable" `volatility`/`spot` que Gbm/Q para los
// parametros que comparten significado economico -- "mu" (drift fisico) no tiene equivalente en
// Gbm/Q, se expone tal cual (coincide con su propia clave de `to_params()`, no necesita alias).
// Mismas cadenas que acepta `engine_core::payoff::sensitivity::GbmPGreek::parse` del lado Rust --
// deben coincidir para que `method=auto` pueda alternar entre bump-and-reval y pathwise sobre el
// MISMO `RiskFactor` sin que el nombre cambie de significado segun el metodo.
const std::map<std::string, std::string>& gbm_p_risk_factor_to_param_key() {
    static const std::map<std::string, std::string> aliases = {{"spot", "s0"}, {"volatility", "sigma"}};
    return aliases;
}

const std::map<std::string, std::string>& gbm_p_param_key_to_risk_factor() {
    static const std::map<std::string, std::string> aliases = {{"s0", "spot"}, {"sigma", "volatility"}};
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
    if (model_type == "GBM_P") {
        const auto& aliases = gbm_p_risk_factor_to_param_key();
        auto it = aliases.find(risk_factor_name);
        if (it != aliases.end()) return it->second;
        // "mu" (y cualquier otra clave de to_params() sin alias) se expone tal cual.
        return risk_factor_name;
    }
    return risk_factor_name;
}

// Inversa de resolve_model_parameter_key, usada por compute_all_greeks para enumerar candidatos
// (PLAN_GREEKS.md §8.5, punto 1): una clave `double` de `to_params()` que no tenga alias
// declarado se expone tal cual (caso HullWhite1F/2F, y "mu" de GBM_P).
std::string risk_factor_name_for_param_key(const std::string& model_type, const std::string& param_key) {
    if (model_type == "GBM") {
        const auto& aliases = gbm_param_key_to_risk_factor();
        auto it = aliases.find(param_key);
        if (it != aliases.end()) return it->second;
    }
    if (model_type == "GBM_P") {
        const auto& aliases = gbm_p_param_key_to_risk_factor();
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

// --- Tabla de capacidades (PLAN_GREEKS.md §5.4, Fase 7) --------------------------------------
// Poblada explícitamente, nunca inferida por reflexión (mismo criterio que
// `payoff::ModelCapabilities` en PLAN_PRODUCTS.md §6): cada entrada requiere un test diferencial
// contra bump-and-reval en verde ANTES de añadirse aquí (§5.3), ver test_greeks.cpp
// (`GreeksFase7Test`). Solo cubre `RiskFactorKind::ModelParameter`, `order=1`, sin
// `cross_factor` -- Gamma/derivadas cruzadas siguen sirviéndose por bump-and-reval incluso para
// un (modelo, métrica) que aquí aparezca (§5.2: "AAD de segundo orden... fuera de alcance").
struct SpecializedCapability {
    std::string model_type;
    std::string metric_name;
};

bool capability_listed(
    const std::vector<SpecializedCapability>& table, const std::string& model_type, const std::string& metric_name
) {
    for (const auto& c : table) {
        if (c.model_type == model_type && c.metric_name == metric_name) return true;
    }
    return false;
}

// Pathwise (`Dual`, forward-mode): generaliza `payoff::sensitivity` (PLAN_GREEKS.md §5.1) a
// `PayoffPriceQ` sobre `GbmModel` y, Fase 7, a `PayoffForecastP` sobre `GbmPModel`. Verificado
// contra bump-and-reval en `GreeksFase7Test.PathwiseDeltaVegaRhoDividendYieldOfACallMatch...`/
// `...UnderPMatch...` (test_greeks.cpp).
const std::vector<SpecializedCapability>& pathwise_capabilities() {
    static const std::vector<SpecializedCapability> table = {
        {"GBM", "PayoffPriceQ"},
        {"GBM_P", "PayoffForecastP"},
    };
    return table;
}

// Gamma (segunda derivada PURA respecto de "spot") via likelihood ratio (PLAN_HYPERDUAL.md §5,
// revisado): la generalizacion original con `Dual2` resulto matematicamente incorrecta para
// payoffs con kink -- ver el doc-comment de `engine_core::payoff::lrm`. Mismas dos entradas que
// `pathwise_capabilities()`, pero ademas requiere dinamicamente `payoff_supports_second_order_lrm`
// (una unica fecha terminal, ver `try_pathwise2`) y `risk_factor.name == "spot"`. Verificado contra
// Black-Scholes cerrado en `GreeksHyperdualTest.AutoResolvesGammaOfACallToLikelihoodRatioPathwise`
// (test_greeks.cpp).
const std::vector<SpecializedCapability>& pathwise2_capabilities() {
    static const std::vector<SpecializedCapability> table = {
        {"GBM", "PayoffPriceQ"},
        {"GBM_P", "PayoffForecastP"},
    };
    return table;
}

// Vanna (derivada cruzada) via likelihood ratio -- mismo criterio que `pathwise2_capabilities()`,
// solo el par ("spot","volatility"). Verificado en
// `GreeksHyperdualTest.AutoResolvesVannaOfACallToLikelihoodRatioPathwise`.
const std::vector<SpecializedCapability>& pathwise_cross_capabilities() {
    static const std::vector<SpecializedCapability> table = {
        {"GBM", "PayoffPriceQ"},
        {"GBM_P", "PayoffForecastP"},
    };
    return table;
}

// Hessiano local del motor de payoff via likelihood ratio (PLAN_BACKWARD.md §9 Fase 1): mismas dos
// entradas que `pathwise2_capabilities()`/`pathwise_cross_capabilities()` (misma restriccion
// dinamica de una unica fecha terminal, comprobada en `try_hessian_likelihood_ratio`). Fase 2/3
// anaden aqui HullWhite1F/2F via AadForwardOverForward (PLAN_BACKWARD.md §7.1) -- HullWhite2F
// excluye cualquier par que incluya "rho" (no es diferenciable, mismo criterio que
// `aad_capabilities()`/`try_aad_reverse`): `try_hessian_forward_over_forward` simplemente nunca
// emite una `HessianEntry` con ese factor, asi que el filtrado generico de `compute_hessian`
// (`distinct_factors_in`/`is_valid`) lo excluye solo, sin necesitar logica especial aqui.
const std::vector<SpecializedCapability>& hessian_capabilities() {
    static const std::vector<SpecializedCapability> table = {
        {"GBM", "PayoffPriceQ"},
        {"GBM_P", "PayoffForecastP"},
        {"HullWhite1F", "HullWhiteModelNpv"},
        {"HullWhite2F", "HullWhiteModelNpv"},
    };
    return table;
}

// Intenta el Hessiano local completo {gamma_ss, volga_vv, vanna_sv} via likelihood ratio, UNA
// UNICA simulacion GBM reutilizada para las tres entradas (PLAN_BACKWARD.md §9 Fase 1,
// `payoff::payoff_local_hessian_gbm[_p]`). Mismo criterio que `try_pathwise2`/`try_pathwise_cross`:
// `std::nullopt` si la combinacion (modelo, metrica) no esta en `hessian_capabilities()`, si el
// producto no es un `PayoffProduct`, o si el contrato no depende de una unica fecha terminal
// (`payoff_supports_second_order_lrm[_p]`) -- nunca lanza, solo informa "no aplica". `metric_name`
// se pasa aparte (en vez de un `GreekRequest` completo, que no tiene sentido aqui: no hay un unico
// `risk_factor`/`order` que pedir, las tres entradas salen siempre juntas de la misma simulacion).
// Orden fijo del array devuelto: [0]=gamma (spot,spot), [1]=volga (volatility,volatility),
// [2]=vanna (spot,volatility).
std::optional<std::array<HessianEntry, 3>> try_hessian_likelihood_ratio(
    const std::string& metric_name, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    const std::string model_type = model.type_name();
    if (!capability_listed(hessian_capabilities(), model_type, metric_name)) return std::nullopt;

    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) return std::nullopt;

    // PLAN_IMPROVE_NOTEBOOK2.md Fase 0 (ADR-IN2-01, mismo criterio que `try_pathwise` mas abajo):
    // `payoff_local_hessian_gbm[_p]` (Rust) nunca recibe `pricing.pricing_date()`, solo n_paths/seed
    // -- bajo `pricing_date() != 0` el Hessiano LRM seria SILENCIOSAMENTE identico al de
    // pricing_date=0. Se rechaza aqui explicito: `compute_hessian` cae a `skipped` con el motivo
    // exacto (no hay fallback bump-and-reval generico de segundo orden todavia, ver Fase 2 de
    // PLAN_IMPROVE_NOTEBOOK2.md) en vez de devolver un numero incorrecto.
    if (pricing.pricing_date() != 0.0) return std::nullopt;

    payoff::LocalHessianResult hessian;
    if (model_type == "GBM") {
        if (!payoff::payoff_supports_second_order_lrm(*payoff_product->payoff_program())) return std::nullopt;
        hessian = payoff::payoff_local_hessian_gbm(
            *payoff_product->payoff_program(), dynamic_cast<const GbmModel&>(model), pricing.n_paths(), pricing.seed()
        );
    } else if (model_type == "GBM_P") {
        if (!payoff::payoff_supports_second_order_lrm_p(*payoff_product->payoff_program())) return std::nullopt;
        hessian = payoff::payoff_local_hessian_gbm_p(
            *payoff_product->payoff_program(), dynamic_cast<const GbmPModel&>(model), pricing.n_paths(), pricing.seed()
        );
    } else {
        return std::nullopt; // inalcanzable dada hessian_capabilities(), defensivo
    }

    const RiskFactor spot{RiskFactorKind::ModelParameter, "model", "spot", std::nullopt};
    const RiskFactor volatility{RiskFactorKind::ModelParameter, "model", "volatility", std::nullopt};

    std::array<HessianEntry, 3> entries;
    entries[0] = HessianEntry{
        spot, spot, hessian.gamma.value, std::optional<double>(hessian.gamma.std_error),
        GreekMethod::LikelihoodRatioHessian, hessian.gamma.measure
    };
    entries[1] = HessianEntry{
        volatility, volatility, hessian.volga.value, std::optional<double>(hessian.volga.std_error),
        GreekMethod::LikelihoodRatioHessian, hessian.volga.measure
    };
    entries[2] = HessianEntry{
        spot, volatility, hessian.vanna.value, std::optional<double>(hessian.vanna.std_error),
        GreekMethod::LikelihoodRatioHessian, hessian.vanna.measure
    };
    return entries;
}

// Hessiano cerrado de Hull-White 1F/2F via Dual2/HyperDual NUEVOS (forward-over-forward,
// PLAN_BACKWARD.md §5/§9 Fase 2/3): UNA UNICA llamada a `hull_white_1f_hessian`/
// `hull_white_2f_hessian` (puente cxx -> `engine_core::models::hull_white_dual`) da el valor, el
// gradiente y las entradas del Hessiano de una vez -- no Monte Carlo, no bump-and-reval, no AAD
// reverse-mode de Burn (que no anida Autodiff para dar un Hessiano, PLAN_BACKWARD.md §1.2). Mismo
// criterio "mejor esfuerzo" que `try_hessian_likelihood_ratio`: `std::nullopt` si la combinacion
// (modelo, metrica) no esta en `hessian_capabilities()`, o si el modelo/producto no son los
// esperados -- nunca lanza. HullWhite1F: orden fijo de las 10 entradas devueltas, 4 diagonales
// (a,a)/(b,b)/(sigma,sigma)/(r0,r0) seguidas de las 6 cruzadas (a,b)/(a,sigma)/(a,r0)/(b,sigma)/
// (b,r0)/(sigma,r0). HullWhite2F: orden fijo de las 15 entradas, 5 diagonales
// (a,a)/(b,b)/(sigma,sigma)/(eta,eta)/(r0,r0) seguidas de las 10 cruzadas (a,b)/(a,sigma)/
// (a,eta)/(a,r0)/(b,sigma)/(b,eta)/(b,r0)/(sigma,eta)/(sigma,r0)/(eta,r0) -- NUNCA se genera una
// entrada para "rho" (no es diferenciable, mismo criterio que `try_aad_reverse` con HullWhite2F).
// Ninguna entrada lleva std_error (formula cerrada, no Monte Carlo) y measure=
// DeterministicScenario (mismo criterio que `try_aad_reverse` para la metrica
// "HullWhiteModelNpv", ver mas abajo).
std::optional<std::vector<HessianEntry>> try_hessian_forward_over_forward(
    const std::string& metric_name, const IModel& model, const IProduct& product
) {
    const std::string model_type = model.type_name();
    if (!capability_listed(hessian_capabilities(), model_type, metric_name)) return std::nullopt;

    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) return std::nullopt; // defensivo: la tabla solo lista IrSwapProduct

    auto push = [](std::vector<HessianEntry>& entries, const RiskFactor& factor_i, const RiskFactor& factor_j, double value) {
        entries.push_back(HessianEntry{
            factor_i, factor_j, value, std::nullopt, GreekMethod::AadForwardOverForward,
            payoff::ProbabilityMeasure::DeterministicScenario
        });
    };

    if (model_type == "HullWhite1F") {
        const auto* hw1f = dynamic_cast<const HullWhite1FModel*>(&model);
        if (!hw1f) return std::nullopt; // defensivo: la tabla solo lista HullWhite1FModel

        HullWhite1FHessian h = hull_white_1f_hessian(
            hw1f->a(), hw1f->b(), hw1f->sigma(), hw1f->r0(), irs_product->notional(), irs_product->fixed_rate(),
            irs_product->use_par_rate(), irs_product->start(), irs_product->payment_times(), irs_product->accruals()
        );

        const RiskFactor fa{RiskFactorKind::ModelParameter, "model", "a", std::nullopt};
        const RiskFactor fb{RiskFactorKind::ModelParameter, "model", "b", std::nullopt};
        const RiskFactor fsigma{RiskFactorKind::ModelParameter, "model", "sigma", std::nullopt};
        const RiskFactor fr0{RiskFactorKind::ModelParameter, "model", "r0", std::nullopt};

        std::vector<HessianEntry> entries;
        entries.reserve(10);
        push(entries, fa, fa, h.d_aa);
        push(entries, fb, fb, h.d_bb);
        push(entries, fsigma, fsigma, h.d_sigmasigma);
        push(entries, fr0, fr0, h.d_r0r0);
        push(entries, fa, fb, h.d_ab);
        push(entries, fa, fsigma, h.d_asigma);
        push(entries, fa, fr0, h.d_ar0);
        push(entries, fb, fsigma, h.d_bsigma);
        push(entries, fb, fr0, h.d_br0);
        push(entries, fsigma, fr0, h.d_sigmar0);
        return entries;
    } else if (model_type == "HullWhite2F") {
        const auto* hw2f = dynamic_cast<const HullWhite2FModel*>(&model);
        if (!hw2f) return std::nullopt; // defensivo: la tabla solo lista HullWhite2FModel

        HullWhite2FHessian h = hull_white_2f_hessian(
            hw2f->a(), hw2f->b(), hw2f->sigma(), hw2f->eta(), hw2f->rho(), hw2f->r0(), irs_product->notional(),
            irs_product->fixed_rate(), irs_product->use_par_rate(), irs_product->start(),
            irs_product->payment_times(), irs_product->accruals()
        );

        const RiskFactor fa{RiskFactorKind::ModelParameter, "model", "a", std::nullopt};
        const RiskFactor fb{RiskFactorKind::ModelParameter, "model", "b", std::nullopt};
        const RiskFactor fsigma{RiskFactorKind::ModelParameter, "model", "sigma", std::nullopt};
        const RiskFactor feta{RiskFactorKind::ModelParameter, "model", "eta", std::nullopt};
        const RiskFactor fr0{RiskFactorKind::ModelParameter, "model", "r0", std::nullopt};

        std::vector<HessianEntry> entries;
        entries.reserve(15);
        push(entries, fa, fa, h.d_aa);
        push(entries, fb, fb, h.d_bb);
        push(entries, fsigma, fsigma, h.d_sigmasigma);
        push(entries, feta, feta, h.d_etaeta);
        push(entries, fr0, fr0, h.d_r0r0);
        push(entries, fa, fb, h.d_ab);
        push(entries, fa, fsigma, h.d_asigma);
        push(entries, fa, feta, h.d_aeta);
        push(entries, fa, fr0, h.d_ar0);
        push(entries, fb, fsigma, h.d_bsigma);
        push(entries, fb, feta, h.d_beta);
        push(entries, fb, fr0, h.d_br0);
        push(entries, fsigma, feta, h.d_sigmaeta);
        push(entries, fsigma, fr0, h.d_sigmar0);
        push(entries, feta, fr0, h.d_etar0);
        return entries;
    }

    return std::nullopt; // inalcanzable dada hessian_capabilities(), defensivo
}

// AAD reverse-mode (`Autodiff<CpuBackend>` de Burn): generaliza `irs_hull_white_npv_delta_r0` a
// las cuatro/cinco Greeks de una sola pasada `backward()` (PLAN_GREEKS.md §5.2), sobre la métrica
// nueva `"HullWhiteModelNpv"` (measure.hpp) -- NO sobre `"PV"`, que desde PLAN_REAPI.md §6 Fase 4
// descuenta por la curva observada y ya no depende del modelo (ver el doc-comment de
// `HullWhiteModelNpvMeasure`). Verificado contra bump-and-reval en
// `GreeksFase7Test.AadMatchesBumpAndRevalForHullWhite1F/2F` (test_greeks.cpp). `rho` de
// HullWhite2F queda fuera (no es un tensor diferenciable en el modelo actual, ver
// `engine::HullWhite2FGreeks`) -- sigue sirviéndose por bump-and-reval, nunca por AAD.
const std::vector<SpecializedCapability>& aad_capabilities() {
    static const std::vector<SpecializedCapability> table = {
        {"HullWhite1F", "HullWhiteModelNpv"},
        {"HullWhite2F", "HullWhiteModelNpv"},
    };
    return table;
}

// Métricas de tipo indicador/probabilidad (PLAN_GREEKS.md §4.5): la derivada pathwise EXACTA de
// una función indicador es 0 en casi todo punto y no informa nada -- NUNCA declaran soporte
// pathwise, ni aquí ni en ninguna fase futura sin cambiar de método (verosimilitud/Malliavin,
// fuera de alcance, §15). Solo informativo/documental por ahora: `pathwise_capabilities()` ya no
// lista ninguna de estas métricas, así que `try_pathwise` nunca las alcanza -- este conjunto
// existe para que un mensaje de error explícito (`method=pathwise` pedido a mano) pueda nombrar
// la razón exacta en vez de un genérico "no soportado".
const std::set<std::string>& indicator_type_metrics() {
    static const std::set<std::string> metrics = {"PayoffHitProbabilityQ", "PayoffHitProbabilityP"};
    return metrics;
}

// Intenta la ruta pathwise para `request` (PLAN_GREEKS.md §5.1). Devuelve `std::nullopt` si la
// combinación (modelo, métrica) no está en la tabla de capacidades O si el contrato de payoff
// contiene `ContractOp::Exercise` (chequeo DINÁMICO, no estático -- un mismo `model_type`/
// `metric_name` puede o no soportar pathwise según el contrato concreto, §5.1: "un contrato con
// Exercise sigue cayendo al fallback bump-and-reval"). El llamante decide qué significa ese
// `nullopt`: bajo `method=Auto`, caer a `BumpAndReval` en silencio (comportamiento correcto,
// documentado); bajo `method=Pathwise` explícito, es un error -- por eso `try_pathwise` no lanza
// nunca, solo informa "no aplica" y dejar que compute_greek decida el mensaje según el método
// pedido.
std::optional<GreekResult> try_pathwise(
    const GreekRequest& request, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    const std::string model_type = model.type_name();
    if (!capability_listed(pathwise_capabilities(), model_type, request.metric_name)) return std::nullopt;

    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) return std::nullopt; // defensivo: la tabla solo lista modelos de PayoffProduct

    if (payoff::payoff_contains_exercise(*payoff_product->payoff_program())) return std::nullopt;

    // PLAN_IMPROVE_NOTEBOOK2.md Fase 0 (ADR-IN2-01): `payoff_sensitivity_gbm[_p]` (Rust) nunca
    // recibe `pricing.pricing_date()`, a diferencia de `risk_neutral_price_gbm` (que SI la honra
    // desde PLAN_GREEKS.md §7.2/Fase 5, ver `PayoffPriceQMeasure::evaluate` en measure.cpp) -- bajo
    // `pricing_date() != 0` esta ruta pathwise devolveria SILENCIOSAMENTE el mismo valor que a
    // pricing_date=0 (el bug real que motiva esta fase: se descubrio porque charm, una diferencia
    // finita de delta entre dos pricing_date, salia exactamente 0.0). Se rechaza aqui explicito:
    // bajo method=Auto, `compute_greek` cae al estencil de bump-and-reval generico (que SI
    // reconstruye el `MeasureResult` con `pricing_date` desplazado, porque llama directamente a
    // `metric->evaluate(..., pricing, ...)` y `PayoffPriceQMeasure::evaluate` ya reenvia
    // `pricing.pricing_date()`); bajo method=Pathwise EXPLICITO, `compute_greek` lanza un error
    // claro via `pathwise_unsupported_reason` en vez de aproximar en silencio.
    if (pricing.pricing_date() != 0.0) return std::nullopt;

    payoff::SensitivityResult sensitivity;
    if (model_type == "GBM") {
        sensitivity = payoff::payoff_sensitivity_gbm(
            *payoff_product->payoff_program(), dynamic_cast<const GbmModel&>(model), request.risk_factor.name,
            pricing.n_paths(), pricing.seed()
        );
    } else if (model_type == "GBM_P") {
        sensitivity = payoff::payoff_sensitivity_gbm_p(
            *payoff_product->payoff_program(), dynamic_cast<const GbmPModel&>(model), request.risk_factor.name,
            pricing.n_paths(), pricing.seed()
        );
    } else {
        return std::nullopt; // inalcanzable dada pathwise_capabilities(), defensivo
    }

    GreekResult result;
    result.has_scalar = true;
    result.value = sensitivity.value;
    result.std_error = sensitivity.std_error;
    result.order = request.order;
    result.risk_factor = request.risk_factor;
    result.method_used = GreekMethod::Pathwise;
    result.measure = sensitivity.measure;
    return result;
}

// Intenta Gamma via likelihood ratio (PLAN_HYPERDUAL.md §5, revisado). Devuelve `std::nullopt` si
// la combinacion (modelo, metrica) no esta en la tabla, si `risk_factor.name != "spot"`, si el
// contrato no depende de una unica fecha terminal (`payoff_supports_second_order_lrm[_p]` -- esto
// EXCLUYE estructuralmente cualquier contrato con `ContractOp::Exercise`, que por construccion
// necesita mas de una fecha), o si el producto no es un `PayoffProduct`. Mismo criterio que
// `try_pathwise`: nunca lanza, solo informa "no aplica".
std::optional<GreekResult> try_pathwise2(
    const GreekRequest& request, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    const std::string model_type = model.type_name();
    if (!capability_listed(pathwise2_capabilities(), model_type, request.metric_name)) return std::nullopt;
    if (request.risk_factor.name != "spot") return std::nullopt;

    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) return std::nullopt;

    // PLAN_IMPROVE_NOTEBOOK2.md Fase 0 (ADR-IN2-01): mismo motivo que `try_pathwise` --
    // `payoff_sensitivity2_gbm[_p]` tampoco recibe `pricing.pricing_date()`. Bajo method=Auto cae
    // al estencil generico de 3 puntos (Gamma), que SI honra `pricing_date` via `metric->evaluate`.
    if (pricing.pricing_date() != 0.0) return std::nullopt;

    payoff::SensitivityResult sensitivity;
    if (model_type == "GBM") {
        if (!payoff::payoff_supports_second_order_lrm(*payoff_product->payoff_program())) return std::nullopt;
        sensitivity = payoff::payoff_sensitivity2_gbm(
            *payoff_product->payoff_program(), dynamic_cast<const GbmModel&>(model), request.risk_factor.name,
            pricing.n_paths(), pricing.seed()
        );
    } else if (model_type == "GBM_P") {
        if (!payoff::payoff_supports_second_order_lrm_p(*payoff_product->payoff_program())) return std::nullopt;
        sensitivity = payoff::payoff_sensitivity2_gbm_p(
            *payoff_product->payoff_program(), dynamic_cast<const GbmPModel&>(model), request.risk_factor.name,
            pricing.n_paths(), pricing.seed()
        );
    } else {
        return std::nullopt; // inalcanzable dada pathwise2_capabilities(), defensivo
    }

    GreekResult result;
    result.has_scalar = true;
    result.value = sensitivity.value;
    result.std_error = sensitivity.std_error;
    result.order = request.order;
    result.risk_factor = request.risk_factor;
    result.method_used = GreekMethod::Pathwise;
    result.measure = sensitivity.measure;
    return result;
}

// Intenta Vanna via likelihood ratio -- mismo criterio que `try_pathwise2`, solo el par
// ("spot","volatility") en cualquier orden.
std::optional<GreekResult> try_pathwise_cross(
    const GreekRequest& request, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    const std::string model_type = model.type_name();
    if (!capability_listed(pathwise_cross_capabilities(), model_type, request.metric_name)) return std::nullopt;
    if (request.risk_factor.kind != RiskFactorKind::ModelParameter) return std::nullopt;
    if (!request.order.cross_factor.has_value()) return std::nullopt;
    const RiskFactor& cross = *request.order.cross_factor;
    if (cross.kind != RiskFactorKind::ModelParameter) return std::nullopt;
    const bool is_vanna_pair = (request.risk_factor.name == "spot" && cross.name == "volatility") ||
                               (request.risk_factor.name == "volatility" && cross.name == "spot");
    if (!is_vanna_pair) return std::nullopt;

    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) return std::nullopt;

    // PLAN_IMPROVE_NOTEBOOK2.md Fase 0 (ADR-IN2-01): mismo motivo que `try_pathwise`/
    // `try_pathwise2` -- `payoff_sensitivity_cross_gbm[_p]` tampoco recibe `pricing.pricing_date()`.
    // Bajo method=Auto cae al estencil generico de 4 puntos (Vanna/cross-gamma), que SI honra
    // `pricing_date` via `metric->evaluate`.
    if (pricing.pricing_date() != 0.0) return std::nullopt;

    payoff::SensitivityResult sensitivity;
    if (model_type == "GBM") {
        if (!payoff::payoff_supports_second_order_lrm(*payoff_product->payoff_program())) return std::nullopt;
        sensitivity = payoff::payoff_sensitivity_cross_gbm(
            *payoff_product->payoff_program(), dynamic_cast<const GbmModel&>(model), request.risk_factor.name,
            cross.name, pricing.n_paths(), pricing.seed()
        );
    } else if (model_type == "GBM_P") {
        if (!payoff::payoff_supports_second_order_lrm_p(*payoff_product->payoff_program())) return std::nullopt;
        sensitivity = payoff::payoff_sensitivity_cross_gbm_p(
            *payoff_product->payoff_program(), dynamic_cast<const GbmPModel&>(model), request.risk_factor.name,
            cross.name, pricing.n_paths(), pricing.seed()
        );
    } else {
        return std::nullopt; // inalcanzable dada pathwise_cross_capabilities(), defensivo
    }

    GreekResult result;
    result.has_scalar = true;
    result.value = sensitivity.value;
    result.std_error = sensitivity.std_error;
    result.order = request.order;
    result.risk_factor = request.risk_factor;
    result.method_used = GreekMethod::Pathwise;
    result.measure = sensitivity.measure;
    return result;
}

// Intenta la ruta AAD reverse-mode (PLAN_GREEKS.md §5.2). A diferencia de `try_pathwise`, el
// único chequeo dinámico es "¿el modelo declara este parámetro?" -- `resolve_model_parameter_key`
// ya lanza un error explícito antes de llegar aquí si no (ver compute_greek), así que
// `try_aad_reverse` solo devuelve `nullopt` cuando la combinación (modelo, métrica) no está en la
// tabla de capacidades, o cuando el parámetro concreto pedido (p.ej. `rho` de HullWhite2F) no
// tiene gradiente disponible aunque el (modelo, métrica) sí esté en la tabla.
std::optional<GreekResult> try_aad_reverse(
    const GreekRequest& request, const IModel& model, const IProduct& product
) {
    const std::string model_type = model.type_name();
    if (!capability_listed(aad_capabilities(), model_type, request.metric_name)) return std::nullopt;

    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) return std::nullopt; // defensivo: la tabla solo lista IrSwapProduct

    double value = 0.0;
    if (model_type == "HullWhite1F") {
        const auto& hw1f = dynamic_cast<const HullWhite1FModel&>(model);
        HullWhite1FGreeks greeks = irs_hull_white_npv_all_greeks(
            hw1f.a(), hw1f.b(), hw1f.sigma(), hw1f.r0(), irs_product->notional(), irs_product->fixed_rate(),
            irs_product->use_par_rate(), irs_product->start(), irs_product->payment_times(), irs_product->accruals()
        );
        if (request.risk_factor.name == "a") value = greeks.d_a;
        else if (request.risk_factor.name == "b") value = greeks.d_b;
        else if (request.risk_factor.name == "sigma") value = greeks.d_sigma;
        else if (request.risk_factor.name == "r0") value = greeks.d_r0;
        else return std::nullopt; // inalcanzable: resolve_model_parameter_key ya validó el nombre
    } else if (model_type == "HullWhite2F") {
        const auto& hw2f = dynamic_cast<const HullWhite2FModel&>(model);
        if (request.risk_factor.name == "rho") return std::nullopt; // §5.2: rho no es diferenciable via AAD
        HullWhite2FGreeks greeks = irs_hull_white_2f_npv_all_greeks(
            hw2f.a(), hw2f.b(), hw2f.sigma(), hw2f.eta(), hw2f.rho(), hw2f.r0(), irs_product->notional(),
            irs_product->fixed_rate(), irs_product->use_par_rate(), irs_product->start(),
            irs_product->payment_times(), irs_product->accruals()
        );
        if (request.risk_factor.name == "a") value = greeks.d_a;
        else if (request.risk_factor.name == "b") value = greeks.d_b;
        else if (request.risk_factor.name == "sigma") value = greeks.d_sigma;
        else if (request.risk_factor.name == "eta") value = greeks.d_eta;
        else if (request.risk_factor.name == "r0") value = greeks.d_r0;
        else return std::nullopt; // inalcanzable: resolve_model_parameter_key ya validó el nombre
    } else {
        return std::nullopt; // inalcanzable dada aad_capabilities(), defensivo
    }

    GreekResult result;
    result.has_scalar = true;
    result.value = value;
    result.order = request.order;
    result.risk_factor = request.risk_factor;
    result.method_used = GreekMethod::AadReverse;
    result.measure = payoff::ProbabilityMeasure::DeterministicScenario;
    return result;
}

// Mensaje explícito para `method='pathwise'` pedido a mano sobre una combinación no soportada
// (§4.5/§5.1: nunca aproxima en silencio, siempre nombra la razón exacta).
std::string pathwise_unsupported_reason(
    const GreekRequest& request, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    if (indicator_type_metrics().count(request.metric_name) != 0) {
        return "compute_greek: metodo 'pathwise' no soportado para '" + request.metric_name +
               "' (metrica de tipo indicador/probabilidad -- su derivada pathwise exacta es 0 en casi todo "
               "punto y no informa nada, PLAN_GREEKS.md §4.5; usa method='auto' o 'bump_and_reval')";
    }
    if (!capability_listed(pathwise_capabilities(), model.type_name(), request.metric_name)) {
        return "compute_greek: metodo 'pathwise' no soportado para (modelo='" + model.type_name() +
               "', metrica='" + request.metric_name + "') -- combinacion no verificada contra "
               "bump-and-reval todavia (PLAN_GREEKS.md §5.3)";
    }
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (payoff_product != nullptr && payoff::payoff_contains_exercise(*payoff_product->payoff_program())) {
        return "compute_greek: metodo 'pathwise' no soportado para un contrato con ContractOp::Exercise "
               "(re-decidir Longstaff-Schwartz bajo el parametro perturbado no es pathwise-diferenciable, "
               "PLAN_GREEKS.md §5.1; usa method='auto' o 'bump_and_reval')";
    }
    if (pricing.pricing_date() != 0.0) {
        return "compute_greek: metodo 'pathwise' no soportado para (modelo='" + model.type_name() + "', metrica='" +
               request.metric_name + "') con PricingContext::pricing_date() != 0 (recibido " +
               std::to_string(pricing.pricing_date()) + ") -- la ruta pathwise simula siempre desde 'hoy'=0 "
               "(payoff_sensitivity_gbm[_p], Rust) y ese desplazamiento se perderia en silencio "
               "(PLAN_IMPROVE_NOTEBOOK2.md Fase 0); usa method='auto' (cae a bump_and_reval, que SI honra "
               "pricing_date) o method='bump_and_reval' explicito";
    }
    return "compute_greek: metodo 'pathwise' no soportado para (modelo='" + model.type_name() + "', metrica='" +
           request.metric_name + "')";
}

// Equivalente de `pathwise_unsupported_reason` para Gamma via likelihood ratio (order=2 puro).
std::string pathwise2_unsupported_reason(
    const GreekRequest& request, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    if (request.risk_factor.name != "spot") {
        return "compute_greek: metodo 'pathwise' de segundo orden solo soportado para 'model.spot' (Gamma) -- "
               "recibido 'model." + request.risk_factor.name + "', PLAN_HYPERDUAL.md §5";
    }
    if (!capability_listed(pathwise2_capabilities(), model.type_name(), request.metric_name)) {
        return "compute_greek: metodo 'pathwise' de segundo orden no soportado para (modelo='" + model.type_name() +
               "', metrica='" + request.metric_name + "') -- combinacion no verificada todavia (PLAN_HYPERDUAL.md §6)";
    }
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    const bool supports_lrm = payoff_product != nullptr &&
        (model.type_name() == "GBM" ? payoff::payoff_supports_second_order_lrm(*payoff_product->payoff_program())
                                     : payoff::payoff_supports_second_order_lrm_p(*payoff_product->payoff_program()));
    if (payoff_product != nullptr && !supports_lrm) {
        return "compute_greek: metodo 'pathwise' de segundo orden no soportado para un contrato "
               "path-dependiente (Gamma via likelihood ratio solo aplica a una unica fecha terminal, "
               "PLAN_HYPERDUAL.md §5; usa method='auto' o 'bump_and_reval')";
    }
    if (pricing.pricing_date() != 0.0) {
        return "compute_greek: metodo 'pathwise' de segundo orden no soportado para (modelo='" + model.type_name() +
               "', metrica='" + request.metric_name + "') con PricingContext::pricing_date() != 0 (recibido " +
               std::to_string(pricing.pricing_date()) + ") -- mismo motivo que la Gamma via likelihood ratio "
               "nunca recibe `pricing.pricing_date()` (PLAN_IMPROVE_NOTEBOOK2.md Fase 0); usa method='auto' "
               "(cae al estencil generico de bump-and-reval de 3 puntos) o method='bump_and_reval' explicito";
    }
    return "compute_greek: metodo 'pathwise' de segundo orden no soportado para (modelo='" + model.type_name() +
           "', metrica='" + request.metric_name + "')";
}

// Equivalente de `pathwise2_unsupported_reason` para Vanna (order=1 con cross_factor).
std::string pathwise_cross_unsupported_reason(
    const GreekRequest& request, const IModel& model, const IProduct& product, const PricingContext& pricing
) {
    const std::string cross_name = request.order.cross_factor.has_value() ? request.order.cross_factor->name : "";
    const bool is_vanna_pair = (request.risk_factor.name == "spot" && cross_name == "volatility") ||
                               (request.risk_factor.name == "volatility" && cross_name == "spot");
    if (!is_vanna_pair) {
        return "compute_greek: metodo 'pathwise' de derivada cruzada solo soportado para el par "
               "('model.spot','model.volatility') (Vanna) -- recibido ('model." + request.risk_factor.name +
               "','model." + cross_name + "'), PLAN_HYPERDUAL.md §5";
    }
    if (!capability_listed(pathwise_cross_capabilities(), model.type_name(), request.metric_name)) {
        return "compute_greek: metodo 'pathwise' de derivada cruzada no soportado para (modelo='" +
               model.type_name() + "', metrica='" + request.metric_name +
               "') -- combinacion no verificada todavia (PLAN_HYPERDUAL.md §6)";
    }
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    const bool supports_lrm = payoff_product != nullptr &&
        (model.type_name() == "GBM" ? payoff::payoff_supports_second_order_lrm(*payoff_product->payoff_program())
                                     : payoff::payoff_supports_second_order_lrm_p(*payoff_product->payoff_program()));
    if (payoff_product != nullptr && !supports_lrm) {
        return "compute_greek: metodo 'pathwise' de derivada cruzada no soportado para un contrato "
               "path-dependiente (Vanna via likelihood ratio solo aplica a una unica fecha terminal, "
               "PLAN_HYPERDUAL.md §5; usa method='auto' o 'bump_and_reval')";
    }
    if (pricing.pricing_date() != 0.0) {
        return "compute_greek: metodo 'pathwise' de derivada cruzada no soportado para (modelo='" +
               model.type_name() + "', metrica='" + request.metric_name + "') con "
               "PricingContext::pricing_date() != 0 (recibido " + std::to_string(pricing.pricing_date()) +
               ") -- mismo motivo que Vanna via likelihood ratio nunca recibe `pricing.pricing_date()` "
               "(PLAN_IMPROVE_NOTEBOOK2.md Fase 0); usa method='auto' (cae al estencil generico de "
               "bump-and-reval de 4 puntos) o method='bump_and_reval' explicito";
    }
    return "compute_greek: metodo 'pathwise' de derivada cruzada no soportado para (modelo='" + model.type_name() +
           "', metrica='" + request.metric_name + "')";
}

// Equivalente de `pathwise_unsupported_reason` para `method='aad'`.
std::string aad_unsupported_reason(const GreekRequest& request, const IModel& model) {
    if (!capability_listed(aad_capabilities(), model.type_name(), request.metric_name)) {
        return "compute_greek: metodo 'aad' no soportado para (modelo='" + model.type_name() + "', metrica='" +
               request.metric_name + "') -- combinacion no verificada contra bump-and-reval todavia "
               "(PLAN_GREEKS.md §5.3)";
    }
    if (model.type_name() == "HullWhite2F" && request.risk_factor.name == "rho") {
        return "compute_greek: metodo 'aad' no soportado para 'model.rho' de HullWhite2F ('rho' no es un "
               "tensor diferenciable en la implementacion actual del modelo, PLAN_GREEKS.md §5.2; usa "
               "method='auto' o 'bump_and_reval')";
    }
    return "compute_greek: metodo 'aad' no soportado para (modelo='" + model.type_name() + "', metrica='" +
           request.metric_name + "')";
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
    if (text == "likelihood_ratio_hessian") return GreekMethod::LikelihoodRatioHessian;
    if (text == "aad_forward_over_forward") return GreekMethod::AadForwardOverForward;
    throw std::invalid_argument(
        "GreekMethod: '" + text + "' desconocido (validos: 'auto', 'bump_and_reval', 'pathwise', 'aad', "
        "'likelihood_ratio_hessian', 'aad_forward_over_forward')"
    );
}

std::string to_string(GreekMethod method) {
    switch (method) {
        case GreekMethod::Auto: return "auto";
        case GreekMethod::BumpAndReval: return "bump_and_reval";
        case GreekMethod::Pathwise: return "pathwise";
        case GreekMethod::AadReverse: return "aad";
        case GreekMethod::LikelihoodRatioHessian: return "likelihood_ratio_hessian";
        case GreekMethod::AadForwardOverForward: return "aad_forward_over_forward";
    }
    throw std::logic_error("engine::greeks::to_string(GreekMethod): GreekMethod desconocido"); // inalcanzable
}

std::string to_string(payoff::ProbabilityMeasure measure) {
    switch (measure) {
        case payoff::ProbabilityMeasure::RiskNeutralQ: return "RiskNeutralQ";
        case payoff::ProbabilityMeasure::PhysicalP: return "PhysicalP";
        case payoff::ProbabilityMeasure::DeterministicScenario: return "DeterministicScenario";
    }
    throw std::logic_error("engine::greeks::to_string(ProbabilityMeasure): valor desconocido"); // inalcanzable
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
        // PLAN_GREEKS.md §5.4: las rutas especializadas SOLO cubren un ModelParameter -- pedirlas
        // explicitamente fuera de ese alcance es un error inmediato (nunca se degrada en silencio
        // a BumpAndReval).
        if (request.risk_factor.kind != RiskFactorKind::ModelParameter) {
            throw std::invalid_argument(
                "compute_greek: metodo '" + to_string(request.method) + "' solo soporta "
                "RiskFactorKind::ModelParameter (pedido: '" + to_string(request.risk_factor) + "')"
            );
        }
        // AAD reverse-mode (Hull-White) sigue restringido a order=1 sin cross_factor -- esta
        // revision (PLAN_HYPERDUAL.md) no toca esa ruta (§5.4, ultimo punto). `Pathwise` en
        // cambio ahora cubre tambien order=2 puro (Gamma, try_pathwise2) y order=1 con
        // cross_factor (Vanna, try_pathwise_cross) -- la forma exacta la valida el propio
        // try_pathwise2/try_pathwise_cross via su tabla de capacidades; order=2 CON cross_factor
        // ya se rechazo mas arriba (tercera derivada, fuera de alcance) antes de llegar aqui.
        if (request.method == GreekMethod::AadReverse &&
            (request.order.order != 1 || request.order.cross_factor.has_value())) {
            throw std::invalid_argument(
                "compute_greek: metodo 'aad' solo soporta order=1 sin cross_factor (Gamma/derivadas "
                "cruzadas de Hull-White via AAD reverse-mode fuera de alcance, PLAN_GREEKS.md §5.2)"
            );
        }
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

    // Rutas especializadas (PLAN_GREEKS.md §5, Fase 7): solo order=1 sin cross_factor sobre un
    // ModelParameter -- la validacion de arriba ya garantiza esa forma para method=Pathwise/
    // AadReverse explicito; para method=Auto se comprueba aqui mismo antes de intentar nada.
    if (request.order.order == 1 && !request.order.cross_factor.has_value() &&
        request.risk_factor.kind == RiskFactorKind::ModelParameter) {
        if (request.method == GreekMethod::Pathwise) {
            std::optional<GreekResult> specialized = try_pathwise(request, model, product, pricing);
            if (specialized.has_value()) return *specialized;
            throw std::invalid_argument(pathwise_unsupported_reason(request, model, product, pricing));
        }
        if (request.method == GreekMethod::AadReverse) {
            std::optional<GreekResult> specialized = try_aad_reverse(request, model, product);
            if (specialized.has_value()) return *specialized;
            throw std::invalid_argument(aad_unsupported_reason(request, model));
        }
        if (request.method == GreekMethod::Auto) {
            // §3.3: la especializacion mas rapida VERIFICADA si existe (pathwise antes que AAD,
            // sin que eso implique una prioridad economica -- hoy nunca coinciden (modelo,
            // metrica) en ambas tablas a la vez); si ninguna aplica, cae a BumpAndReval sin que
            // el llamante lo note salvo por GreekResult::method_used.
            std::optional<GreekResult> specialized = try_pathwise(request, model, product, pricing);
            if (!specialized.has_value()) specialized = try_aad_reverse(request, model, product);
            if (specialized.has_value()) return *specialized;
        }
    }

    // Gamma via likelihood ratio (PLAN_HYPERDUAL.md §5, revisado): order=2 puro sobre un
    // ModelParameter -- mismo patron que el bloque de arriba, restringido a "spot" y a contratos
    // de una unica fecha terminal (`try_pathwise2` lo comprueba dinamicamente).
    if (request.order.order == 2 && !request.order.cross_factor.has_value() &&
        request.risk_factor.kind == RiskFactorKind::ModelParameter) {
        if (request.method == GreekMethod::Pathwise) {
            std::optional<GreekResult> specialized = try_pathwise2(request, model, product, pricing);
            if (specialized.has_value()) return *specialized;
            throw std::invalid_argument(pathwise2_unsupported_reason(request, model, product, pricing));
        }
        if (request.method == GreekMethod::Auto) {
            std::optional<GreekResult> specialized = try_pathwise2(request, model, product, pricing);
            if (specialized.has_value()) return *specialized;
        }
    }

    if (request.order.cross_factor.has_value()) {
        // Vanna via likelihood ratio (PLAN_HYPERDUAL.md §5, revisado): se intenta ANTES del
        // estencil generico de 4 puntos, mismo patron que try_pathwise/try_pathwise2. `method=
        // Pathwise` explicito con cross_factor SIEMPRE se resuelve aqui (exito o error inmediato
        // con la razon exacta, via `try_pathwise_cross`/`pathwise_cross_unsupported_reason`, que
        // ya comprueban `kind`/nombres/capacidad/alcance) -- nunca cae en silencio al estencil
        // generico de abajo, igual que el bloque de order=1 sin cross_factor de mas arriba.
        if (request.method == GreekMethod::Pathwise) {
            std::optional<GreekResult> specialized = try_pathwise_cross(request, model, product, pricing);
            if (specialized.has_value()) return *specialized;
            throw std::invalid_argument(pathwise_cross_unsupported_reason(request, model, product, pricing));
        }
        if (request.method == GreekMethod::Auto) {
            std::optional<GreekResult> specialized = try_pathwise_cross(request, model, product, pricing);
            if (specialized.has_value()) return *specialized;
        }

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

// Compara dos RiskFactor por contenido (RiskFactor no define operator==) -- usado por
// `compute_hessian` para deduplicar/filtrar factores sin asumir mas que igualdad estructural.
bool same_risk_factor(const RiskFactor& lhs, const RiskFactor& rhs) {
    return lhs.kind == rhs.kind && lhs.scope == rhs.scope && lhs.name == rhs.name && lhs.pillar_index == rhs.pillar_index;
}

// Factores distintos que aparecen (como factor_i o factor_j) en un conjunto de HessianEntry --
// generaliza "{spot, volatility}"/"{a,b,sigma,r0}" sin necesitar que compute_hessian conozca de
// antemano cual es el conjunto valido para cada (modelo,metrica): ya viene implicito en las
// entradas que devolvio try_hessian_likelihood_ratio/try_hessian_forward_over_forward.
std::vector<RiskFactor> distinct_factors_in(const std::vector<HessianEntry>& entries) {
    std::vector<RiskFactor> factors;
    auto contains = [&](const RiskFactor& f) {
        return std::any_of(factors.begin(), factors.end(), [&](const RiskFactor& g) { return same_risk_factor(f, g); });
    };
    for (const HessianEntry& entry : entries) {
        if (!contains(entry.factor_i)) factors.push_back(entry.factor_i);
        if (!contains(entry.factor_j)) factors.push_back(entry.factor_j);
    }
    return factors;
}

// Hessiano local de un unico trade, "mejor esfuerzo" (PLAN_BACKWARD.md §7/§9 Fase 1-3) -- a
// diferencia de `compute_greek`, NUNCA lanza sobre una combinacion no soportada: todo lo no
// cubierto va a `skipped` con el motivo exacto (mismo criterio que `compute_all_greeks`). Intenta,
// en orden, `try_hessian_likelihood_ratio` (GBM/GBM_P, factores en {spot, volatility}, Fase 1) y
// `try_hessian_forward_over_forward` (HullWhite1F via Dual2/HyperDual, factores en
// {a,b,sigma,r0}, Fase 2; HullWhite2F, factores en {a,b,sigma,eta,r0} -- sin "rho", Fase 3) -- las
// dos tablas de capacidades son disjuntas (ningun modelo aparece en ambas), asi que el orden no
// importa. La enumeracion automatica (`factors` vacio) es exactamente el conjunto de factores que
// la especializacion aplicable produjo (documentado explicitamente aqui: {spot,volatility} para
// GBM/GBM_P, {a,b,sigma,r0} para HullWhite1F, {a,b,sigma,eta,r0} para HullWhite2F).
//
// PLAN_IMPROVE_NOTEBOOK2.md Fase 2 (decision (b), ver el doc-comment junto a `!applied` mas abajo
// para el detalle): un contrato GBM/GBM_P que no depende de una unica fecha terminal (p.ej. un
// calendar spread, `payoff_supports_second_order_lrm[_p]` en `false`) tiene su PROPIA entrada de
// `skipped` distinguible del caso generico "(modelo, metrica) no cubierta" -- nunca queda
// completamente ausente ni se confunde con "esta combinacion de (modelo, metrica) nunca fue
// cableada". No existe fallback bump-and-reval generico de segundo orden/cruzado para este caso
// (mas caro: 3-4 revaluaciones por entrada -- ver el texto de la fase); queda como "Fase 2b"
// explicitamente pendiente si en el futuro se quiere el numero real en vez de solo el motivo.
HessianReport compute_hessian(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution, const std::vector<RiskFactor>& factors
) {
    // Fase 1-2 solo necesitan likelihood ratio (payoff GBM/GBM_P) y el pricer cerrado de
    // Hull-White (via el modelo/producto ya recibidos), ninguno de los dos toca `registries`/
    // `metric_params`/`market`/`execution` -- Fase 3+ SI podria necesitarlos al cablear mas
    // especializaciones aqui, de ahi que la firma ya los incluya.
    (void)registries;
    (void)metric_params;
    (void)market;
    (void)execution;

    HessianReport report;

    std::vector<HessianEntry> entries;
    bool applied = false;
    if (std::optional<std::array<HessianEntry, 3>> lr = try_hessian_likelihood_ratio(metric_name, model, product, pricing);
        lr.has_value()) {
        entries.assign(lr->begin(), lr->end());
        applied = true;
    } else if (std::optional<std::vector<HessianEntry>> fof = try_hessian_forward_over_forward(metric_name, model, product);
               fof.has_value()) {
        entries = std::move(*fof);
        applied = true;
    }

    if (!applied) {
        // PLAN_IMPROVE_NOTEBOOK2.md Fase 0 (ADR-IN2-01): si la combinacion (modelo, metrica) SI
        // esta en `hessian_capabilities()` via likelihood ratio pero `pricing.pricing_date() != 0`,
        // `try_hessian_likelihood_ratio` ya devolvio `nullopt` por ese motivo especifico (no por no
        // estar cubierta) -- mensaje mas preciso que el generico de abajo. No hay fallback
        // bump-and-reval generico de segundo orden todavia (eso es la Fase 2 de este mismo plan).
        const bool capability_present = (model.type_name() == "GBM" || model.type_name() == "GBM_P") &&
            capability_listed(hessian_capabilities(), model.type_name(), metric_name);
        if (capability_present && pricing.pricing_date() != 0.0) {
            report.skipped.push_back(
                "Hessiano local: (" + model.type_name() + ", " + metric_name + ") via likelihood ratio no "
                "soporta PricingContext::pricing_date() != 0 (recibido " + std::to_string(pricing.pricing_date()) +
                ") -- payoff_local_hessian_gbm[_p] (Rust) nunca recibe pricing_date, honrarlo en silencio daria "
                "un Hessiano incorrecto (PLAN_IMPROVE_NOTEBOOK2.md Fase 0); no existe fallback bump-and-reval "
                "generico de segundo orden todavia (ver Fase 2 de PLAN_IMPROVE_NOTEBOOK2.md)"
            );
            return report;
        }
        // PLAN_IMPROVE_NOTEBOOK2.md Fase 2: (modelo, metrica) SI esta en `hessian_capabilities()`
        // y `pricing_date() == 0`, pero `try_hessian_likelihood_ratio` igual devolvio `nullopt` --
        // el UNICO motivo restante posible con esas dos condiciones ya descartadas es que el
        // contrato no depende de una unica fecha terminal (`payoff_supports_second_order_lrm[_p]`
        // en `false`, p.ej. un calendar spread con patas en T distintas sobre el mismo
        // observable -- mismo criterio estructural que excluye `ContractOp::Exercise`). Antes de
        // esta fase, este caso caia en el mensaje generico de abajo ("no esta cubierta por
        // hessian_capabilities()"), que es enganoso: la combinacion SI esta cubierta, solo que
        // este contrato concreto no cumple la condicion dinamica de una unica fecha. Decision
        // (b) de esta fase (ver PLAN_IMPROVE_NOTEBOOK2.md): no se implementa un fallback
        // bump-and-reval generico de segundo orden/cruzado para este caso (alcance "esfuerzo
        // alto", fuera de esta fase de "esfuerzo medio") -- se deja como Fase 2b pendiente. Aqui
        // solo se nombra el motivo exacto en `skipped`, nunca se omite en silencio.
        if (capability_present) {
            const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
            if (payoff_product != nullptr) {
                const bool supports_lrm = model.type_name() == "GBM"
                    ? payoff::payoff_supports_second_order_lrm(*payoff_product->payoff_program())
                    : payoff::payoff_supports_second_order_lrm_p(*payoff_product->payoff_program());
                if (!supports_lrm) {
                    report.skipped.push_back(
                        "Hessiano local: (" + model.type_name() + ", " + metric_name + ") via likelihood ratio "
                        "solo cubre contratos que dependen de una unica fecha terminal (mismo criterio "
                        "estructural que excluye ContractOp::Exercise, ver payoff_supports_second_order_lrm" +
                        std::string(model.type_name() == "GBM" ? "" : "_p") + "); este contrato depende de mas "
                        "de una fecha (p.ej. un calendar spread con patas en T distintas sobre el mismo "
                        "observable) -- no existe fallback bump-and-reval generico de segundo orden/cruzado "
                        "para este caso todavia (PLAN_IMPROVE_NOTEBOOK2.md Fase 2, decision (b): solo skip "
                        "explicito con el motivo exacto, sin fallback numerico -- ver Fase 2b pendiente)"
                    );
                    return report;
                }
            }
        }
        report.skipped.push_back(
            "Hessiano local: (" + model.type_name() + ", " + metric_name + ") no esta cubierta por "
            "hessian_capabilities() ({GBM,PayoffPriceQ}/{GBM_P,PayoffForecastP} via likelihood ratio, "
            "{HullWhite1F,HullWhite2F}x{HullWhiteModelNpv} via forward-over-forward) -- ver "
            "PLAN_BACKWARD.md §9 Fase 1-3"
        );
        return report;
    }

    if (factors.empty()) {
        // Enumeracion automatica (§9): exactamente los factores que produjo la especializacion
        // aplicable -- {spot, volatility} para GBM/GBM_P, {a,b,sigma,r0} para HullWhite1F.
        report.entries = std::move(entries);
        return report;
    }

    // Interseccion de los factores pedidos con los que la especializacion aplicable soporta --
    // cualquier factor pedido fuera de ese conjunto va a `skipped` con el motivo exacto, nunca se
    // ignora en silencio.
    const std::vector<RiskFactor> valid_factors = distinct_factors_in(entries);
    auto is_valid = [&](const RiskFactor& f) {
        return std::any_of(valid_factors.begin(), valid_factors.end(), [&](const RiskFactor& g) { return same_risk_factor(f, g); });
    };

    std::vector<RiskFactor> requested;
    for (const RiskFactor& factor : factors) {
        if (is_valid(factor)) {
            requested.push_back(factor);
        } else {
            report.skipped.push_back(
                "Hessiano local: factor '" + to_string(factor) + "' fuera de los factores soportados por "
                "(" + model.type_name() + ", " + metric_name + ") -- ver hessian_capabilities(), PLAN_BACKWARD.md §9"
            );
        }
    }

    auto was_requested = [&](const RiskFactor& f) {
        return std::any_of(requested.begin(), requested.end(), [&](const RiskFactor& g) { return same_risk_factor(f, g); });
    };

    for (const HessianEntry& entry : entries) {
        if (was_requested(entry.factor_i) && was_requested(entry.factor_j)) {
            report.entries.push_back(entry);
        }
    }

    return report;
}

// Producto Hessiano-vector "H*v" (PLAN_BACKWARD.md §5/§7/§9 Fase 3) -- se apoya en
// `compute_hessian` YA EXISTENTE en vez de repetir dispatch por modelo: funciona igual de bien
// para Hull-White (`AadForwardOverForward`) que para GBM/GBM_P (`LikelihoodRatioHessian`) sin
// código nuevo por modelo. `direction.size() != factors.size()` es un error del llamante (no hay
// forma razonable de emparejar direcciones con factores si no coinciden en longitud) -- se lanza
// en vez de degradarse en silencio, mismo criterio que el resto del motor de Greeks para errores
// estructurales de la petición (PLAN_GREEKS.md §5.3/§5.4).
HvpReport compute_hvp(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    const std::vector<RiskFactor>& factors, const std::vector<double>& direction
) {
    if (direction.size() != factors.size()) {
        throw std::invalid_argument(
            "compute_hvp: direction.size() (" + std::to_string(direction.size()) + ") debe coincidir con "
            "factors.size() (" + std::to_string(factors.size()) + ") -- direction[i] corresponde a factors[i]"
        );
    }

    HessianReport hessian = compute_hessian(
        registries, metric_name, metric_params, model, product, market, pricing, execution, factors
    );

    // Busca H[i][j] entre (factors[i],factors[j]) y (factors[j],factors[i]) -- misma entrada por
    // simetria (compute_hessian solo guarda el triangulo superior + diagonal).
    auto find_entry = [&](const RiskFactor& fi, const RiskFactor& fj) -> const HessianEntry* {
        for (const HessianEntry& entry : hessian.entries) {
            if ((same_risk_factor(entry.factor_i, fi) && same_risk_factor(entry.factor_j, fj)) ||
                (same_risk_factor(entry.factor_i, fj) && same_risk_factor(entry.factor_j, fi))) {
                return &entry;
            }
        }
        return nullptr;
    };

    HvpReport report;
    for (std::size_t i = 0; i < factors.size(); ++i) {
        // Fila i completa: hace falta H[i][j] para TODO j en 0..factors.size(), incluida la
        // diagonal -- si falta cualquiera, el componente i de H*v no se puede calcular sin
        // inventar un 0.0 silencioso, asi que se reporta en `skipped` y se omite.
        bool row_complete = true;
        for (std::size_t j = 0; j < factors.size() && row_complete; ++j) {
            row_complete = find_entry(factors[i], factors[j]) != nullptr;
        }
        if (!row_complete) {
            report.skipped.push_back(
                "HVP: fila del Hessiano incompleta para el factor '" + to_string(factors[i]) + "' -- "
                "ver HessianReport::skipped de compute_hessian (via este mismo compute_hvp) para el motivo exacto"
            );
            continue;
        }

        double hv_i = 0.0;
        for (std::size_t j = 0; j < factors.size(); ++j) {
            hv_i += find_entry(factors[i], factors[j])->value * direction[j];
        }
        const HessianEntry* diagonal = find_entry(factors[i], factors[i]);
        report.components.push_back(HvpComponent{factors[i], hv_i, diagonal->method_used});
    }

    for (const std::string& reason : hessian.skipped) {
        report.skipped.push_back("HVP: " + reason);
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
    result.has_scalar = out.has_scalar;
    result.scalar = out.value;
    result.times = out.times;
    result.primary = out.primary;
    result.secondary = out.secondary;
    // PLAN_IMPROVE_NOTEBOOK2.md Fase 5: propaga el bump efectivo tal cual lo dejó
    // `compute_greek` -- `std::nullopt` si el método realmente ejecutado no usó bump numérico
    // (AAD/pathwise), nunca un valor inventado.
    result.bump_used = out.bump_used;
    return result;
}

} // namespace engine
