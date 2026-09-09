#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include "engine/bootstrap.hpp"
#include "engine/engine.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// Convierte un dict de Python a engine::Params (PLAN.md §5.4): mismo bag de parámetros que
// consumen las factories del registry en C++, expuesto en Python como el dict nativo del
// lenguaje en vez de una clase Params dedicada, para que la API se sienta "de Python"
// (PLAN.md §4: "la API debe sentirse equivalente en Python y en Excel", no idéntica letra a
// letra al C++ subyacente). bool se comprueba antes que double porque en Python bool es
// subtipo de int/float.
engine::Params dict_to_params(const nb::dict& params) {
    engine::Params result;
    for (auto item : params) {
        auto key = nb::cast<std::string>(item.first);
        nb::handle value = item.second;
        if (nb::isinstance<nb::bool_>(value)) {
            result.emplace(std::move(key), nb::cast<bool>(value));
        } else if (nb::isinstance<nb::list>(value) || nb::isinstance<nb::tuple>(value)) {
            result.emplace(std::move(key), nb::cast<std::vector<double>>(value));
        } else {
            result.emplace(std::move(key), nb::cast<double>(value));
        }
    }
    return result;
}

// Envuelve engine::Registries + register_builtins (PLAN.md §5.4, §7.6) en un único objeto
// Python: se instancia una vez (register_builtins se ejecuta en el constructor) y expone
// list_*/create_* como métodos, en vez de dejar que el cliente Python tenga que llamar a una
// función de bootstrap suelta.
class Engine {
public:
    Engine() { engine::register_builtins(registries_); }

    std::vector<std::string> list_models() const { return registries_.models.list(); }
    std::vector<std::string> list_products() const { return registries_.products.list(); }
    std::vector<std::string> list_measures() const { return registries_.measures.list(); }

    std::unique_ptr<engine::IModel> create_model(const std::string& name, const nb::dict& params) const {
        return registries_.models.create(name, dict_to_params(params));
    }

    std::unique_ptr<engine::IProduct> create_product(const std::string& name, const nb::dict& params) const {
        return registries_.products.create(name, dict_to_params(params));
    }

    std::unique_ptr<engine::IMeasure> create_measure(const std::string& name) const {
        return registries_.measures.create(name);
    }

private:
    engine::Registries registries_;
};

} // namespace

// Fase 0: cadena de humo del pipeline de build (PLAN.md §7.1).
// Fase 3 (PLAN.md §6): binding 1:1 (o casi) con la API pública del registry C++ de Fase 2
// (Registries/register_builtins/Registry<T>::create/IMeasure::evaluate, PLAN.md §7.6), además
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

    // --- Registry de modelos/productos/medidas (Fase 3, PLAN.md §5.4/§7.6) ---

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

    nb::class_<engine::IMeasure>(m, "Measure")
        .def_prop_ro("type_name", &engine::IMeasure::type_name)
        .def(
            "evaluate",
            [](const engine::IMeasure& self, const engine::IModel& model, const engine::IProduct& product, const nb::dict& params) {
                return self.evaluate(model, product, dict_to_params(params));
            },
            nb::arg("model"),
            nb::arg("product"),
            nb::arg("params") = nb::dict()
        )
        .def("__repr__", [](const engine::IMeasure& self) { return "<Measure '" + self.type_name() + "'>"; });

    nb::class_<Engine>(m, "Engine")
        .def(nb::init<>())
        .def("list_models", &Engine::list_models)
        .def("list_products", &Engine::list_products)
        .def("list_measures", &Engine::list_measures)
        .def("create_model", &Engine::create_model, nb::arg("name"), nb::arg("params") = nb::dict())
        .def("create_product", &Engine::create_product, nb::arg("name"), nb::arg("params") = nb::dict())
        .def("create_measure", &Engine::create_measure, nb::arg("name"));
}
