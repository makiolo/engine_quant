#pragma once

#include <string>

#include "engine/params.hpp"
#include "engine/product.hpp"

#include "engine/payoff/contract.hpp"

namespace engine {
namespace payoff {

// Programa de payoff resuelto: árbol validado + hash canónico (PLAN_PRODUCTS.md §9.1). Nunca
// se modifica tras construirse -- si se necesita otro contrato, se construye otro
// `PayoffProduct`.
struct PayoffProgram {
    std::string id;
    ContractPtr contract;
    std::string canonical_hash;
};

// Producto universal del registry (PLAN_PRODUCTS.md §7.2, §9.1, factory `"Payoff"`). Envuelve
// cualquier `Contract` ya validado; las medidas que sepan interpretar `payoff_program()`
// (Fase 4+) no necesitan una clase nominal nueva por producto.
class PayoffProduct : public IProduct {
public:
    // Autoría directa en C++ (§7.1): `PayoffProduct("AAPL_CALL_100", call)`.
    PayoffProduct(std::string id, ContractPtr contract);

    // Vía registry (§7.2): `registries.products.create("Payoff", {{"spec", canonical_json}})`.
    // Lee `"spec"` con `engine::get_string` (lanza si falta/no es string) y lo parsea.
    explicit PayoffProduct(const Params& params);

    std::string type_name() const override { return "Payoff"; }
    const PayoffProgram* payoff_program() const override { return &program_; }

    // Árbol y cabecera (id/hash) en texto legible (§5.1 "ExplainVisitor será una función de
    // producto"), reutilizando `ExplainVisitor` de Fases 1-2.
    std::string explain() const;

private:
    static PayoffProgram build_program(std::string id, ContractPtr contract);

    PayoffProgram program_;
};

} // namespace payoff
} // namespace engine
