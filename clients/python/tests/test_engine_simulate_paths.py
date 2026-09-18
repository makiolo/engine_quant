"""Tests de `Engine.simulate_paths` (PLAN_IMPROVE_NOTEBOOK.md Fase 0: "Exponer diagnostico de
trayectorias Monte Carlo"). No es una medida de `Engine.price` -- es una herramienta de
notebook/diagnostico que expone la matriz completa de trayectorias que el simulador ya calcula
por dentro para las medidas `Payoff*Q`/`Payoff*P`, en vez de solo el agregado final que esas
medidas consumen (ver `clients/python/notebooks/07_montecarlo_paths_q_vs_p.ipynb`).

Mismo criterio de tolerancia que `test_market_products_realistic.py`/los tests de paridad de
`rust/crates/engine-core/src/api.rs::tests::simulate_paths_gbm_q_terminal_moments_match_
lognormal_gbm_formula`: los momentos (media/varianza) de la matriz simulada en `t=T` deben
coincidir con la formula analitica del GBM lognormal dentro de un margen generoso de error
estandar Monte Carlo -- no golden values fragiles.
"""

import math
import sys
from pathlib import Path

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import numpy as np  # noqa: E402
import pytest  # noqa: E402

import engine  # noqa: E402


def _gbm_q_model(eng, s0=100.0, r=0.05, q=0.0, sigma=0.2, observable="EQ.SPOT.TEST"):
    return eng.create_model("GBM", {"s0": s0, "r": r, "q": q, "sigma": sigma, "observable": observable})


def _gbm_p_model(eng, s0=100.0, mu=0.08, sigma=0.2, observable="EQ.SPOT.TEST"):
    return eng.create_model("GBM_P", {"s0": s0, "mu": mu, "sigma": sigma, "observable": observable})


def _market(maturity, zero_rate=0.05):
    # T = market.pillars()[-1] (PLAN_IMPROVE_NOTEBOOK.md Fase 0, criterio explicito): el ultimo
    # pillar de la curva es el horizonte de simulacion.
    return engine.MarketSnapshot(pillars=[maturity], zero_rates=[zero_rate])


def _pricing(n_paths, n_steps, seed):
    return engine.PricingContext({"n_paths": float(n_paths), "n_steps": float(n_steps), "seed": float(seed)})


def test_simulate_paths_shape_times_grid_and_t0_column_equals_s0():
    eng = engine.Engine()
    s0, maturity = 100.0, 2.0
    n_paths, n_steps = 500, 8
    model = _gbm_q_model(eng, s0=s0)
    market = _market(maturity)
    pricing = _pricing(n_paths, n_steps, seed=1)

    times, paths = eng.simulate_paths(model, market, pricing)

    assert times.shape == (n_steps + 1,)
    assert paths.shape == (n_paths, n_steps + 1)
    assert times[0] == 0.0
    assert math.isclose(times[-1], maturity, rel_tol=1e-12)
    # malla uniforme dt = T/n_steps
    dt = maturity / n_steps
    assert np.allclose(np.diff(times), dt)
    # t=0 no se simula: S0 conocido, identico para todas las rutas
    assert np.all(paths[:, 0] == s0)


def test_simulate_paths_gbm_q_terminal_moments_match_black_scholes_lognormal_formula():
    eng = engine.Engine()
    s0, r, q_div, sigma, maturity = 100.0, 0.05, 0.01, 0.25, 1.5
    n_paths, n_steps = 30_000, 12
    model = _gbm_q_model(eng, s0=s0, r=r, q=q_div, sigma=sigma)
    market = _market(maturity, zero_rate=r)
    pricing = _pricing(n_paths, n_steps, seed=7)

    _, paths = eng.simulate_paths(model, market, pricing)
    terminal = paths[:, -1]

    mean = terminal.mean()
    variance = terminal.var(ddof=1)
    std_error = math.sqrt(variance / n_paths)

    # E_Q[S_T] = S0 * exp((r-q)*T) -- invariante de martingala bajo Q.
    expected_mean = s0 * math.exp((r - q_div) * maturity)
    assert abs(mean - expected_mean) < 6.0 * std_error, f"mean={mean} expected={expected_mean} se={std_error}"

    # Var_Q[S_T] = S0^2 * exp(2*(r-q)*T) * (exp(sigma^2*T) - 1) -- varianza cerrada lognormal.
    expected_variance = s0**2 * math.exp(2.0 * (r - q_div) * maturity) * (math.exp(sigma**2 * maturity) - 1.0)
    relative_error = abs(variance - expected_variance) / expected_variance
    assert relative_error < 0.1, f"variance={variance} expected={expected_variance} rel_err={relative_error}"


def test_simulate_paths_gbm_p_terminal_mean_matches_e_p_s_t_equals_s0_exp_mu_t():
    eng = engine.Engine()
    s0, mu, sigma, maturity = 100.0, 0.08, 0.2, 1.0
    n_paths, n_steps = 30_000, 10
    model = _gbm_p_model(eng, s0=s0, mu=mu, sigma=sigma)
    market = _market(maturity)
    pricing = _pricing(n_paths, n_steps, seed=99)

    _, paths = eng.simulate_paths(model, market, pricing)
    terminal = paths[:, -1]

    mean = terminal.mean()
    std_error = math.sqrt(terminal.var(ddof=1) / n_paths)
    expected_mean = s0 * math.exp(mu * maturity)
    assert abs(mean - expected_mean) < 6.0 * std_error, f"mean={mean} expected={expected_mean} se={std_error}"


def test_simulate_paths_same_seed_is_reproducible():
    eng = engine.Engine()
    model = _gbm_q_model(eng)
    market = _market(1.0)
    pricing = _pricing(200, 5, seed=123)

    _, paths_1 = eng.simulate_paths(model, market, pricing)
    _, paths_2 = eng.simulate_paths(model, market, pricing)

    assert np.array_equal(paths_1, paths_2)


def test_simulate_paths_rejects_a_model_that_generates_no_observable():
    eng = engine.Engine()
    hw = eng.create_model("HullWhite1F", {"a": 0.1, "b": 0.03, "sigma": 0.01, "r0": 0.02})
    market = _market(1.0)
    pricing = _pricing(100, 5, seed=1)

    with pytest.raises(ValueError):
        eng.simulate_paths(hw, market, pricing)


def test_simulate_paths_rejects_n_paths_times_n_steps_over_the_hard_cap():
    # PLAN_IMPROVE_NOTEBOOK.md Fase 0, tope duro documentado: 50_000 paths x 500 pasos
    # (rust/crates/engine-core/src/api.rs::SIMULATE_PATHS_MAX_PATHS/SIMULATE_PATHS_MAX_STEPS).
    eng = engine.Engine()
    model = _gbm_q_model(eng)
    market = _market(1.0)

    over_paths = _pricing(50_001, 10, seed=1)
    with pytest.raises(ValueError):
        eng.simulate_paths(model, market, over_paths)

    over_steps = _pricing(10, 501, seed=1)
    with pytest.raises(ValueError):
        eng.simulate_paths(model, market, over_steps)


def _gbm_basket_model(eng, s0=(100.0, 50.0), r=(0.03, 0.03), q=(0.0, 0.0), sigma=(0.2, 0.35), rho=0.4):
    return eng.create_model(
        "GbmBasket",
        {
            "observables": ["EQ.SPOT.A", "EQ.SPOT.B"],
            "s0": list(s0),
            "r": list(r),
            "q": list(q),
            "sigma": list(sigma),
            "correlation": [1.0, rho, rho, 1.0],
        },
    )


# PLAN_IMPROVE_NOTEBOOK2.md Fase 4: eng.simulate_paths sobre GbmBasketModel -- generaliza los
# tests de arriba (GBM/GBM_P, un unico observable) a N activos correlacionados. La forma de salida
# gana una dimension extra (n_assets); el resto de la convencion (times[0] == 0.0, paths[:, 0, :]
# == s0 sin simular, malla uniforme dt = T/n_steps) es identica.


def test_simulate_paths_basket_shape_times_grid_and_t0_columns_equal_s0_per_asset():
    eng = engine.Engine()
    s0 = (100.0, 50.0)
    maturity = 2.0
    n_paths, n_steps = 500, 8
    model = _gbm_basket_model(eng, s0=s0)
    market = _market(maturity)
    pricing = _pricing(n_paths, n_steps, seed=1)

    times, paths = eng.simulate_paths(model, market, pricing)

    assert times.shape == (n_steps + 1,)
    assert paths.shape == (n_paths, n_steps + 1, 2)
    assert times[0] == 0.0
    assert math.isclose(times[-1], maturity, rel_tol=1e-12)
    dt = maturity / n_steps
    assert np.allclose(np.diff(times), dt)
    # t=0 no se simula: S0 de CADA activo, identico para todas las rutas.
    assert np.all(paths[:, 0, 0] == s0[0])
    assert np.all(paths[:, 0, 1] == s0[1])


def test_simulate_paths_basket_terminal_marginal_moments_per_asset_match_black_scholes_lognormal_formula():
    # PLAN_IMPROVE_NOTEBOOK2.md Fase 4, criterio de aceptacion: paridad de momentos MARGINALES por
    # activo -- mismo criterio de tolerancia que el test equivalente de GBM de un unico activo
    # arriba, aplicado a cada activo del basket por separado (la correlacion solo afecta la
    # relacion ENTRE activos, nunca la distribucion marginal de uno solo).
    eng = engine.Engine()
    s0 = (100.0, 60.0)
    r, q_div, sigma = (0.05, 0.05), (0.01, 0.02), (0.25, 0.3)
    maturity = 1.5
    n_paths, n_steps = 30_000, 10
    model = _gbm_basket_model(eng, s0=s0, r=r, q=q_div, sigma=sigma, rho=0.5)
    market = _market(maturity, zero_rate=r[0])
    pricing = _pricing(n_paths, n_steps, seed=7)

    _, paths = eng.simulate_paths(model, market, pricing)

    for asset in (0, 1):
        terminal = paths[:, -1, asset]
        mean = terminal.mean()
        variance = terminal.var(ddof=1)
        std_error = math.sqrt(variance / n_paths)

        expected_mean = s0[asset] * math.exp((r[asset] - q_div[asset]) * maturity)
        assert abs(mean - expected_mean) < 6.0 * std_error, (
            f"asset={asset} mean={mean} expected={expected_mean} se={std_error}"
        )

        expected_variance = (
            s0[asset] ** 2 * math.exp(2.0 * (r[asset] - q_div[asset]) * maturity) * (math.exp(sigma[asset] ** 2 * maturity) - 1.0)
        )
        relative_error = abs(variance - expected_variance) / expected_variance
        assert relative_error < 0.1, f"asset={asset} variance={variance} expected={expected_variance} rel_err={relative_error}"


def test_simulate_paths_basket_same_seed_is_reproducible():
    eng = engine.Engine()
    model = _gbm_basket_model(eng)
    market = _market(1.0)
    pricing = _pricing(200, 5, seed=123)

    _, paths_1 = eng.simulate_paths(model, market, pricing)
    _, paths_2 = eng.simulate_paths(model, market, pricing)

    assert np.array_equal(paths_1, paths_2)


def test_simulate_paths_uses_the_last_pillar_of_the_market_as_the_horizon():
    eng = engine.Engine()
    model = _gbm_q_model(eng)
    n_paths, n_steps = 50, 4
    market = engine.MarketSnapshot(pillars=[0.5, 1.0, 3.0], zero_rates=[0.04, 0.045, 0.05])
    pricing = _pricing(n_paths, n_steps, seed=1)

    times, _ = eng.simulate_paths(model, market, pricing)

    assert math.isclose(times[-1], 3.0, rel_tol=1e-12)
