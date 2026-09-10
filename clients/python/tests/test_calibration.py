"""Tests del binding Python de MarketSnapshot/Calibrator (PLAN.md §7.14): equivalente en
Python de cpp/engine/tests/test_calibration.cpp -- confirma que el binding nanobind expone la
misma API que el registry C++ (MarketSnapshot, Registry<ICalibrator>, ICalibrator.calibrate)
sin reimplementar su lógica. No repite la validación numérica fina del optimizador (eso ya lo
cubre rust/crates/engine-core/src/calibration.rs).
"""

import sys

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])

import engine  # noqa: E402


def test_register_builtins_populates_calibrator_registry():
    eng = engine.Engine()
    assert "HullWhite1F" in eng.list_calibrators()


def test_market_snapshot_zero_rate_interpolates_linearly():
    market = engine.MarketSnapshot([1.0, 2.0, 5.0], [0.02, 0.03, 0.04])
    assert abs(market.zero_rate(1.0) - 0.02) < 1e-12
    assert abs(market.zero_rate(2.0) - 0.03) < 1e-12
    assert abs(market.zero_rate(3.5) - 0.035) < 1e-12


def test_market_snapshot_rejects_non_increasing_pillars():
    try:
        engine.MarketSnapshot([1.0, 1.0], [0.02, 0.03])
        assert False, "se esperaba ValueError"
    except ValueError:
        pass


def test_synthetic_from_hull_white_reproduces_the_models_own_prices():
    a, b, sigma, r0 = 0.1, 0.03, 0.01, 0.02
    pillars = [1.0, 2.0, 5.0, 10.0]
    market = engine.MarketSnapshot.synthetic_from_hull_white(a, b, sigma, r0, pillars)

    for t in pillars:
        expected = engine.hull_white_zero_coupon_bond(a, b, sigma, r0, 0.0, t)
        assert abs(market.discount_factor(t) - expected) < 1e-9


def test_calibrator_recovers_known_parameters_and_feeds_create_model():
    eng = engine.Engine()
    true_a, true_b, sigma, r0 = 0.15, 0.025, 0.008, 0.02
    pillars = [0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0]
    market = engine.MarketSnapshot.synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars)

    calibrator = eng.create_calibrator("HullWhite1F")
    # Estimación inicial deliberadamente lejos de los parámetros "verdaderos".
    result = calibrator.calibrate(market, {"a": 0.3, "b": 0.01, "sigma": sigma, "r0": r0})

    assert result.converged, f"no convergió: rmse={result.rmse} iterations={result.iterations}"
    assert abs(result.optimal_params["a"] - true_a) < 1e-4
    assert abs(result.optimal_params["b"] - true_b) < 1e-4
    assert result.optimal_params["sigma"] == sigma
    assert result.optimal_params["r0"] == r0

    # El resultado debe poder alimentar directamente create_model -- cerrar el círculo
    # Market -> calibrar -> Model calibrado.
    calibrated_model = eng.create_model("HullWhite1F", result.optimal_params)
    assert calibrated_model.type_name == "HullWhite1F"


def test_create_unknown_calibrator_raises_index_error():
    eng = engine.Engine()
    try:
        eng.create_calibrator("NoExiste")
        assert False, "se esperaba IndexError"
    except IndexError:
        pass


if __name__ == "__main__":
    test_register_builtins_populates_calibrator_registry()
    test_market_snapshot_zero_rate_interpolates_linearly()
    test_market_snapshot_rejects_non_increasing_pillars()
    test_synthetic_from_hull_white_reproduces_the_models_own_prices()
    test_calibrator_recovers_known_parameters_and_feeds_create_model()
    test_create_unknown_calibrator_raises_index_error()
    print("OK: tests de Market/Calibrator (equivalente a test_calibration.cpp) pasaron")
