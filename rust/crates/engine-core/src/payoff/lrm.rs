//! Segundo orden puro/cruzado ("Gamma"/"Vanna") de un `CompiledPayoff` bajo GBM via el metodo del
//! ratio de verosimilitud ("likelihood ratio method", Broadie & Glasserman 1996) -- PLAN_HYPERDUAL.md
//! §5 (revisado): la generalizacion original de este documento (`Dual2`/`HyperDual`, propagar la
//! aritmetica dual DOS veces a traves del mismo interprete de payoff que ya usa `sensitivity.rs`)
//! resulto ser matematicamente INCORRECTA para cualquier payoff con una ramificacion que dependa
//! del propio parametro que se deriva (`Max`/`Min`/`Abs`/`If`/`Trigger` -- es decir, practicamente
//! todas las calls/puts/barreras reales, ver PLAN_HYPERDUAL.md §5 "Por que Dual2/HyperDual no
//! funcionan" para la prueba). Este modulo es el reemplazo correcto para el caso mas comun: un
//! payoff que depende del subyacente en una UNICA fecha terminal (sin dependencia de trayectoria).
//!
//! **La idea (density differentiation, no pathwise differentiation)**: en vez de derivar el PAYOFF
//! `h(S_T)` respecto del parametro (lo que exige que `h` sea suave, y falla en el kink), se deriva
//! la DENSIDAD de `S_T` respecto del parametro y se deja `h` intacto:
//!
//! ```text
//! d/dtheta E[h(S_T)] = d/dtheta integral h(s) f(s;theta) ds = E[h(S_T) * (d ln f/dtheta)(S_T)]
//! ```
//!
//! Esta identidad NO exige que `h` sea diferenciable en ningun punto -- solo que sea integrable
//! (medible y con momentos finitos), asi que un kink (o incluso una discontinuidad genuina, como un
//! payoff digital) no es un problema: `h` nunca se deriva, solo se EVALUA (via el interprete `f64`
//! normal, `eval::evaluate_with_events_seeded`) y se pondera por un peso deterministico de la
//! normal estandar realizada en esa ruta. Aplicar la identidad una segunda vez (derivando `ln f`
//! otra vez respecto del mismo parametro, o de un segundo parametro para la cruzada) da Gamma/Vanna
//! sin necesitar `h''` en ningun momento -- ver `gamma_weight`/`vanna_weight` para las formulas
//! cerradas resultantes (GBM es lognormal, asi que `f` tiene forma cerrada).
//!
//! **Alcance deliberadamente limitado**: la formula de `f` de abajo es la densidad MARGINAL de
//! `S_T` en la UNICA fecha terminal -- generalizarla a un payoff con dependencia de trayectoria
//! (barreras, triggers, `Exercise`) exigiria la densidad conjunta de TODA la trayectoria (Malliavin
//! calculus sobre el proceso completo, Fournie et al. 1999), fuera de alcance de este modulo.
//! `single_terminal_time` es el guard que impide aplicar estas formulas fuera de su dominio de
//! validez: un contrato con mas de una fecha requerida (o con `ContractOp::Exercise`) se rechaza
//! explicito, nunca se aproxima en silencio.

use super::ir::CompiledPayoff;

/// `true` solo si `payoff` depende del subyacente en una UNICA fecha (sin barreras/triggers/
/// ejercicio que anadan mas fechas de monitorizacion) -- unico caso donde `gamma_weight`/
/// `vanna_weight` son validos (ver el doc-comment del modulo). Devuelve esa fecha unica.
pub(crate) fn single_terminal_time(payoff: &CompiledPayoff) -> Result<f64, String> {
    let times = payoff.required_times();
    match times.as_slice() {
        [t] => Ok(*t),
        [] => Err(
            "payoff: el contrato no depende de ningun instante de mercado (nada que derivar)".to_string(),
        ),
        _ => Err(format!(
            "payoff: Gamma/Vanna via likelihood ratio solo soportado para contratos de una UNICA fecha \
             terminal (sin dependencia de trayectoria) -- este contrato requiere {} fechas distintas \
             ({:?}); usa method='bump_and_reval' para Gamma/Vanna de payoffs path-dependientes \
             (PLAN_HYPERDUAL.md §5)",
            times.len(),
            times
        )),
    }
}

/// Recupera la normal estandar `Z` realizada por una ruta GBM ya simulada (`S_t = s0 *
/// exp((r-q-0.5*sigma^2)*t + sigma*sqrt(t)*Z)`) -- mismo Browniano que `GbmDualPath` recupera como
/// `W_t = sqrt(t)*Z`, aqui normalizado a varianza unidad porque los pesos de abajo estan escritos
/// en terminos de `Z ~ N(0,1)`, la convencion estandar de la literatura (Broadie-Glasserman).
pub(crate) fn recover_terminal_z(s0: f64, r: f64, q: f64, sigma: f64, t: f64, s_t: f64) -> f64 {
    let drift_no_diffusion = r - q - 0.5 * sigma * sigma;
    ((s_t / s0).ln() - drift_no_diffusion * t) / (sigma * t.sqrt())
}

/// Peso de Gamma (segunda derivada PURA respecto de `s0`) -- `Gamma = E[h(S_T) * gamma_weight(Z)]`
/// (sin descontar; quien llama aplica `exp(-r*T)` sobre el valor presente del ledger, no sobre este
/// peso). Derivado de `(d^2 f/ds0^2)/f` para la densidad lognormal de `S_T` (ver el doc-comment del
/// modulo); verificado por cuadratura determinista contra la segunda diferencia finita de la propia
/// integral en `tests` (sin ruido de Monte Carlo) y, en `api.rs`, contra el Gamma cerrado de
/// Black-Scholes.
pub(crate) fn gamma_weight(z: f64, s0: f64, sigma: f64, t: f64) -> f64 {
    (z * z - 1.0 - sigma * t.sqrt() * z) / (s0 * s0 * sigma * sigma * t)
}

/// Peso de Vanna (derivada cruzada `s0`/`sigma`, orden 1 en cada direccion) -- `Vanna = E[h(S_T) *
/// vanna_weight(Z)]`. Derivado de `(d^2 f/(ds0 dsigma))/f`; mismo criterio de verificacion que
/// `gamma_weight`.
pub(crate) fn vanna_weight(z: f64, s0: f64, sigma: f64, t: f64) -> f64 {
    let sqrt_t = t.sqrt();
    (z * (z * z - 3.0) / (sigma * sigma * sqrt_t) + (1.0 - z * z) / sigma) / s0
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Integra `E[f(Z)]` para `Z ~ N(0,1)` por cuadratura de punto medio en un rango finito --
    /// deterministico (sin RNG, sin `rng_test_lock`), suficiente para verificar identidades
    /// analiticas sin el ruido de un estimador Monte Carlo.
    fn integrate_normal<F: Fn(f64) -> f64>(f: F) -> f64 {
        let n = 400_000;
        let zmax = 9.0;
        let dz = 2.0 * zmax / n as f64;
        let mut acc = 0.0;
        for i in 0..n {
            let z = -zmax + (i as f64 + 0.5) * dz;
            let phi = (-0.5 * z * z).exp() / (2.0 * std::f64::consts::PI).sqrt();
            acc += f(z) * phi * dz;
        }
        acc
    }

    fn s_t_of(s0: f64, r: f64, q: f64, sigma: f64, t: f64, z: f64) -> f64 {
        s0 * ((r - q - 0.5 * sigma * sigma) * t + sigma * t.sqrt() * z).exp()
    }

    #[test]
    fn gamma_weight_matches_numerical_second_derivative_for_a_smooth_payoff() {
        // h(x)=x^2 no tiene ramificacion -- d^2/ds0^2 E[h(S_T)] puede obtenerse TAMBIEN por
        // diferencias finitas de la propia integral (oraculo independiente de gamma_weight).
        let (s0, r, q, sigma, t) = (100.0, 0.05, 0.02, 0.3, 0.75);
        let h = |x: f64| x * x;

        let v = |s0: f64| integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)));
        let eps = 1e-2;
        let numerical_gamma = (v(s0 + eps) - 2.0 * v(s0) + v(s0 - eps)) / (eps * eps);

        let lrm_gamma = integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)) * gamma_weight(z, s0, sigma, t));

        assert!(
            (lrm_gamma - numerical_gamma).abs() < 1e-4 * numerical_gamma.abs().max(1.0),
            "lrm={lrm_gamma} numerical={numerical_gamma}"
        );
    }

    #[test]
    fn gamma_weight_matches_numerical_second_derivative_for_a_kinked_call_payoff() {
        // El caso que motiva todo el modulo: h tiene un kink real (max(S_T-K,0)). numerical_gamma
        // sigue siendo un oraculo valido porque diferencia la EXPECTATION (una funcion suave de
        // s0, aunque el integrando no lo sea), no el integrando.
        let (s0, strike, r, q, sigma, t) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let h = |x: f64| (x - strike).max(0.0);

        let v = |s0: f64| integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)));
        let eps = 1e-2;
        let numerical_gamma = (v(s0 + eps) - 2.0 * v(s0) + v(s0 - eps)) / (eps * eps);

        let lrm_gamma = integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)) * gamma_weight(z, s0, sigma, t));

        assert!(
            (lrm_gamma - numerical_gamma).abs() < 1e-3 * numerical_gamma.abs().max(1.0),
            "lrm={lrm_gamma} numerical={numerical_gamma}"
        );
    }

    #[test]
    fn vanna_weight_matches_numerical_mixed_partial_for_a_kinked_call_payoff() {
        let (s0, strike, r, q, sigma, t) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let h = |x: f64| (x - strike).max(0.0);

        let v = |s0: f64, sigma: f64| integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)));
        let (eps_s0, eps_sigma) = (1e-2, 1e-4);
        let numerical_vanna = (v(s0 + eps_s0, sigma + eps_sigma) - v(s0 + eps_s0, sigma - eps_sigma)
            - v(s0 - eps_s0, sigma + eps_sigma)
            + v(s0 - eps_s0, sigma - eps_sigma))
            / (4.0 * eps_s0 * eps_sigma);

        let lrm_vanna = integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)) * vanna_weight(z, s0, sigma, t));

        assert!(
            (lrm_vanna - numerical_vanna).abs() < 1e-2 * numerical_vanna.abs().max(1.0),
            "lrm={lrm_vanna} numerical={numerical_vanna}"
        );
    }

    #[test]
    fn single_terminal_time_accepts_exactly_one_required_time_and_rejects_others() {
        use crate::payoff::compile::compile;

        let single = compile(
            r#"{"schema":"engine.payoff/v1","id":"CALL","contract":{"type":"when","time":1.0,
                "child":{"type":"cashflow","currency":"USD",
                    "amount":{"type":"max","left":{"type":"sub",
                        "left":{"type":"fixing","observable":"EQ.SPOT.XYZ","time":1.0},
                        "right":{"type":"constant","value":100.0}},
                        "right":{"type":"constant","value":0.0}}}}}"#,
        )
        .unwrap();
        assert_eq!(single_terminal_time(&single).unwrap(), 1.0);

        let two_dates = compile(
            r#"{"schema":"engine.payoff/v1","id":"TWO","contract":{"type":"both","children":[
                {"type":"when","time":0.5,"child":{"type":"cashflow","currency":"USD",
                    "amount":{"type":"constant","value":1.0}}},
                {"type":"when","time":1.0,"child":{"type":"cashflow","currency":"USD",
                    "amount":{"type":"constant","value":1.0}}}]}}"#,
        )
        .unwrap();
        let err = single_terminal_time(&two_dates).unwrap_err();
        assert!(err.contains("2"));
    }
}
