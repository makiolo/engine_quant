#pragma once

#include <string>
#include <vector>

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
    std::string explain() const override;

private:
    static PayoffProgram build_program(std::string id, ContractPtr contract);

    PayoffProgram program_;
};

// --- Autoría/validación sin registry (PLAN_PRODUCTS.md Fase 10, §7.1) ----------------------
// Superficie usada por nanobind/C ABI/Excel para validar o explicar un spec `engine.payoff/v1`
// sin pasar por `Registry<IProduct>::create` (que exige un `Params` con la clave "spec" ya
// dentro de un `nb::dict`/`EngineParam`/rango Excel -- aquí basta el JSON crudo).

// Valida un documento `engine.payoff/v1` sin construir el producto. JSON sintácticamente
// inválido o `schema` ausente/desconocido se reporta como un único mensaje (`ParseError` se
// detiene en el primer problema, ver json_parser.hpp); un documento parseable pero con árbol
// inválido reporta TODOS los mensajes de `ValidationVisitor` (§7.1, ADR-P0-05). Vector vacío =
// spec válido.
std::vector<std::string> validate_payoff_spec(const std::string& spec_json);

// Construye el producto (valida internamente, ver `build_program`) y devuelve `explain()`.
// Lanza `std::invalid_argument` con el mismo mensaje agregado que el constructor si el spec es
// inválido, o la excepción de `parse_payoff_document` si ni siquiera parsea.
std::string explain_payoff_spec(const std::string& spec_json);

} // namespace payoff
} // namespace engine
