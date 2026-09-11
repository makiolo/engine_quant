#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include "engine/price.hpp"
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

// Traduce un elemento de la lista `measures` de Engine.price/price_batch/price_many/price_grid a
// un engine::MeasureSpec (PLAN_REAPI.md §6 Fase 3): un string "pelado" (measure_names de
// siempre, PLAN.md §7.15/§7.19) es MeasureSpec{name, {}}; una tupla (nombre, dict) es
// MeasureSpec{name, dict_to_params(dict)} -- así engine_typed.DV01(bump=...).to_spec() (una
// tupla (str, dict)) y el ["PV", "DV01"] de siempre conviven en la misma llamada.
engine::MeasureSpec to_measure_spec(nb::handle item) {
    if (nb::isinstance<nb::str>(item)) {
        return engine::MeasureSpec{nb::cast<std::string>(item), engine::Params{}};
    }
    if (nb::isinstance<nb::tuple>(item)) {
        nb::tuple spec = nb::borrow<nb::tuple>(item);
        if (spec.size() != 2) {
            throw std::invalid_argument("Engine.price: una medida-tupla debe ser (nombre, params)");
        }
        std::string name = nb::cast<std::string>(spec[0]);
        nb::dict params = nb::cast<nb::dict>(spec[1]);
        return engine::MeasureSpec{std::move(name), dict_to_params(params)};
    }
    throw std::invalid_argument("Engine.price: cada medida debe ser un string o una tupla (nombre, params)");
}

std::vector<engine::MeasureSpec> to_measure_specs(const nb::list& measures) {
    std::vector<engine::MeasureSpec> result;
    result.reserve(measures.size());
    for (nb::handle item : measures) {
        result.push_back(to_measure_spec(item));
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
// list_*/create_*/price como métodos, en vez de dejar que el cliente Python tenga que llamar a
// una función de bootstrap suelta.
class Engine {
public:
    Engine() { engine::register_builtins(registries_); }

    std::vector<std::string> list_models() const { return registries_.models.list(); }
    std::vector<std::string> list_products() const { return registries_.products.list(); }
    // Nombres de ENGINE.PRICE (PLAN.md §7.15: "PV"/"DV01"/"ExpectedExposure"/"PFE95"/
    // "UnilateralCVA"), no los nombres registrados en Registry<IMeasure> -- ese registro
    // sigue siendo el mecanismo de extensión (PLAN.md §5.4), pero ya no se expone
    // directamente: se consume a través de price().
    std::vector<std::string> list_measures() const { return engine::price_measure_names(registries_); }
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
    // de medidas de una vez, devolviendo un dict {nombre: MeasureResult} en vez de una medida
    // a la vez con un dict de parámetros genérico. Cada elemento de `measures` es un string
    // "pelado" o una tupla (nombre, params) -- PLAN_REAPI.md §6 Fase 3, ver to_measure_spec.
    nb::dict price(
        const engine::IProduct& product,
        const nb::list& measures,
        const engine::IModel& model,
        const engine::MarketSnapshot& market,
        const engine::PricingContext& pricing,
        const engine::ExecutionContext& execution
    ) const {
        engine::PriceResult result =
            engine::price(registries_, product, to_measure_specs(measures), model, market, pricing, execution);
        nb::dict out;
        for (const auto& entry : result) {
            out[entry.measure_name.c_str()] = entry.result;
        }
        return out;
    }

    // Nivel 3, lote homogéneo (PLAN.md §7.17/§7.19): `products` debe ser del mismo tipo
    // registrado y, para IRSwap, compartir calendario y traer fixed_rate explícito (sin
    // use_par_rate) -- ver engine::price_batch. Devuelve list[BatchResult], no una lista
    // anidada: cada fila lleva su trade_index explícito, misma filosofía en las cinco capas.
    std::vector<engine::PriceBatchResultEntry> price_batch(
        const std::vector<const engine::IProduct*>& products,
        const nb::list& measures,
        const engine::IModel& model,
        const engine::MarketSnapshot& market,
        const engine::PricingContext& pricing,
        const engine::ExecutionContext& execution
    ) const {
        return engine::price_batch(registries_, products, to_measure_specs(measures), model, market, pricing, execution);
    }

    // Nivel 2, lista heterogénea (PLAN.md §7.17/§7.19): misma forma de resultado que
    // price_batch, pero products puede mezclar tipos/calendarios distintos -- se agrupan
    // internamente y nunca falla por heterogeneidad (ver engine::price_many).
    std::vector<engine::PriceBatchResultEntry> price_many(
        const std::vector<const engine::IProduct*>& products,
        const nb::list& measures,
        const engine::IModel& model,
        const engine::MarketSnapshot& market,
        const engine::PricingContext& pricing,
        const engine::ExecutionContext& execution
    ) const {
        return engine::price_many(registries_, products, to_measure_specs(measures), model, market, pricing, execution);
    }

    // Explosión de combinaciones Trades x Models x Markets (PLAN.md §7.19): pricing/execution
    // son compartidos, no forman parte de la rejilla -- ver engine::price_grid.
    std::vector<engine::PriceGridResultEntry> price_grid(
        const std::vector<const engine::IProduct*>& products,
        const nb::list& measures,
        const std::vector<const engine::IModel*>& models,
        const std::vector<engine::MarketSnapshot>& markets,
        const engine::PricingContext& pricing,
        const engine::ExecutionContext& execution
    ) const {
        return engine::price_grid(registries_, products, to_measure_specs(measures), models, markets, pricing, execution);
    }

private:
    engine::Registries registries_;
};

// Traduce un engine::PriceResult (usado dentro de BatchResult/GridResult) a un dict
// {nombre_medida: MeasureResult} -- misma forma que ya devuelve Engine.price, extraída aquí
// para no duplicarla entre BatchResult y GridResult (PLAN.md §7.19).
nb::dict calc_result_to_dict(const engine::PriceResult& result) {
    nb::dict out;
    for (const auto& entry : result) {
        out[entry.measure_name.c_str()] = entry.result;
    }
    return out;
}

} // namespace

// Fase 0: cadena de humo del pipeline de build (PLAN.md §7.1).
// Fase 3 (PLAN.md §6): binding 1:1 (o casi) con la API pública del registry C++ de Fase 2
// (Registries/register_builtins/Registry<T>::create/engine::price, PLAN.md §7.6/§7.15), además
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
        "hull_white_2f_zero_coupon_bond",
        &engine::hull_white_2f_zero_coupon_bond,
        nb::arg("a"),
        nb::arg("b"),
        nb::arg("sigma"),
        nb::arg("eta"),
        nb::arg("rho"),
        nb::arg("r0"),
        nb::arg("maturity"),
        "Equivalente de dos factores de hull_white_zero_coupon_bond (PLAN.md §7.18): precio "
        "del bono cero-cupon de HullWhite2F/G2++ con los dos factores latentes en su valor "
        "inicial (x_0 = y_0 = 0)."
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

    // --- price_batch / price_many / price_grid (PLAN.md §7.17/§7.19) ---
    // Cada fila lleva su(s) índice(s) explícito(s) -- nunca una lista anidada -- misma
    // filosofía en las cinco capas (C++/C ABI/Python/Excel).

    nb::class_<engine::PriceBatchResultEntry>(m, "BatchResult")
        .def_ro("trade_index", &engine::PriceBatchResultEntry::trade_index)
        .def_prop_ro(
            "measures", [](const engine::PriceBatchResultEntry& self) { return calc_result_to_dict(self.measures); }
        )
        .def("__repr__", [](const engine::PriceBatchResultEntry& self) {
            return "<BatchResult trade_index=" + std::to_string(self.trade_index) + ">";
        });

    nb::class_<engine::PriceGridResultEntry>(m, "GridResult")
        .def_ro("trade_index", &engine::PriceGridResultEntry::trade_index)
        .def_ro("model_index", &engine::PriceGridResultEntry::model_index)
        .def_ro("market_index", &engine::PriceGridResultEntry::market_index)
        .def_prop_ro(
            "measures", [](const engine::PriceGridResultEntry& self) { return calc_result_to_dict(self.measures); }
        )
        .def("__repr__", [](const engine::PriceGridResultEntry& self) {
            return "<GridResult trade_index=" + std::to_string(self.trade_index) +
                   " model_index=" + std::to_string(self.model_index) +
                   " market_index=" + std::to_string(self.market_index) + ">";
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
            "solo usa la medida 'UnilateralCVA' de Engine.price."
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
        .def_static(
            "synthetic_from_hull_white_2f",
            &engine::MarketSnapshot::synthetic_from_hull_white_2f,
            nb::arg("a"),
            nb::arg("b"),
            nb::arg("sigma"),
            nb::arg("eta"),
            nb::arg("rho"),
            nb::arg("r0"),
            nb::arg("pillars"),
            nb::arg("hazard_rate") = 0.0,
            nb::arg("recovery_rate") = 0.0,
            "Equivalente de dos factores de synthetic_from_hull_white (PLAN.md §7.18): "
            "fabrica un MarketSnapshot leyendo la propia formula cerrada de HullWhite2F/G2++ "
            "en los pillars dados."
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
            "Contexto de valoracion de Engine.price (PLAN.md §7.15): dict con 'pricing_date' "
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
            "Como ejecutar Engine.price (PLAN.md §7.15, sustituye el backend global de la Fase "
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
            "price",
            &Engine::price,
            nb::arg("product"),
            nb::arg("measure_names"),
            nb::arg("model"),
            nb::arg("market"),
            nb::arg("pricing"),
            nb::arg("execution"),
            "Calcula un lote de medidas nombradas (ver list_measures()) sobre el mismo "
            "product/model/market/pricing/execution de una vez. Devuelve un dict {nombre: "
            "MeasureResult} en el mismo orden que measure_names.\n\n"
            ">>> eng.price(trade, ['PV', 'DV01', 'ExpectedExposure', 'PFE95', 'UnilateralCVA'],\n"
            "...          model, market, pricing, execution)"
        )
        .def(
            "price_batch",
            &Engine::price_batch,
            nb::arg("products"),
            nb::arg("measure_names"),
            nb::arg("model"),
            nb::arg("market"),
            nb::arg("pricing"),
            nb::arg("execution"),
            "Nivel 3 (PLAN.md §7.17/§7.19): calcula measure_names para una LISTA de trades del "
            "mismo tipo/calendario, vectorizado sin bucle -- cada trade de IRSwap debe traer "
            "fixed_rate explicito (sin use_par_rate). Devuelve list[BatchResult], una fila por "
            "trade en el mismo orden que products."
        )
        .def(
            "price_many",
            &Engine::price_many,
            nb::arg("products"),
            nb::arg("measure_names"),
            nb::arg("model"),
            nb::arg("market"),
            nb::arg("pricing"),
            nb::arg("execution"),
            "Nivel 2 (PLAN.md §7.17/§7.19): igual que price_batch pero products puede mezclar "
            "tipos/calendarios distintos -- se agrupan internamente (nunca falla por "
            "heterogeneidad) y el resultado se devuelve en el orden de entrada original."
        )
        .def(
            "price_grid",
            &Engine::price_grid,
            nb::arg("products"),
            nb::arg("measure_names"),
            nb::arg("models"),
            nb::arg("markets"),
            nb::arg("pricing"),
            nb::arg("execution"),
            "Explosion de combinaciones Trades x Models x Markets (PLAN.md §7.19): por cada "
            "par (modelo, mercado), llama a price_many sobre products entero. pricing/execution "
            "son compartidos, no forman parte de la rejilla. Devuelve list[GridResult]."
        );
}
