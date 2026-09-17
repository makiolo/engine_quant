#include "engine/model.hpp"

#include <stdexcept>

namespace engine {

namespace {

// Parseo del string delimitado por comas de "observables" (PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2
// punto 4, Opcion A): sin dependencia nueva, `std::string::find` es suficiente para un separador
// de un unico caracter. Cada elemento se recorta de espacios en blanco alrededor (comodidad para
// quien escriba "A, B, C" a mano) pero NUNCA se descarta un elemento vacio en silencio -- un
// elemento vacio (p.ej. "A,,B" o una coma final "A,B,") es un error explicito, no un activo
// fantasma.
std::vector<std::string> split_comma_separated(const std::string& value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t comma = value.find(',', start);
        std::size_t end = (comma == std::string::npos) ? value.size() : comma;
        std::string token = value.substr(start, end - start);
        std::size_t first = token.find_first_not_of(" \t");
        std::size_t last = token.find_last_not_of(" \t");
        if (first == std::string::npos) {
            throw std::invalid_argument(
                "GbmBasketModel: 'observables' contiene un elemento vacio (revisa comas dobles o finales): '" +
                value + "'"
            );
        }
        out.push_back(token.substr(first, last - first + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

} // namespace

HullWhite1FModel::HullWhite1FModel(const Params& params)
    : a_(get_double(params, "a")),
      b_(get_double(params, "b")),
      sigma_(get_double(params, "sigma")),
      r0_(get_double(params, "r0")) {}

Params HullWhite1FModel::to_params() const {
    return Params{{"a", a_}, {"b", b_}, {"sigma", sigma_}, {"r0", r0_}};
}

double HullWhite1FModel::a() const { return a_; }
double HullWhite1FModel::b() const { return b_; }
double HullWhite1FModel::sigma() const { return sigma_; }
double HullWhite1FModel::r0() const { return r0_; }

HullWhite2FModel::HullWhite2FModel(const Params& params)
    : a_(get_double(params, "a")),
      b_(get_double(params, "b")),
      sigma_(get_double(params, "sigma")),
      eta_(get_double(params, "eta")),
      rho_(get_double(params, "rho")),
      r0_(get_double(params, "r0")) {}

Params HullWhite2FModel::to_params() const {
    return Params{{"a", a_}, {"b", b_}, {"sigma", sigma_}, {"eta", eta_}, {"rho", rho_}, {"r0", r0_}};
}

double HullWhite2FModel::a() const { return a_; }
double HullWhite2FModel::b() const { return b_; }
double HullWhite2FModel::sigma() const { return sigma_; }
double HullWhite2FModel::eta() const { return eta_; }
double HullWhite2FModel::rho() const { return rho_; }
double HullWhite2FModel::r0() const { return r0_; }

GbmModel::GbmModel(const Params& params)
    : s0_(get_double(params, "s0")),
      r_(get_double(params, "r")),
      q_(get_double(params, "q")),
      sigma_(get_double(params, "sigma")),
      observable_(payoff::ObservableId{get_string(params, "observable")}) {}

std::optional<payoff::ModelCapabilities> GbmModel::capabilities() const {
    payoff::ModelCapabilities caps;
    caps.generated_observables = {observable_};
    caps.supported_measures = {payoff::ProbabilityMeasure::RiskNeutralQ};
    // Fase 6: GBM soporta la correccion de Brownian bridge para barreras (§4.2) -- ver
    // rust/crates/engine-core/src/payoff/eval.rs::resolve_trigger_states, que la aplica sobre
    // cualquier patron de barrera simple que compile::compile reconozca.
    caps.supports_continuous_barrier_bridge = true;
    // Fase 9: GBM soporta la regresion de Longstaff-Schwartz para Exercise (§10) -- ver
    // rust/crates/engine-core/src/payoff/lsm.rs::resolve_exercise_decisions.
    caps.supports_early_exercise_regression = true;
    return caps;
}

Params GbmModel::to_params() const {
    return Params{{"s0", s0_}, {"r", r_}, {"q", q_}, {"sigma", sigma_}, {"observable", observable_.value}};
}

double GbmModel::s0() const { return s0_; }
double GbmModel::r() const { return r_; }
double GbmModel::q() const { return q_; }
double GbmModel::sigma() const { return sigma_; }
const payoff::ObservableId& GbmModel::observable() const { return observable_; }

GbmPModel::GbmPModel(const Params& params)
    : s0_(get_double(params, "s0")),
      mu_(get_double(params, "mu")),
      sigma_(get_double(params, "sigma")),
      observable_(payoff::ObservableId{get_string(params, "observable")}) {}

std::optional<payoff::ModelCapabilities> GbmPModel::capabilities() const {
    payoff::ModelCapabilities caps;
    caps.generated_observables = {observable_};
    caps.supported_measures = {payoff::ProbabilityMeasure::PhysicalP};
    // Misma dinamica de difusion que GbmModel (solo cambia el drift), asi que la correccion de
    // Brownian bridge (§4.2) es igualmente aplicable bajo P.
    caps.supports_continuous_barrier_bridge = true;
    return caps;
}

Params GbmPModel::to_params() const {
    return Params{{"s0", s0_}, {"mu", mu_}, {"sigma", sigma_}, {"observable", observable_.value}};
}

double GbmPModel::s0() const { return s0_; }
double GbmPModel::mu() const { return mu_; }
double GbmPModel::sigma() const { return sigma_; }
const payoff::ObservableId& GbmPModel::observable() const { return observable_; }

GbmBasketModel::GbmBasketModel(const Params& params) {
    std::vector<std::string> names = split_comma_separated(get_string(params, "observables"));
    observables_.reserve(names.size());
    for (std::string& name : names) observables_.push_back(payoff::ObservableId{std::move(name)});
    const std::size_t n = observables_.size();
    if (n == 0) {
        throw std::invalid_argument("GbmBasketModel: 'observables' no puede estar vacio");
    }

    s0_ = get_vector(params, "s0");
    r_ = get_vector(params, "r");
    q_ = get_vector(params, "q");
    sigma_ = get_vector(params, "sigma");
    correlation_ = get_vector(params, "correlation");

    if (s0_.size() != n || r_.size() != n || q_.size() != n || sigma_.size() != n) {
        throw std::invalid_argument(
            "GbmBasketModel: 'observables' declara " + std::to_string(n) +
            " activos, pero 's0'/'r'/'q'/'sigma' no tienen todos esa misma longitud (s0=" +
            std::to_string(s0_.size()) + ", r=" + std::to_string(r_.size()) + ", q=" + std::to_string(q_.size()) +
            ", sigma=" + std::to_string(sigma_.size()) + ")"
        );
    }
    if (correlation_.size() != n * n) {
        throw std::invalid_argument(
            "GbmBasketModel: 'correlation' debe tener " + std::to_string(n) + "x" + std::to_string(n) + "=" +
            std::to_string(n * n) + " elementos (aplanada fila a fila), se recibieron " +
            std::to_string(correlation_.size())
        );
    }
}

std::optional<payoff::ModelCapabilities> GbmBasketModel::capabilities() const {
    payoff::ModelCapabilities caps;
    for (const payoff::ObservableId& observable : observables_) {
        caps.generated_observables.insert(observable);
    }
    caps.supported_measures = {payoff::ProbabilityMeasure::RiskNeutralQ};
    // Fuera de alcance de Fase 3 (documentado, no oculto -- PLAN_PRODUCTS.md §16): sin Brownian
    // bridge continuo para barreras ni Longstaff-Schwartz para Exercise sobre un basket todavia
    // -- ver PLAN_IMPROVE_NOTEBOOK.md Fase 3, "limitaciones dejadas fuera a proposito".
    caps.supports_continuous_barrier_bridge = false;
    caps.supports_early_exercise_regression = false;
    return caps;
}

Params GbmBasketModel::to_params() const {
    std::string joined;
    for (std::size_t i = 0; i < observables_.size(); ++i) {
        if (i > 0) joined += ",";
        joined += observables_[i].value;
    }
    return Params{
        {"observables", joined}, {"s0", s0_}, {"r", r_}, {"q", q_}, {"sigma", sigma_}, {"correlation", correlation_}
    };
}

} // namespace engine
