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

/// Peso de Volga (segunda derivada PURA respecto de `sigma`) -- `Volga = E[h(S_T) *
/// volga_weight(Z)]`. Derivado con el MISMO metodo que `gamma_weight`/`vanna_weight`
/// (PLAN_BACKWARD.md §9 Fase 1): la identidad `d^2/dtheta^2 E[h(S_T)] = E[h(S_T) * ((d ln f/dtheta)^2
/// + d^2 ln f/dtheta^2)]` evaluada en `theta = sigma`, con `f` la densidad lognormal de `S_T` bajo
/// GBM y `Z` la normal estandar recuperada por `recover_terminal_z`.
///
/// Derivacion (mantener aqui para auditar el algebra, no solo el resultado): fijando el punto
/// observado `s = S_T` (y por tanto `x = ln(s/s0)`) y dejando variar `sigma`, `ln f(s;sigma) =
/// -ln(sigma) - const - z(sigma)^2/2` donde `z(sigma) = (x - (r-q-0.5*sigma^2)*t) / (sigma*sqrt(t))`
/// es la MISMA `Z` recuperada por `recover_terminal_z`, ahora vista como funcion de `sigma` con `x`
/// fijo (el termino `-ln(s)` no depende de `sigma`, asi que no afecta a ninguna derivada respecto de
/// `sigma`). Escribiendo `N(sigma) = x - (r-q)*t + 0.5*sigma^2*t` (el numerador de `z`, con
/// `dN/dsigma = sigma*t`):
///
/// ```text
/// dz/dsigma  = sqrt(t) - z/sigma
/// d(z^2/2)/dsigma  = z*sqrt(t) - z^2/sigma
/// d^2(z^2/2)/dsigma^2 = t - 3*z*sqrt(t)/sigma + 3*z^2/sigma^2
///
/// d ln f/dsigma   = -1/sigma - z*sqrt(t) + z^2/sigma       = (z^2-1)/sigma - z*sqrt(t)
/// d^2 ln f/dsigma^2 = 1/sigma^2 - t + 3*z*sqrt(t)/sigma - 3*z^2/sigma^2
///
/// volga_weight = (d ln f/dsigma)^2 + d^2 ln f/dsigma^2
///              = (z^4 - 5*z^2 + 2)/sigma^2 + sqrt(t)*z*(5 - 2*z^2)/sigma + t*(z^2 - 1)
/// ```
///
/// Verificado por cuadratura determinista contra la segunda diferencia finita respecto de `sigma`
/// de la propia integral (no del integrando) en `tests`, mismo criterio que `gamma_weight`/
/// `vanna_weight`: sin `s0` en la formula (una derivada pura en `sigma` no depende de `s0`), a
/// diferencia de `gamma_weight`/`vanna_weight` que si escalan por `1/s0^2`/`1/s0`.
pub(crate) fn volga_weight(z: f64, sigma: f64, t: f64) -> f64 {
    let sqrt_t = t.sqrt();
    let z2 = z * z;
    (z2 * z2 - 5.0 * z2 + 2.0) / (sigma * sigma) + sqrt_t * z * (5.0 - 2.0 * z2) / sigma + t * (z2 - 1.0)
}

/// Las tres entradas del Hessiano local de una unica fecha terminal, reutilizando una UNICA `z`
/// recuperada por ruta (PLAN_BACKWARD.md §9 Fase 1: "una pasada cara, muchas derivadas baratas") --
/// evita que quien llama repita `gamma_weight`/`volga_weight`/`vanna_weight` a mano en tres sitios
/// distintos.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct LocalHessianWeights {
    pub gamma: f64,
    pub volga: f64,
    pub vanna: f64,
}

pub(crate) fn local_hessian_weights(z: f64, s0: f64, sigma: f64, t: f64) -> LocalHessianWeights {
    LocalHessianWeights {
        gamma: gamma_weight(z, s0, sigma, t),
        volga: volga_weight(z, sigma, t),
        vanna: vanna_weight(z, s0, sigma, t),
    }
}

// PLAN_BACKWARD.md §4.2 (investigacion, NO implementado -- no existe hoy ningun payoff
// multi-activo, `Gbm` es de un unico observable): generalizacion de `gamma_weight`/`volga_weight`/
// `vanna_weight` a `n` activos lognormales con Brownianos correlacionados (matriz de correlacion
// `rho` FIJA, no un parametro a derivar). La densidad conjunta de `S_T` en un punto `s` tiene la
// forma `ln f(s;theta) = A(theta) - 0.5 * eps(theta)^T P eps(theta)`, donde `eps(theta)` es el
// vector de residuos estandarizados (`eps_k = (ln(s_k/s0_k) - mu_k(theta)*T) / (sigma_k(theta) *
// sqrt(T))`, evaluado en el punto observado `s`, como funcion de los parametros `theta`), `P =
// rho^-1` (la matriz de precision, constante) y `A(theta)` recoge los terminos normalizadores que
// dependen de `theta` solo a traves de `sigma_k` (nunca de `s0_k`). El peso de Hessiano para
// cualquier par de parametros `(theta_i, theta_j)` es la misma identidad que arriba, ahora con
// productos matriciales:
//
// ```text
// weight_ij(eps) = (d ln f/d theta_i)(d ln f/d theta_j) + d^2 ln f/(d theta_i d theta_j)
//
// d ln f/d theta_i        = A'_i(theta) - eps^T P (d eps/d theta_i)
// d^2 ln f/(d theta_i d theta_j) = A''_ij(theta) - (d eps/d theta_j)^T P (d eps/d theta_i)
//                                  - eps^T P (d^2 eps/(d theta_i d theta_j))
// ```
//
// `Gamma = E[valor_presente(ruta) * weight_ii]`, `Hessiano_cruzado_ij = E[valor_presente(ruta) *
// weight_ij]` -- de nuevo sin derivar el payoff, solo reponderando. Para `n=1` esta formula se
// reduce exactamente a `gamma_weight`/`vanna_weight`/`volga_weight` de arriba.
//
// HALLAZGO NO OBVIO (verificado numericamente por cuadratura 2D en PLAN_BACKWARD.md §4.2, factor
// de error ~2 si se ignora): en presencia de correlacion, ni siquiera la Gamma "propia" de un
// activo (mantener fijo todo excepto `s0_1`) coincide con la formula univariante de un unico activo
// aislado -- hay un termino de correccion que depende de `rho` y del residuo del OTRO activo
// (`eps_2`), porque se esta derivando la densidad CONJUNTA, no la marginal. Cualquier implementacion
// futura de esta formula (condicionada a que exista un payoff multi-activo real, PLAN_BACKWARD.md
// §12) DEBE repetir esa verificacion por cuadratura antes de confiar en la formula -- no se
// implementa aqui, solo se documenta.

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
    fn volga_weight_matches_numerical_second_derivative_for_a_smooth_payoff() {
        // Mismo criterio que gamma_weight_matches_numerical_second_derivative_for_a_smooth_payoff,
        // pero diferenciando respecto de sigma en vez de s0.
        let (s0, r, q, sigma, t) = (100.0, 0.05, 0.02, 0.3, 0.75);
        let h = |x: f64| x * x;

        let v = |sigma: f64| integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)));
        let eps = 1e-4;
        let numerical_volga = (v(sigma + eps) - 2.0 * v(sigma) + v(sigma - eps)) / (eps * eps);

        let lrm_volga =
            integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)) * volga_weight(z, sigma, t));

        assert!(
            (lrm_volga - numerical_volga).abs() < 1e-3 * numerical_volga.abs().max(1.0),
            "lrm={lrm_volga} numerical={numerical_volga}"
        );
    }

    #[test]
    fn volga_weight_matches_numerical_second_derivative_for_a_kinked_call_payoff() {
        // El caso que motiva el modulo: h tiene un kink real (max(S_T-K,0)); numerical_volga sigue
        // siendo un oraculo valido porque diferencia la EXPECTATION (suave en sigma), no el
        // integrando.
        let (s0, strike, r, q, sigma, t) = (100.0, 100.0, 0.05, 0.0, 0.2, 1.0);
        let h = |x: f64| (x - strike).max(0.0);

        let v = |sigma: f64| integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)));
        // eps mas grande que en el caso suave: la derivada finita de segundo orden divide por
        // eps^2, amplificando el error de discretizacion de `integrate_normal` cerca del kink
        // (mismo motivo por el que `vanna_weight_matches_numerical_mixed_partial_for_a_kinked_call_payoff`
        // usa una tolerancia mas laxa que su contraparte suave).
        let eps = 1e-3;
        let numerical_volga = (v(sigma + eps) - 2.0 * v(sigma) + v(sigma - eps)) / (eps * eps);

        let lrm_volga =
            integrate_normal(|z| h(s_t_of(s0, r, q, sigma, t, z)) * volga_weight(z, sigma, t));

        assert!(
            (lrm_volga - numerical_volga).abs() < 1e-2 * numerical_volga.abs().max(1.0),
            "lrm={lrm_volga} numerical={numerical_volga}"
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
