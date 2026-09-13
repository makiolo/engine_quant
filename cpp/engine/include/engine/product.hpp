#pragma once

#include <string>
#include <vector>

#include "engine/params.hpp"

namespace engine {

// Forward decl (PLAN_PRODUCTS.md §9.1, Fase 3): evita que este header, consumido por
// medidas/precios existentes, arrastre toda la jerarquia de engine/payoff/*.
namespace payoff {
struct PayoffProgram;
}

// Interfaz base de todo producto registrable (PLAN.md §5.4).
class IProduct {
public:
    virtual ~IProduct() = default;
    virtual std::string type_name() const = 0;

    // Evolución aditiva (PLAN_PRODUCTS.md §9.1): `PayoffProduct` devuelve su programa; los
    // productos legacy (IrSwapProduct, FakeProduct de test) devuelven `nullptr` hasta ser
    // migrados -- no rompe ningún consumidor existente.
    virtual const payoff::PayoffProgram* payoff_program() const { return nullptr; }

    // Evolución aditiva (PLAN_PRODUCTS.md §5.1, §12 Fase 10: "ExplainVisitor sera una funcion
    // de producto", expuesta ahora en nanobind/C ABI/Excel). Default = `type_name()`: productos
    // legacy sin AST propio (IrSwapProduct, FakeProduct de test) no necesitan implementarlo;
    // `PayoffProduct` lo sobrescribe con el árbol/cashflows legibles de `ExplainVisitor`.
    virtual std::string explain() const { return type_name(); }
};

// IRS vanilla (mismo caso base que `rust/crates/engine-core/src/products/irs.rs`, PLAN.md
// §5.2). Params: "notional" (double, requerido), "start" (double, default 0.0),
// "payment_times" (vector<double>, requerido), "accruals" (vector<double>, requerido, mismo
// largo que payment_times), "fixed_rate" (double, opcional): si está ausente, el swap es "a
// la par" (use_par_rate() == true) y el tipo fijo se calcula en Rust, que necesita r0 del
// modelo — no disponible en esta capa.
//
// DEPRECADO para trades nuevos (PLAN_PRODUCTS.md §9.3/§12 Fase 8, "deprecar, sin borrar aún,
// constructores nominales públicos"): `templates::irs_swap` (payoff/irs_templates.hpp) genera
// el mismo swap como `Contract` AST, verificado numéricamente equivalente para PV/DV01 contra
// esta clase (`test_irs_templates.cpp`, `test_specialization_visitor.cpp`). Esta clase sigue
// siendo la ruta activa para `ExposureProfile`/`UnilateralCVA` bajo Hull-White (sin puente
// AST->Hull-White todavía, ver specialization_visitor.hpp) y para código existente -- no se
// retira ningún constructor ni registro; no usar como base de trades nuevos que solo necesiten
// PV/DV01/cashflows deterministas.
class IrSwapProduct : public IProduct {
public:
    explicit IrSwapProduct(const Params& params);

    std::string type_name() const override { return "IRSwap"; }

    double notional() const;
    double fixed_rate() const;
    bool use_par_rate() const;
    double start() const;
    const std::vector<double>& payment_times() const;
    const std::vector<double>& accruals() const;

private:
    double notional_;
    double fixed_rate_;
    bool use_par_rate_;
    double start_;
    std::vector<double> payment_times_;
    std::vector<double> accruals_;
};

} // namespace engine
