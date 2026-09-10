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

// Gestor de contexto nativo para PLAN.md §7.12: "with engine.backend('gpu'): ...". Inspirado
// en `decimal.localcontext()`/`torch.device()` (Python estándar/PyTorch) — recordar el
// backend previo al entrar y restaurarlo al salir, incluso si el bloque lanza una excepción
// — pero implementado como clase de nanobind en vez de un decorador `@contextlib.
// contextmanager` de Python: este binding es el único módulo que se distribuye (PLAN.md
// §7.9, `pyproject.toml`: "no hay paquete Python que auto-copiar"), así que el protocolo de
// gestor de contexto (`__enter__`/`__exit__`, duck-typed por Python, contextlib es solo un
// azúcar sintáctico para construirlo con un generador) vive aquí en vez de en un fichero .py
// nuevo. `clients/python/examples/backend_selection.py` muestra el equivalente con
// contextlib para quien prefiera construir el suyo por encima de `set_compute_backend`/
// `get_compute_backend`.
class BackendScope {
public:
    explicit BackendScope(std::string requested) : requested_(std::move(requested)) {}

    std::string enter() {
        previous_ = engine::compute_backend_name();
        if (!engine::set_compute_backend(requested_)) {
            throw std::invalid_argument(
                "Backend de computo no disponible: '" + requested_ +
                "' (gpu disponible en este build: " +
                (engine::is_gpu_backend_available() ? "si" : "no") + ")");
        }
        return engine::compute_backend_name();
    }

    bool exit(nb::object, nb::object, nb::object) {
        engine::set_compute_backend(previous_);
        return false; // no suprime la excepcion, si la hubo
    }

private:
    std::string requested_;
    std::string previous_;
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

    // --- Selección de backend de cómputo (PLAN.md §7.12) ---

    m.def(
        "set_compute_backend",
        &engine::set_compute_backend,
        nb::arg("name"),
        "Selecciona el backend de computo global ('cpu'/'gpu'). Devuelve False sin cambiar "
        "nada si el nombre no se reconoce o pide un backend no compilado en este build "
        "(ver is_gpu_backend_available()). Afecta a todas las llamadas siguientes hasta el "
        "proximo set_compute_backend() -- para un cambio acotado a un bloque, usar backend()."
    );
    m.def(
        "get_compute_backend",
        &engine::compute_backend_name,
        "Backend de computo actualmente seleccionado ('cpu' o 'gpu')."
    );
    m.def(
        "is_gpu_backend_available",
        &engine::is_gpu_backend_available,
        "True si este build se compilo con soporte GPU (feature `gpu` de engine-core), "
        "independientemente de cual sea el backend seleccionado ahora mismo."
    );

    nb::class_<BackendScope>(m, "_BackendScope")
        .def(nb::init<std::string>())
        .def("__enter__", &BackendScope::enter)
        // Python invoca __exit__(None, None, None) cuando el bloque `with` termina sin
        // excepcion: .none() en cada argumento es obligatorio en nanobind (por defecto
        // rechaza None incluso para un nb::object generico, ver nb_attr.h) o cada salida
        // limpia del `with` lanzaria un TypeError en vez de restaurar el backend.
        .def(
            "__exit__",
            &BackendScope::exit,
            nb::arg("exc_type").none(),
            nb::arg("exc_value").none(),
            nb::arg("traceback").none()
        );

    m.def(
        "backend",
        [](const std::string& name) { return BackendScope(name); },
        nb::arg("name"),
        "Gestor de contexto: selecciona el backend de computo ('cpu'/'gpu') solo dentro del "
        "bloque `with`, restaurando el anterior al salir (incluso si el bloque lanza una "
        "excepcion) -- inspirado en decimal.localcontext()/torch.device().\n\n"
        ">>> with engine.backend('gpu'):\n"
        "...     medida.evaluate(modelo, producto, params)  # corre en GPU\n"
        "... # aqui ya se ha restaurado el backend anterior"
    );
}
