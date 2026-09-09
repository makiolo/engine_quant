"""Cadena de humo del pipeline de build (PLAN.md §7.1): valida que Rust -> cxx -> C++ ->
nanobind -> Python funciona de punta a punta, primero con `ping()` (Fase 0) y luego con
funciones que ejercitan Hull-White 1F + IRS + exposición/CVA + AAD ya sobre Burn (PLAN.md
§5.1, §5.3), reutilizando la misma cadena. No son la API definitiva del motor — eso es el
registry de Fase 2 (§5.4) y los bindings 1:1 de Fase 3 (§6) — solo confirman que el
pipeline de build sigue funcionando end-to-end con lógica de negocio real detrás.
"""

import math
import sys
from pathlib import Path

# El módulo compilado (engine.pyd) vive en el directorio de build de CMake;
# la localización definitiva del artefacto se resuelve en Fase 3 (empaquetado del cliente Python).
if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def test_ping():
    assert engine.ping() == 42.0


def _vasicek_zero_coupon_bond(a, b, sigma, r0, t, maturity):
    """Réplica en Python puro de la fórmula cerrada de Hull-White 1F con reversión
    constante (PLAN.md §5.2, equivalente a Vasicek), usada como referencia independiente
    de la implementación Rust/Burn para el smoke test — no reemplaza los tests numéricos
    de Rust (PLAN.md §5.6), solo confirma que el valor cruza las cuatro capas sin corromperse.
    """
    tau = maturity - t
    b_t_t = (1.0 - math.exp(-a * tau)) / a
    a_t_t = math.exp(
        (b_t_t - tau) * (a * a * b - sigma * sigma / 2.0) / (a * a) - sigma * sigma * b_t_t**2 / (4.0 * a)
    )
    return a_t_t * math.exp(-b_t_t * r0)


def test_hull_white_zero_coupon_bond_matches_closed_form_reference():
    a, b, sigma, r0, t, maturity = 0.1, 0.03, 0.01, 0.02, 0.0, 5.0
    price = engine.hull_white_zero_coupon_bond(a, b, sigma, r0, t, maturity)
    expected = _vasicek_zero_coupon_bond(a, b, sigma, r0, t, maturity)
    assert math.isclose(price, expected, rel_tol=1e-9), f"price={price} expected={expected}"
    assert 0.0 < price < 1.0


def test_hull_white_zero_coupon_bond_delta_r0_is_negative_and_consistent():
    a, b, sigma, r0, t, maturity = 0.1, 0.03, 0.01, 0.02, 0.0, 5.0
    delta = engine.hull_white_zero_coupon_bond_delta_r0(a, b, sigma, r0, t, maturity)
    # Subir el tipo corto baja el precio del bono: dP/dr0 < 0.
    assert delta < 0.0

    # Contraste contra diferencias finitas centrales (bump-and-reval, PLAN.md §5.6 capa 3),
    # calculado con la misma referencia cerrada de arriba, no con el motor.
    h = 1e-6
    bump = (
        _vasicek_zero_coupon_bond(a, b, sigma, r0 + h, t, maturity)
        - _vasicek_zero_coupon_bond(a, b, sigma, r0 - h, t, maturity)
    ) / (2 * h)
    assert math.isclose(delta, bump, abs_tol=1e-6), f"delta={delta} bump-and-reval={bump}"


def test_irs_unilateral_cva_5y_is_nonnegative_and_sane():
    notional = 1_000_000.0
    cva = engine.irs_unilateral_cva_5y(
        a=0.1,
        b=0.03,
        sigma=0.01,
        r0=0.02,
        notional=notional,
        hazard_rate=0.02,
        recovery_rate=0.4,
        n_paths=5_000,
        seed=42,
    )
    assert cva >= 0.0
    # Cota floja de cordura: el CVA de un IRS a 5 años con hazard rate del 2% no debería
    # acercarse siquiera al nocional (evita solo errores groseros, no reemplaza los tests
    # de Rust que sí acotan por error estándar del estimador, PLAN.md §5.6 capa 2).
    assert cva < notional * 0.1, f"CVA={cva} sospechosamente alto"


if __name__ == "__main__":
    test_ping()
    test_hull_white_zero_coupon_bond_matches_closed_form_reference()
    test_hull_white_zero_coupon_bond_delta_r0_is_negative_and_consistent()
    test_irs_unilateral_cva_5y_is_nonnegative_and_sane()
    print("OK: smoke tests (ping, Hull-White, delta AAD, CVA IRS) pasaron")
