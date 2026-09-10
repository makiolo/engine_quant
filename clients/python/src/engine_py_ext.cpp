#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include "engine/calc.hpp"
#include "engine/calibrator.hpp"
#include "engine/engine.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// Convierte un dict de Python a engine::Params (PLAN.md §5.4): mismo bag de parámetros que
// consumen las factories del registry en C++, expuesto en Python como el dict nativo del
// lenguaje en vez de una clase Params dedicada, para que la API se sienta "de Python"
// (PLAN.md §4: "la API debe sentirse equivalente en Python y en Excel", no idéntica letra a
// letra al C++ subyacente). bool se comprueba antes que double porque en Python bool es
// subtipo de int/float. str (PLAN.md §7.15, para ExecutionContext: "backend"/"precision")
// se comprueba antes que el resto porque nb::cast<double> no la aceptaría.
engine::Params dict_to_params(const nb::dict& params) {
    engine::Params result;
    for (auto item : params) {
        auto key = nb::cast<std::string>(item.first);
        nb::handle value = item.second;
        if (nb::isinstance<nb::bool_>(value)) {
            result.emplace(std::move(key), nb::cast<bool>(value));
        } else if (nb::isinstance<nb::str>(value)) {
            result.emplace(std::move(key), nb::cast<std::string>(value));
        } else if (nb::isinstance<nb::list>(value) || nb::isinstance<nb::tuple>(value)) {
            result.emplace(std::move(key), nb::cast<std::vector<double>>(value));
        } else {
            result.emplace(std::move(key), nb::cast<double>(value));
        }
    }
    return result;
}

// Inversa de dict_to_params (PLAN.md §7.14): CalibrationResult.optimal_params ya viene como
// un engine::Params del lado C++ (mismo tipo que consumen las factories del registry) -- para
// que se sienta "de Python" a la salida igual que a la entrada, se expone como dict nativo en
// vez de una clase Params dedicada.
nb::dict params_to_dict(const engine::Params& params) {
    nb::dict result;
    for (const auto& [key, value] : params) {
        std::visit([&](const auto& v) { result[key.c_str()] = v; }, value);
    }
    return result;
}

// Envuelve engine::Registries + register_builtins (PLAN.md §5.4, §7.6) en un único objeto
// Python: se instancia una vez (register_builtins se ejecuta en el constructor) y expone
// list_*/create_*/calc como métodos, en vez de dejar que el cliente Python tenga que llamar a
// una función de bootstrap suelta.
class Engine {
public:
    Engine() { engine::register_builtins(registries_); }

    std::vector<std::string> list_models() const { return registries_.models.list(); }
    std::vector<std::string> list_products() const { return registries_.products.list(); }
    // Nombres de ENGINE.CALC (PLAN.md §7.15: "PV"/"DV01"/"ExpectedExposure"/"PFE95"/
    // "UnilateralCVA"), no los nombres registrados en Registry<IMeasure> -- ese registro
    // sigue siendo el mecanismo de extensión (PLAN.md §5.4), pero ya no se expone
    // directamente: se consume a través de calc().
    std::vector<std::string> list_measures() const { return engine::calc_measure_names(); }
    std::vector<std::string> list_calibrators() const { return registries_.calibrators.list(); }

    std::unique_ptr<engine::IModel> create_model(const std::string& name, const nb::dict& params) const {
        return registries_.models.create(name, dict_to_params(params));
    }

    std::unique_ptr<engine::IProduct> create_product(const std::string& name, const nb::dict& params) const {
        return registries_.products.create(name, dict_to_params(params));
    }

    std::unique_ptr<engine::ICalibrator> create_calibrator(const std::string& name) const {
        return registries_.calibrators.create(name);
    }

    // Sustituye por completo create_measure/Measure.evaluate (PLAN.md §7.15): calcula un lote
    // de medidas nombradas de una vez, devolviendo un dict {nombre: MeasureResult} en vez de
    // una medida a la vez con un dict de parámetros genérico.
    nb::dict calc(
        const engine::IProduct& product,
        const std::vector<std::string>& measure_names,
        const engine::IModel& model,
        const engine::MarketSnapshot& market,
        const engine::PricingContext& pricing,
        const engine::ExecutionContext& execution
    ) const {
        engine::CalcResult result = engine::calc(registries_, product, measure_names, model, market, pricing, execution);
        nb::dict out;
        for (const auto& entry : result) {
            out[entry.measure_name.c_str()] = entry.result;
        }
        return out;
    }

private:
    engine::Registries registries_;
};

} // namespace

// Fase 0: cadena de humo del pipeline de build (PLAN.md §7.1).
// Fase 3 (PLAN.md §6): binding 1:1 (o casi) con la API pública del registry C++ de Fase 2
// (Registries/register_builtins/Registry<T>::create/engine::calc, PLAN.md §7.6/§7.15), además
// de las funciones de cadena de humo ampliada que ya se exponían desde Fase 0-1 (siguen
// existiendo tal cual, PLAN.md §7.6).
NB_MODULE(engine, m) {
    m.def("ping", &engine::ping);

    m.def(
        "hull_white_zero_coupon_bond",
        &engine::hull_white_zero_coupon_bond,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("r0"),
        nb::arg("t"),
        nb::arg("maturity")
    );

    m.def(
        "hull_white_zero_coupon_bond_delta_r0",
        &engine::hull_white_zero_coupon_bond_delta_r0,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("r0"),
        nb::arg("t"),
        nb::arg("maturity")
    );

    m.def(
        "irs_unilateral_cva_5y",
        &engine::irs_unilateral_cva_5y,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("r0"),
        nb::arg("notional"),
        nb::arg("hazard_rate"),
        nb::arg("recovery_rate"),
        nb::arg("n_paths"),
        nb::arg("seed")
    );

    m.def(
        "is_gpu_backend_available",
        &engine::is_gpu_backend_available,
        "True si este build se compilo con soporte GPU (feature `gpu` de engine-core) -- "
        "usar para decidir si pedir \"gpu\"/\"auto\" en ExecutionContext tiene sentido."
    );

    // --- Registry de modelos/productos (Fase 3, PLAN.md §5.4/§7.6) ---

    nb::class_<engine::IModel>(m, "Model")
        .def_prop_ro("type_name", &engine::IModel::type_name)
        .def("__repr__", [](const engine::IModel& self) { return "<Model '" + self.type_name() + "'>"; });

    nb::class_<engine::IProduct>(m, "Product")
        .def_prop_ro("type_name", &engine::IProduct::type_name)
        .def("__repr__", [](const engine::IProduct& self) { return "<Product '" + self.type_name() + "'>"; });

    nb::class_<engine::MeasureResult>(m, "MeasureResult")
        .def_ro("times", &engine::MeasureResult::times)
        .def_ro("primary", &engine::MeasureResult::primary)
        .def_ro("secondary", &engine::MeasureResult::secondary)
        .def_ro("has_scalar", &engine::MeasureResult::has_scalar)
        .def_ro("scalar", &engine::MeasureResult::scalar)
        .def("__repr__", [](const engine::MeasureResult& self) {
            return "<MeasureResult times=" + std::to_string(self.times.size()) +
                   " has_scalar=" + (self.has_scalar ? std::string("True") : std::string("False")) + ">";
        });

    // --- Market / PricingContext / ExecutionContext (PLAN.md §7.15) ---

    nb::class_<engine::MarketSnapshot>(m, "MarketSnapshot")
        .def(
            nb::init<std::vector<double>, std::vector<double>, double, double>(),
            nb::arg("pillars"),
            nb::arg("zero_rates"),
            nb::arg("hazard_rate") = 0.0,
            nb::arg("recovery_rate") = 0.0,
            "Curva de mercado observada -- o fabricada, ver synthetic_from_hull_white -- en "
            "un instante dado: pillars (anios desde hoy, estrictamente creciente) + "
            "zero_rates (tipos cero de capitalizacion continua, mismo largo). "
            "hazard_rate/recovery_rate (opcionales, default 0.0) son datos de credito que "
            "solo usa la medida 'UnilateralCVA' de Engine.calc."
        )
        .def_prop_ro("pillars", &engine::MarketSnapshot::pillars)
        .def_prop_ro("zero_rates", &engine::MarketSnapshot::zero_rates)
        .def_prop_ro("hazard_rate", &engine::MarketSnapshot::hazard_rate)
        .def_prop_ro("recovery_rate", &engine::MarketSnapshot::recovery_rate)
        .def("zero_rate", &engine::MarketSnapshot::zero_rate, nb::arg("t"))
        .def("discount_factor", &engine::MarketSnapshot::discount_factor, nb::arg("t"))
        .def_static(
            "synthetic_from_hull_white",
            &engine::MarketSnapshot::synthetic_from_hull_white,
            nb::arg("a"),
            nb::arg("b"),
            nb::arg("sigma"),
            nb::arg("r0"),
            nb::arg("pillars"),
            nb::arg("hazard_rate") = 0.0,
            nb::arg("recovery_rate") = 0.0,
            "Mercado 'falso': fabrica un MarketSnapshot leyendo la propia formula cerrada de "
            "HullWhite1F en los pillars dados -- util para probar/demostrar calibrate() sin "
            "depender de datos de mercado reales."
        )
        .def("__repr__", [](const engine::MarketSnapshot& self) {
            return "<MarketSnapshot pillars=" + std::to_string(self.pillars().size()) + ">";
        });

    nb::class_<engine::PricingContext>(m, "PricingContext")
        .def(
            "__init__",
            [](engine::PricingContext* self, const nb::dict& params) {
                new (self) engine::PricingContext(dict_to_params(params));
            },
            nb::arg("params"),
            "Contexto de valoracion de Engine.calc (PLAN.md §7.15): dict con 'pricing_date' "
            "(opcional, default 0.0 -- metadato, sin aritmetica de calendario todavia), "
            "'n_paths', 'n_steps' y 'seed' (todos requeridos)."
        )
        .def_prop_ro("pricing_date", &engine::PricingContext::pricing_date)
        .def_prop_ro("n_paths", &engine::PricingContext::n_paths)
        .def_prop_ro("n_steps", &engine::PricingContext::n_steps)
        .def_prop_ro("seed", &engine::PricingContext::seed)
        .def("__repr__", [](const engine::PricingContext& self) {
            return "<PricingContext n_paths=" + std::to_string(self.n_paths()) +
                   " n_steps=" + std::to_string(self.n_steps()) + ">";
        });

    nb::class_<engine::ExecutionContext>(m, "ExecutionContext")
        .def(
            "__init__",
            [](engine::ExecutionContext* self, const nb::dict& params) {
                new (self) engine::ExecutionContext(dict_to_params(params));
            },
            nb::arg("params"),
            "Como ejecutar Engine.calc (PLAN.md §7.15, sustituye el backend global de la Fase "
            "5/§7.12): dict con 'backend' ('cpu'/'gpu'/'auto', resuelto aqui mismo) y "
            "'precision' (opcional, default 'fp64' -- unico valor soportado hoy)."
        )
        .def_prop_ro("backend", &engine::ExecutionContext::backend)
        .def_prop_ro("precision", &engine::ExecutionContext::precision)
        .def("__repr__", [](const engine::ExecutionContext& self) {
            return "<ExecutionContext backend='" + self.backend() + "'>";
        });

    // --- Calibración (PLAN.md §7.14) ---

    nb::class_<engine::CalibrationResult>(m, "CalibrationResult")
        .def_prop_ro("optimal_params", [](const engine::CalibrationResult& self) { return params_to_dict(self.optimal_params); })
        .def_ro("rmse", &engine::CalibrationResult::rmse)
        .def_ro("iterations", &engine::CalibrationResult::iterations)
        .def_ro("converged", &engine::CalibrationResult::converged)
        .def("__repr__", [](const engine::CalibrationResult& self) {
            return "<CalibrationResult rmse=" + std::to_string(self.rmse) +
                   " converged=" + (self.converged ? std::string("True") : std::string("False")) + ">";
        });

    nb::class_<engine::ICalibrator>(m, "Calibrator")
        .def_prop_ro("type_name", &engine::ICalibrator::type_name)
        .def(
            "calibrate",
            [](const engine::ICalibrator& self, const engine::MarketSnapshot& market, const nb::dict& initial_guess) {
                return self.calibrate(market, dict_to_params(initial_guess));
            },
            nb::arg("market"),
            nb::arg("initial_guess"),
            "Calibra este modelo a `market` partiendo de `initial_guess` (dict, mismas claves "
            "que create_model para el mismo tipo de modelo). Devuelve un CalibrationResult "
            "cuyo optimal_params se puede pasar directamente a Engine.create_model."
        )
        .def("__repr__", [](const engine::ICalibrator& self) { return "<Calibrator '" + self.type_name() + "'>"; });

    nb::class_<Engine>(m, "Engine")
        .def(nb::init<>())
        .def("list_models", &Engine::list_models)
        .def("list_products", &Engine::list_products)
        .def("list_measures", &Engine::list_measures)
        .def("list_calibrators", &Engine::list_calibrators)
        .def("create_model", &Engine::create_model, nb::arg("name"), nb::arg("params") = nb::dict())
        .def("create_product", &Engine::create_product, nb::arg("name"), nb::arg("params") = nb::dict())
        .def("create_calibrator", &Engine::create_calibrator, nb::arg("name"))
        .def(
            "calc",
            &Engine::calc,
            nb::arg("product"),
            nb::arg("measure_names"),
            nb::arg("model"),
            nb::arg("market"),
            nb::arg("pricing"),
            nb::arg("execution"),
            "Calcula un lote de medidas nombradas (ver list_measures()) sobre el mismo "
            "product/model/market/pricing/execution de una vez. Devuelve un dict {nombre: "
            "MeasureResult} en el mismo orden que measure_names.\n\n"
            ">>> eng.calc(trade, ['PV', 'DV01', 'ExpectedExposure', 'PFE95', 'UnilateralCVA'],\n"
            "...          model, market, pricing, execution)"
        );
}
