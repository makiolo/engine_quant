//! Hessiano cerrado de Hull-White 1 factor via numeros duales de segundo orden (`Dual2`/
//! `HyperDual`), PLAN_BACKWARD.md §5/§9 Fase 2.
//!
//! **Por que existe este modulo, y por que NO reutiliza Burn**: `rust/crates/engine-core/src/
//! models/hull_white.rs` (el pricer "de produccion") vectoriza sobre paths con tensores Burn y
//! obtiene su gradiente de primer orden via `Autodiff<CpuBackend>` reverse-mode
//! (`api::irs_hull_white_npv_all_greeks`). Burn 0.21 no anida `Autodiff<Autodiff<_>>` (spike
//! documentado en PLAN_BACKWARD.md §1.2: la segunda pasada `backward()` no propaga gradiente a la
//! capa exterior), asi que no hay Hessiano "gratis" via Burn. La alternativa de este modulo es una
//! SEGUNDA implementacion escalar (sin tensores, sin Burn) del mismo pricer cerrado, parametrizada
//! sobre `T: DualNumber` (el mismo trait de `crate::payoff::dual`, reutilizado sin cambios,
//! ADR-BW-03/PLAN_BACKWARD.md §9 Fase 0) instanciado con `Dual2` (orden 2 puro, para las 4
//! entradas diagonales del Hessiano) y `HyperDual` (orden 1 cruzado, para las 6 entradas
//! cruzadas). ADR-BW-01 (PLAN_BACKWARD.md §9.0) confirmo, linea a linea, que el camino
//! `a`/`b`/`sigma`/`r0` -> NPV de un swap vainilla no tiene ninguna rama (`if`/`min`/`max`) -- es
//! decir, es una funcion analitica pura, la condicion exacta que hace valida la convencion de
//! Taylor sin normalizar de `Dual2`/`HyperDual` (a diferencia del motor de payoff, donde la MISMA
//! convencion daba una segunda derivada degenerada por los kinks del payoff,
//! PLAN_HYPERDUAL.md §5.0 -- por eso esos tipos nunca se implementaron alli).
//!
//! **Verificacion obligatoria (PLAN_BACKWARD.md §10), en este orden**:
//! 1. Aritmetica `Dual2`/`HyperDual` (`Mul`/`Div`/`exp`/`ln`/`powf`) contra diferencias finitas en
//!    una expresion representativa (`tests::dual2_arithmetic_matches_finite_differences`/
//!    `tests::hyperdual_arithmetic_matches_finite_differences`).
//! 2. `value`/gradiente de `hull_white_1f_hessian` contra la ruta Burn YA EN PRODUCCION
//!    (`crate::api::irs_hull_white_npv_all_greeks`) -- la comparacion MAS IMPORTANTE de esta fase:
//!    si estas dos divergen, el Hessiano de una tercera fuente (esta) no sirve de nada.
//! 3. Consistencia interna: el gradiente que dan las 6 llamadas `HyperDual` (por construccion,
//!    coincide con el de las 4 llamadas `Dual2`, calculado por un camino distinto).
//! 4. Las 10 entradas del Hessiano contra un estencil de diferencias finitas de bump-and-reval de
//!    segundo orden aplicado directamente sobre `hull_white_1f_hessian(...).value`.

use crate::payoff::dual::DualNumber;

// ---------------------------------------------------------------------------------------------
// `Dual2`: orden 2 puro (valor + primera derivada + segunda derivada de UN UNICO parametro).
// ---------------------------------------------------------------------------------------------

/// Convencion de Taylor SIN normalizar (ADR-BW-03, identica a PLAN_HYPERDUAL.md ADR-HD-02):
/// representa el polinomio truncado `value + d1*eps + d2*eps^2` de la serie
/// `f(x+eps) = f(x) + f'(x)*eps + f''(x)/2!*eps^2 + ...` -- `d2` guarda `f''(x)/2`, NO `f''(x)`
/// directamente. Solo `second_derivative()` aplica la correccion (`2.0 * d2`) al leer el
/// resultado final; la aritmetica (`Mul`/`exp`/`ln`/`powf`) nunca la aplica internamente, porque
/// es precisamente esa convencion la que hace que el producto de Cauchy truncado sea la
/// composicion de Taylor exacta sin factores de correccion intermedios.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct Dual2 {
    pub(crate) value: f64,
    pub(crate) d1: f64,
    pub(crate) d2: f64,
}

impl Dual2 {
    /// El parametro que se deriva (`d1 = 1`, punto de partida de la regla de la cadena de
    /// segundo orden).
    pub(crate) fn variable(value: f64) -> Self {
        Self { value, d1: 1.0, d2: 0.0 }
    }

    /// Constante: no depende del parametro que se esta diferenciando (`d1 = d2 = 0`).
    pub(crate) fn constant(value: f64) -> Self {
        Self { value, d1: 0.0, d2: 0.0 }
    }

    /// `f''(x)` real -- deshace la convencion sin normalizar de `d2` (ADR-BW-03).
    pub(crate) fn second_derivative(self) -> f64 {
        2.0 * self.d2
    }
}

impl std::ops::Add for Dual2 {
    type Output = Dual2;
    fn add(self, rhs: Dual2) -> Dual2 {
        Dual2 { value: self.value + rhs.value, d1: self.d1 + rhs.d1, d2: self.d2 + rhs.d2 }
    }
}

impl std::ops::Sub for Dual2 {
    type Output = Dual2;
    fn sub(self, rhs: Dual2) -> Dual2 {
        Dual2 { value: self.value - rhs.value, d1: self.d1 - rhs.d1, d2: self.d2 - rhs.d2 }
    }
}

impl std::ops::Neg for Dual2 {
    type Output = Dual2;
    fn neg(self) -> Dual2 {
        Dual2 { value: -self.value, d1: -self.d1, d2: -self.d2 }
    }
}

/// Producto de Cauchy truncado mod eps^3 (PLAN_HYPERDUAL.md §3.3, copiado literalmente --
/// ADR-BW-03 reutiliza esta convencion sin cambios).
impl std::ops::Mul for Dual2 {
    type Output = Dual2;
    fn mul(self, rhs: Dual2) -> Dual2 {
        Dual2 {
            value: self.value * rhs.value,
            d1: self.d1 * rhs.value + self.value * rhs.d1,
            d2: self.d2 * rhs.value + self.d1 * rhs.d1 + self.value * rhs.d2,
        }
    }
}

/// Cociente de series truncadas mod eps^3, derivado invirtiendo el producto de Cauchy de arriba
/// (`h = f/g` <=> `h*g = f`, igualando coeficiente a coeficiente) -- verificado contra
/// diferencias finitas en `tests::dual2_arithmetic_matches_finite_differences`.
impl std::ops::Div for Dual2 {
    type Output = Dual2;
    fn div(self, rhs: Dual2) -> Dual2 {
        let h0 = self.value / rhs.value;
        let h1 = (self.d1 - h0 * rhs.d1) / rhs.value;
        let h2 = (self.d2 - h0 * rhs.d2 - h1 * rhs.d1) / rhs.value;
        Dual2 { value: h0, d1: h1, d2: h2 }
    }
}

impl DualNumber for Dual2 {
    fn re(self) -> f64 {
        self.value
    }
    fn constant(value: f64) -> Self {
        Dual2::constant(value)
    }

    /// El pricer cerrado de Hull-White (ADR-BW-01) nunca ejerce `abs` -- implementado por
    /// completitud del trait con el mismo criterio conservador que `Dual::abs` (subgradiente en
    /// el signo de `.value`), sin pretender un significado matematico de segunda derivada de
    /// `abs` (que ni siquiera esta definida en 0).
    fn abs(self) -> Self {
        if self.value >= 0.0 {
            self
        } else {
            -self
        }
    }

    /// `exp(f(x+eps))` mod eps^3: derivado componiendo la serie de Taylor de `exp` con la de
    /// `self` (ver el doc-comment del modulo, punto 1 de verificacion) -- `d2' = exp(v)*(d2 +
    /// d1^2/2)` porque el termino cuadratico de `exp(u)` con `u = d1*eps+d2*eps^2` aporta
    /// `d1^2/2` ademas del `d2` ya presente en `u`.
    fn exp(self) -> Self {
        let v = self.value.exp();
        Dual2 { value: v, d1: v * self.d1, d2: v * (self.d2 + self.d1 * self.d1 * 0.5) }
    }

    /// `ln(f(x+eps))` mod eps^3: `d2' = d2/v - d1^2/(2*v^2)`, simetrico de `exp` (ver
    /// doc-comment del modulo).
    fn ln(self) -> Self {
        let v = self.value;
        Dual2 { value: v.ln(), d1: self.d1 / v, d2: self.d2 / v - (self.d1 * self.d1) / (2.0 * v * v) }
    }

    /// Unico caso que Hull-White 1F necesita: `self.powf(Dual2::constant(n))` con exponente
    /// CONSTANTE (`other.d1 == other.d2 == 0`) -- Hull-White solo eleva expresiones a exponentes
    /// literales (`a^2`, `sigma^2`, `b_t_t^2`, nunca `a^b`). Con AMBOS operandos no constantes
    /// haria falta la formula general de `a^b = exp(b*ln(a))`, que no se necesita aqui: se deja
    /// como error explicito en vez de una formula sin verificar.
    fn powf(self, other: Self) -> Self {
        assert!(
            other.d1 == 0.0 && other.d2 == 0.0,
            "Dual2::powf: exponente no constante no soportado (Hull-White 1F nunca lo necesita, \
             ver doc-comment de models::hull_white_dual)"
        );
        let n = other.value;
        let a0 = self.value;
        let d1 = n * a0.powf(n - 1.0) * self.d1;
        let d2 = n * a0.powf(n - 1.0) * self.d2 + (n * (n - 1.0) * 0.5) * a0.powf(n - 2.0) * self.d1 * self.d1;
        Dual2 { value: a0.powf(n), d1, d2 }
    }

    /// Nunca ejercido por Hull-White (ADR-BW-01) -- misma rama entera que `Dual::min`.
    fn min(self, other: Self) -> Self {
        if self.value <= other.value {
            self
        } else {
            other
        }
    }

    /// Nunca ejercido por Hull-White (ADR-BW-01) -- misma rama entera que `Dual::max`.
    fn max(self, other: Self) -> Self {
        if self.value >= other.value {
            self
        } else {
            other
        }
    }
}

// ---------------------------------------------------------------------------------------------
// `HyperDual`: orden 1 cruzado (valor + d/dx + d/dy + d^2/dxdy).
// ---------------------------------------------------------------------------------------------

/// Algebra `eps^2 = eta^2 = 0`, `eps*eta` sobrevive (PLAN_HYPERDUAL.md §3.4): `dxy` guarda
/// `d^2f/dxdy` SIN normalizar (la serie de Taylor bivariada no lleva factorial en el termino
/// cruzado, a diferencia de `Dual2::d2`, ADR-BW-03).
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct HyperDual {
    pub(crate) value: f64,
    pub(crate) dx: f64,
    pub(crate) dy: f64,
    pub(crate) dxy: f64,
}

impl HyperDual {
    /// Direccion primaria (`dx = 1`, resto 0).
    pub(crate) fn variable_x(value: f64) -> Self {
        Self { value, dx: 1.0, dy: 0.0, dxy: 0.0 }
    }

    /// Direccion secundaria (`dy = 1`, resto 0).
    pub(crate) fn variable_y(value: f64) -> Self {
        Self { value, dx: 0.0, dy: 1.0, dxy: 0.0 }
    }

    /// Constante: ninguna componente de derivada.
    pub(crate) fn constant(value: f64) -> Self {
        Self { value, dx: 0.0, dy: 0.0, dxy: 0.0 }
    }
}

impl std::ops::Add for HyperDual {
    type Output = HyperDual;
    fn add(self, rhs: HyperDual) -> HyperDual {
        HyperDual {
            value: self.value + rhs.value,
            dx: self.dx + rhs.dx,
            dy: self.dy + rhs.dy,
            dxy: self.dxy + rhs.dxy,
        }
    }
}

impl std::ops::Sub for HyperDual {
    type Output = HyperDual;
    fn sub(self, rhs: HyperDual) -> HyperDual {
        HyperDual {
            value: self.value - rhs.value,
            dx: self.dx - rhs.dx,
            dy: self.dy - rhs.dy,
            dxy: self.dxy - rhs.dxy,
        }
    }
}

impl std::ops::Neg for HyperDual {
    type Output = HyperDual;
    fn neg(self) -> HyperDual {
        HyperDual { value: -self.value, dx: -self.dx, dy: -self.dy, dxy: -self.dxy }
    }
}

/// Copiado literalmente de PLAN_HYPERDUAL.md §3.4.
impl std::ops::Mul for HyperDual {
    type Output = HyperDual;
    fn mul(self, rhs: HyperDual) -> HyperDual {
        HyperDual {
            value: self.value * rhs.value,
            dx: self.dx * rhs.value + self.value * rhs.dx,
            dy: self.dy * rhs.value + self.value * rhs.dy,
            dxy: self.dxy * rhs.value + self.value * rhs.dxy + self.dx * rhs.dy + self.dy * rhs.dx,
        }
    }
}

/// Cociente derivado invirtiendo el producto de arriba (`h*g = f`, igualando componente a
/// componente) -- verificado contra diferencias finitas en
/// `tests::hyperdual_arithmetic_matches_finite_differences`.
impl std::ops::Div for HyperDual {
    type Output = HyperDual;
    fn div(self, rhs: HyperDual) -> HyperDual {
        let h0 = self.value / rhs.value;
        let hx = (self.dx - h0 * rhs.dx) / rhs.value;
        let hy = (self.dy - h0 * rhs.dy) / rhs.value;
        let hxy = (self.dxy - h0 * rhs.dxy - hx * rhs.dy - hy * rhs.dx) / rhs.value;
        HyperDual { value: h0, dx: hx, dy: hy, dxy: hxy }
    }
}

impl DualNumber for HyperDual {
    fn re(self) -> f64 {
        self.value
    }
    fn constant(value: f64) -> Self {
        HyperDual::constant(value)
    }

    /// Nunca ejercido por Hull-White (ADR-BW-01) -- mismo criterio conservador que `Dual2::abs`.
    fn abs(self) -> Self {
        if self.value >= 0.0 {
            self
        } else {
            -self
        }
    }

    /// `exp` sobre la serie truncada `eps^2=eta^2=0`: `dxy' = exp(v)*(dxy + dx*dy)` (el termino
    /// cruzado de `exp(u)` con `u=dx*eps+dy*eta+dxy*eps*eta` aporta `dx*dy` ademas del `dxy` ya
    /// presente en `u`, ver el doc-comment del modulo).
    fn exp(self) -> Self {
        let v = self.value.exp();
        HyperDual { value: v, dx: v * self.dx, dy: v * self.dy, dxy: v * (self.dxy + self.dx * self.dy) }
    }

    /// `ln`, simetrico de `exp`: `dxy' = dxy/v - dx*dy/v^2`.
    fn ln(self) -> Self {
        let v = self.value;
        HyperDual {
            value: v.ln(),
            dx: self.dx / v,
            dy: self.dy / v,
            dxy: self.dxy / v - (self.dx * self.dy) / (v * v),
        }
    }

    /// Mismo criterio que `Dual2::powf`: solo exponente CONSTANTE (Hull-White 1F nunca necesita
    /// mas).
    fn powf(self, other: Self) -> Self {
        assert!(
            other.dx == 0.0 && other.dy == 0.0 && other.dxy == 0.0,
            "HyperDual::powf: exponente no constante no soportado (Hull-White 1F nunca lo necesita, \
             ver doc-comment de models::hull_white_dual)"
        );
        let n = other.value;
        let a0 = self.value;
        let dx = n * a0.powf(n - 1.0) * self.dx;
        let dy = n * a0.powf(n - 1.0) * self.dy;
        let dxy = n * a0.powf(n - 1.0) * self.dxy + (n * (n - 1.0)) * a0.powf(n - 2.0) * self.dx * self.dy;
        HyperDual { value: a0.powf(n), dx, dy, dxy }
    }

    /// Nunca ejercido por Hull-White (ADR-BW-01).
    fn min(self, other: Self) -> Self {
        if self.value <= other.value {
            self
        } else {
            other
        }
    }

    /// Nunca ejercido por Hull-White (ADR-BW-01).
    fn max(self, other: Self) -> Self {
        if self.value >= other.value {
            self
        } else {
            other
        }
    }
}

// ---------------------------------------------------------------------------------------------
// `f64` como `DualNumber` trivial: permite reutilizar las MISMAS funciones genericas de abajo
// (`zero_coupon_bond`/`npv`/`par_rate`) con `T=f64` para el valor base y para el tipo fijo "a la
// par" en aritmetica f64 pura, sin duplicar ninguna formula (PLAN_BACKWARD.md §9 Fase 2, "opcional
// recomendado").
// ---------------------------------------------------------------------------------------------

impl DualNumber for f64 {
    fn re(self) -> f64 {
        self
    }
    fn constant(value: f64) -> Self {
        value
    }
    fn abs(self) -> Self {
        f64::abs(self)
    }
    fn exp(self) -> Self {
        f64::exp(self)
    }
    fn ln(self) -> Self {
        f64::ln(self)
    }
    fn powf(self, other: Self) -> Self {
        f64::powf(self, other)
    }
    fn min(self, other: Self) -> Self {
        f64::min(self, other)
    }
    fn max(self, other: Self) -> Self {
        f64::max(self, other)
    }
}

// ---------------------------------------------------------------------------------------------
// Pricer cerrado de Hull-White 1F, generico sobre `T: DualNumber` -- traduccion mecanica de
// `models::hull_white::HullWhite1F::b_factor`/`a_factor`/`zero_coupon_bond` (tensores Burn ->
// escalares `T`, ADR-BW-01 ya confirmo que no hay ninguna rama en este camino) y de
// `products::irs::IrSwap::npv`/`par_rate` (a `t=0`/`t=start` respectivamente, igual que el
// original).
// ---------------------------------------------------------------------------------------------

/// `B(t,T) = (1 - exp(-a*(T-t))) / a`.
fn b_factor<T: DualNumber>(a: T, tau: f64) -> T {
    let neg_a_tau = a * T::constant(-tau);
    let one_minus_exp = -neg_a_tau.exp() + T::constant(1.0);
    one_minus_exp / a
}

/// `A(t,T)` del precio afin del bono cero-cupon (Brigo-Mercurio ec. 3.9) -- misma formula que
/// `HullWhite1F::a_factor`.
fn a_factor<T: DualNumber>(a: T, b: T, sigma: T, tau: f64, b_t_t: T) -> T {
    let a2 = a * a;
    let sigma2 = sigma * sigma;

    let term1 = (b_t_t + T::constant(-tau)) * (a2 * b - sigma2 * T::constant(0.5)) / a2;
    let term2 = (sigma2 * b_t_t * b_t_t) / (a * T::constant(4.0));

    (term1 - term2).exp()
}

/// `P(t,T) = A(t,T) * exp(-B(t,T)*r_t)` -- misma formula que `HullWhite1F::zero_coupon_bond`.
fn zero_coupon_bond<T: DualNumber>(a: T, b: T, sigma: T, r_t: T, t: f64, maturity: f64) -> T {
    let tau = maturity - t;
    let b_t_t = b_factor(a, tau);
    let a_t_t = a_factor(a, b, sigma, tau, b_t_t);
    a_t_t * (-(b_t_t * r_t)).exp()
}

/// NPV del swap pagador a `t=0` -- misma formula que `IrSwap::npv` (limitacion identica: requiere
/// `t=0 <= start`, siempre cierto para el unico caso que se necesita aqui,
/// `irs_hull_white_npv_all_greeks` siempre llama a `t=0.0`). `notional`/`fixed_rate`/`accruals`/
/// `payment_times`/`start` son `f64` PUROS -- constantes que nunca se derivan, igual que en la
/// version Burn (`build_irs_swap`/`irs_hull_white_npv_all_greeks`: el cupon fijo de un swap ya
/// emitido no depende del parametro que se esta shockeando).
#[allow(clippy::too_many_arguments)]
fn npv<T: DualNumber>(
    a: T,
    b: T,
    sigma: T,
    r0: T,
    notional: f64,
    fixed_rate: f64,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
) -> T {
    let p_start = zero_coupon_bond(a, b, sigma, r0, 0.0, start);
    let p_end = zero_coupon_bond(a, b, sigma, r0, 0.0, *payment_times.last().expect("un swap necesita al menos un periodo"));
    let floating_leg = T::constant(notional) * (p_start - p_end);

    let fixed_leg = payment_times
        .iter()
        .zip(accruals.iter())
        .map(|(&ti, &tau)| {
            let p_i = zero_coupon_bond(a, b, sigma, r0, 0.0, ti);
            T::constant(notional * fixed_rate * tau) * p_i
        })
        .fold(T::constant(0.0), |acc, leg| acc + leg);

    floating_leg - fixed_leg
}

/// Tipo fijo "a la par" (`NPV = 0`) visto desde `t=start` -- misma formula que `IrSwap::par_rate`
/// (a diferencia de `npv`, aqui la fecha de valoracion es `start`, no `0.0`, igual que el
/// original: `p_start = zero_coupon_bond(state, start, start)`). Se llama SIEMPRE con `T=f64`
/// (aritmetica plana, sin derivar) -- mismo motivo que `build_irs_swap` en `api.rs`: el cupon fijo
/// de un swap ya emitido no debe moverse cuando se shockea `a`/`b`/`sigma`/`r0` para medir una
/// sensibilidad/Hessiano, solo el descuento con el que se revalora.
fn par_rate<T: DualNumber>(a: T, b: T, sigma: T, r0: T, start: f64, payment_times: &[f64], accruals: &[f64]) -> T {
    let p_start = zero_coupon_bond(a, b, sigma, r0, start, start);
    let p_end = zero_coupon_bond(a, b, sigma, r0, start, *payment_times.last().expect("un swap necesita al menos un periodo"));
    let numerator = p_start - p_end;

    let denominator = payment_times
        .iter()
        .zip(accruals.iter())
        .map(|(&ti, &tau)| zero_coupon_bond(a, b, sigma, r0, start, ti) * T::constant(tau))
        .fold(T::constant(0.0), |acc, leg| acc + leg);

    numerator / denominator
}

/// Valor y Hessiano 4x4 completo (10 pares) del NPV determinista de un IRS bajo Hull-White 1F,
/// PLAN_BACKWARD.md §9 Fase 2. `value`/gradiente salen de 1+4 evaluaciones `f64`/`Dual2`; las 6
/// entradas cruzadas salen de 6 evaluaciones `HyperDual` -- 11 evaluaciones del pricer cerrado en
/// total, cada una O(n_payments), muy por debajo de cualquier coste de Monte Carlo/bump-and-reval
/// de 10 pares independientes.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HullWhite1FHessian {
    pub value: f64,
    pub d_a: f64,
    pub d_b: f64,
    pub d_sigma: f64,
    pub d_r0: f64,
    pub d_aa: f64,
    pub d_bb: f64,
    pub d_sigmasigma: f64,
    pub d_r0r0: f64,
    pub d_ab: f64,
    pub d_asigma: f64,
    pub d_ar0: f64,
    pub d_bsigma: f64,
    pub d_br0: f64,
    pub d_sigmar0: f64,
}

#[allow(clippy::too_many_arguments)]
pub fn hull_white_1f_hessian(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    use_par_rate: bool,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
) -> HullWhite1FHessian {
    // Mismo patron que `build_irs_swap`/`irs_hull_white_npv_all_greeks` (api.rs): el tipo fijo se
    // fija UNA vez, en aritmetica f64 plana (sin derivar), antes de construir cualquier `Dual2`/
    // `HyperDual` -- el cupon de un swap ya emitido no depende de a/b/sigma/r0.
    let fixed_rate_f64 = if use_par_rate {
        par_rate::<f64>(a, b, sigma, r0, start, payment_times, accruals)
    } else {
        fixed_rate
    };

    let value = npv::<f64>(a, b, sigma, r0, notional, fixed_rate_f64, start, payment_times, accruals);

    // Gradiente + diagonal del Hessiano: 4 llamadas Dual2, una por parametro.
    let dual_a = npv::<Dual2>(
        Dual2::variable(a), Dual2::constant(b), Dual2::constant(sigma), Dual2::constant(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let dual_b = npv::<Dual2>(
        Dual2::constant(a), Dual2::variable(b), Dual2::constant(sigma), Dual2::constant(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let dual_sigma = npv::<Dual2>(
        Dual2::constant(a), Dual2::constant(b), Dual2::variable(sigma), Dual2::constant(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let dual_r0 = npv::<Dual2>(
        Dual2::constant(a), Dual2::constant(b), Dual2::constant(sigma), Dual2::variable(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );

    // Entradas cruzadas del Hessiano: 6 llamadas HyperDual, una por par distinto.
    let hyper_ab = npv::<HyperDual>(
        HyperDual::variable_x(a), HyperDual::variable_y(b), HyperDual::constant(sigma), HyperDual::constant(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let hyper_asigma = npv::<HyperDual>(
        HyperDual::variable_x(a), HyperDual::constant(b), HyperDual::variable_y(sigma), HyperDual::constant(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let hyper_ar0 = npv::<HyperDual>(
        HyperDual::variable_x(a), HyperDual::constant(b), HyperDual::constant(sigma), HyperDual::variable_y(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let hyper_bsigma = npv::<HyperDual>(
        HyperDual::constant(a), HyperDual::variable_x(b), HyperDual::variable_y(sigma), HyperDual::constant(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let hyper_br0 = npv::<HyperDual>(
        HyperDual::constant(a), HyperDual::variable_x(b), HyperDual::constant(sigma), HyperDual::variable_y(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );
    let hyper_sigmar0 = npv::<HyperDual>(
        HyperDual::constant(a), HyperDual::constant(b), HyperDual::variable_x(sigma), HyperDual::variable_y(r0),
        notional, fixed_rate_f64, start, payment_times, accruals,
    );

    HullWhite1FHessian {
        value,
        d_a: dual_a.d1,
        d_b: dual_b.d1,
        d_sigma: dual_sigma.d1,
        d_r0: dual_r0.d1,
        d_aa: dual_a.second_derivative(),
        d_bb: dual_b.second_derivative(),
        d_sigmasigma: dual_sigma.second_derivative(),
        d_r0r0: dual_r0.second_derivative(),
        d_ab: hyper_ab.dxy,
        d_asigma: hyper_asigma.dxy,
        d_ar0: hyper_ar0.dxy,
        d_bsigma: hyper_bsigma.dxy,
        d_br0: hyper_br0.dxy,
        d_sigmar0: hyper_sigmar0.dxy,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::api::irs_hull_white_npv_all_greeks;

    const H: f64 = 1e-6;

    fn central_diff<F: Fn(f64) -> f64>(f: F, x: f64) -> f64 {
        (f(x + H) - f(x - H)) / (2.0 * H)
    }

    fn central_second_diff<F: Fn(f64) -> f64>(f: F, x: f64, h: f64) -> f64 {
        (f(x + h) - 2.0 * f(x) + f(x - h)) / (h * h)
    }

    // ---------------------------------------------------------------------------------------
    // 1) Aritmetica Dual2/HyperDual vs diferencias finitas (PLAN_BACKWARD.md §10, primer paso
    //    obligatorio -- nunca confiar en una formula analitica de Mul/Div/exp/ln/powf sin
    //    verificarla numericamente antes de usarla en el pricer).
    // ---------------------------------------------------------------------------------------

    #[test]
    fn dual2_arithmetic_matches_finite_differences_for_a_representative_expression() {
        // f(x) = exp(x) * ln(x + 5.0) / (x - 8.0).powf(2.0) -- ejercita Mul/Sub/Add/Div/Exp/Ln/Powf
        // encadenados, en un punto lejos de cualquier singularidad (x+5 > 0, x-8 != 0).
        let f = |x: f64| x.exp() * (x + 5.0).ln() / (x - 8.0).powf(2.0);
        let x0 = 1.7;
        let h = 1e-3;

        let xd = Dual2::variable(x0);
        let result = xd.exp() * (xd + Dual2::constant(5.0)).ln() / (xd - Dual2::constant(8.0)).powf(Dual2::constant(2.0));

        assert!((result.value - f(x0)).abs() < 1e-10, "value={} expected={}", result.value, f(x0));

        let expected_d1 = central_diff(f, x0);
        assert!(
            (result.d1 - expected_d1).abs() < 1e-6,
            "d1={} expected={}",
            result.d1,
            expected_d1
        );

        let expected_d2 = central_second_diff(f, x0, h);
        let tol = 1e-3 * expected_d2.abs().max(1.0);
        assert!(
            (result.second_derivative() - expected_d2).abs() < tol,
            "f''={} expected={} tol={tol}",
            result.second_derivative(),
            expected_d2
        );
    }

    #[test]
    fn hyperdual_arithmetic_matches_finite_differences_for_a_representative_expression() {
        // g(x,y) = exp(x) * ln(y + 4.0) / (x + y + 2.0).powf(2.0) -- misma familia de operaciones
        // que el test de Dual2, con dos variables independientes para ejercitar dx/dy/dxy.
        let g = |x: f64, y: f64| x.exp() * (y + 4.0).ln() / (x + y + 2.0).powf(2.0);
        let (x0, y0) = (0.6, 0.9);
        let h = 1e-3;

        let xd = HyperDual::variable_x(x0);
        let yd = HyperDual::variable_y(y0);
        let result =
            xd.exp() * (yd + HyperDual::constant(4.0)).ln() / (xd + yd + HyperDual::constant(2.0)).powf(HyperDual::constant(2.0));

        assert!((result.value - g(x0, y0)).abs() < 1e-10, "value={} expected={}", result.value, g(x0, y0));

        let expected_dx = central_diff(|x| g(x, y0), x0);
        let expected_dy = central_diff(|y| g(x0, y), y0);
        assert!((result.dx - expected_dx).abs() < 1e-6, "dx={} expected={}", result.dx, expected_dx);
        assert!((result.dy - expected_dy).abs() < 1e-6, "dy={} expected={}", result.dy, expected_dy);

        // Estencil de 4 puntos para la derivada cruzada.
        let expected_dxy =
            (g(x0 + h, y0 + h) - g(x0 + h, y0 - h) - g(x0 - h, y0 + h) + g(x0 - h, y0 - h)) / (4.0 * h * h);
        let tol = 1e-3 * expected_dxy.abs().max(1.0);
        assert!(
            (result.dxy - expected_dxy).abs() < tol,
            "dxy={} expected={expected_dxy} tol={tol}",
            result.dxy
        );
    }

    // ---------------------------------------------------------------------------------------
    // 2) value/gradiente de hull_white_1f_hessian vs la ruta Burn EXISTENTE
    //    (irs_hull_white_npv_all_greeks) -- la comparacion MAS IMPORTANTE de esta fase
    //    (PLAN_BACKWARD.md §10).
    // ---------------------------------------------------------------------------------------

    struct Fixture {
        a: f64,
        b: f64,
        sigma: f64,
        r0: f64,
        notional: f64,
        fixed_rate: f64,
        use_par_rate: bool,
        start: f64,
        payment_times: Vec<f64>,
        accruals: Vec<f64>,
    }

    fn fixtures() -> Vec<Fixture> {
        vec![
            Fixture {
                a: 0.1, b: 0.03, sigma: 0.01, r0: 0.02,
                notional: 1_000_000.0, fixed_rate: 0.02, use_par_rate: false, start: 0.0,
                payment_times: vec![1.0, 2.0, 3.0, 4.0, 5.0], accruals: vec![1.0; 5],
            },
            Fixture {
                a: 0.1, b: 0.03, sigma: 0.01, r0: 0.02,
                notional: 1_000_000.0, fixed_rate: 0.0, use_par_rate: true, start: 0.0,
                payment_times: vec![1.0, 2.0, 3.0, 4.0, 5.0], accruals: vec![1.0; 5],
            },
            Fixture {
                a: 0.25, b: 0.045, sigma: 0.015, r0: 0.018,
                notional: 2_500_000.0, fixed_rate: 0.03, use_par_rate: false, start: 0.0,
                payment_times: vec![0.5, 1.0, 1.5, 2.0], accruals: vec![0.5; 4],
            },
        ]
    }

    #[test]
    fn value_and_gradient_match_the_burn_autodiff_route() {
        for (idx, fx) in fixtures().into_iter().enumerate() {
            let hessian = hull_white_1f_hessian(
                fx.a, fx.b, fx.sigma, fx.r0, fx.notional, fx.fixed_rate, fx.use_par_rate, fx.start,
                &fx.payment_times, &fx.accruals,
            );
            let burn = irs_hull_white_npv_all_greeks(
                fx.a, fx.b, fx.sigma, fx.r0, fx.notional, fx.fixed_rate, fx.use_par_rate, fx.start,
                fx.payment_times.clone(), fx.accruals.clone(),
            );
            let burn_value = crate::api::irs_hull_white_npv(
                fx.a, fx.b, fx.sigma, fx.r0, fx.notional, fx.fixed_rate, fx.use_par_rate, fx.start,
                fx.payment_times.clone(), fx.accruals.clone(),
            );

            let rel_tol = |reference: f64| 1e-8 * reference.abs().max(1.0);

            assert!(
                (hessian.value - burn_value).abs() < rel_tol(burn_value),
                "fixture {idx}: value={} burn={burn_value}",
                hessian.value
            );
            assert!(
                (hessian.d_a - burn.d_a).abs() < rel_tol(burn.d_a),
                "fixture {idx}: d_a={} burn={}",
                hessian.d_a, burn.d_a
            );
            assert!(
                (hessian.d_b - burn.d_b).abs() < rel_tol(burn.d_b),
                "fixture {idx}: d_b={} burn={}",
                hessian.d_b, burn.d_b
            );
            assert!(
                (hessian.d_sigma - burn.d_sigma).abs() < rel_tol(burn.d_sigma),
                "fixture {idx}: d_sigma={} burn={}",
                hessian.d_sigma, burn.d_sigma
            );
            assert!(
                (hessian.d_r0 - burn.d_r0).abs() < rel_tol(burn.d_r0),
                "fixture {idx}: d_r0={} burn={}",
                hessian.d_r0, burn.d_r0
            );
        }
    }

    // ---------------------------------------------------------------------------------------
    // 3) Consistencia interna: el gradiente que dan las 6 llamadas HyperDual (dx/dy) coincide
    //    con el de las 4 llamadas Dual2 (d1), calculado por un camino distinto.
    // ---------------------------------------------------------------------------------------

    #[test]
    fn hyperdual_gradient_components_are_consistent_with_dual2_gradient() {
        let fx = &fixtures()[0];
        let hessian = hull_white_1f_hessian(
            fx.a, fx.b, fx.sigma, fx.r0, fx.notional, fx.fixed_rate, fx.use_par_rate, fx.start,
            &fx.payment_times, &fx.accruals,
        );

        // Reconstruye dx/dy de cada llamada HyperDual usada internamente y compara contra el
        // gradiente ya expuesto (d_a/d_b/d_sigma/d_r0) -- llamadas equivalentes, camino distinto.
        let hyper_ab = npv::<HyperDual>(
            HyperDual::variable_x(fx.a), HyperDual::variable_y(fx.b), HyperDual::constant(fx.sigma),
            HyperDual::constant(fx.r0), fx.notional,
            if fx.use_par_rate { par_rate::<f64>(fx.a, fx.b, fx.sigma, fx.r0, fx.start, &fx.payment_times, &fx.accruals) } else { fx.fixed_rate },
            fx.start, &fx.payment_times, &fx.accruals,
        );
        assert!((hyper_ab.dx - hessian.d_a).abs() < 1e-9, "dx={} d_a={}", hyper_ab.dx, hessian.d_a);
        assert!((hyper_ab.dy - hessian.d_b).abs() < 1e-9, "dy={} d_b={}", hyper_ab.dy, hessian.d_b);

        let hyper_sigmar0 = npv::<HyperDual>(
            HyperDual::constant(fx.a), HyperDual::constant(fx.b), HyperDual::variable_x(fx.sigma),
            HyperDual::variable_y(fx.r0), fx.notional,
            if fx.use_par_rate { par_rate::<f64>(fx.a, fx.b, fx.sigma, fx.r0, fx.start, &fx.payment_times, &fx.accruals) } else { fx.fixed_rate },
            fx.start, &fx.payment_times, &fx.accruals,
        );
        assert!(
            (hyper_sigmar0.dx - hessian.d_sigma).abs() < 1e-9,
            "dx={} d_sigma={}", hyper_sigmar0.dx, hessian.d_sigma
        );
        assert!(
            (hyper_sigmar0.dy - hessian.d_r0).abs() < 1e-9,
            "dy={} d_r0={}", hyper_sigmar0.dy, hessian.d_r0
        );
    }

    // ---------------------------------------------------------------------------------------
    // 4) Las 10 entradas del Hessiano vs un estencil de bump-and-reval de segundo orden aplicado
    //    directamente sobre npv::<f64> -- oraculo independiente (PLAN_BACKWARD.md §10).
    // ---------------------------------------------------------------------------------------

    // El tipo fijo se fija UNA VEZ con los parametros ORIGINALES del fixture (igual que
    // `hull_white_1f_hessian`/`build_irs_swap`: el cupon de un swap ya emitido no se recalcula
    // cuando se shockea a/b/sigma/r0 para medir una sensibilidad/Hessiano) -- recalcularlo en
    // cada bump derivaria una funcion distinta (NPV de un swap que se re-emite a la par en cada
    // escenario), no la misma que diferencia `hull_white_1f_hessian`.
    fn npv_f64(fx: &Fixture, fixed_rate_f64: f64, a: f64, b: f64, sigma: f64, r0: f64) -> f64 {
        npv::<f64>(a, b, sigma, r0, fx.notional, fixed_rate_f64, fx.start, &fx.payment_times, &fx.accruals)
    }

    fn fixed_rate_of(fx: &Fixture) -> f64 {
        if fx.use_par_rate {
            par_rate::<f64>(fx.a, fx.b, fx.sigma, fx.r0, fx.start, &fx.payment_times, &fx.accruals)
        } else {
            fx.fixed_rate
        }
    }

    #[test]
    fn hessian_diagonal_matches_a_three_point_bump_and_reval_stencil() {
        for (idx, fx) in fixtures().into_iter().enumerate() {
            let hessian = hull_white_1f_hessian(
                fx.a, fx.b, fx.sigma, fx.r0, fx.notional, fx.fixed_rate, fx.use_par_rate, fx.start,
                &fx.payment_times, &fx.accruals,
            );
            let fixed_rate_f64 = fixed_rate_of(&fx);

            let h_a = 1e-4 * fx.a.abs().max(1.0);
            let h_b = 1e-4 * fx.b.abs().max(1.0);
            let h_sigma = 1e-4 * fx.sigma.abs().max(1.0);
            let h_r0 = 1e-4 * fx.r0.abs().max(1.0);

            let bump_aa = (npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b, fx.sigma, fx.r0)
                - 2.0 * npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b, fx.sigma, fx.r0))
                / (h_a * h_a);
            let bump_bb = (npv_f64(&fx, fixed_rate_f64, fx.a, fx.b + h_b, fx.sigma, fx.r0)
                - 2.0 * npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a, fx.b - h_b, fx.sigma, fx.r0))
                / (h_b * h_b);
            let bump_sigmasigma = (npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma + h_sigma, fx.r0)
                - 2.0 * npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma - h_sigma, fx.r0))
                / (h_sigma * h_sigma);
            let bump_r0r0 = (npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma, fx.r0 + h_r0)
                - 2.0 * npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma, fx.r0 - h_r0))
                / (h_r0 * h_r0);

            let tol = |reference: f64| 1e-3 * reference.abs().max(1.0);
            assert!(
                (hessian.d_aa - bump_aa).abs() < tol(bump_aa),
                "fixture {idx}: d_aa={} bump={bump_aa}",
                hessian.d_aa
            );
            assert!(
                (hessian.d_bb - bump_bb).abs() < tol(bump_bb),
                "fixture {idx}: d_bb={} bump={bump_bb}",
                hessian.d_bb
            );
            assert!(
                (hessian.d_sigmasigma - bump_sigmasigma).abs() < tol(bump_sigmasigma),
                "fixture {idx}: d_sigmasigma={} bump={bump_sigmasigma}",
                hessian.d_sigmasigma
            );
            assert!(
                (hessian.d_r0r0 - bump_r0r0).abs() < tol(bump_r0r0),
                "fixture {idx}: d_r0r0={} bump={bump_r0r0}",
                hessian.d_r0r0
            );
        }
    }

    #[test]
    fn hessian_cross_terms_match_a_four_point_bump_and_reval_stencil() {
        for (idx, fx) in fixtures().into_iter().enumerate() {
            let hessian = hull_white_1f_hessian(
                fx.a, fx.b, fx.sigma, fx.r0, fx.notional, fx.fixed_rate, fx.use_par_rate, fx.start,
                &fx.payment_times, &fx.accruals,
            );
            let fixed_rate_f64 = fixed_rate_of(&fx);

            let h_a = 1e-4 * fx.a.abs().max(1.0);
            let h_b = 1e-4 * fx.b.abs().max(1.0);
            let h_sigma = 1e-4 * fx.sigma.abs().max(1.0);
            let h_r0 = 1e-4 * fx.r0.abs().max(1.0);

            let cross_ab = (npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b + h_b, fx.sigma, fx.r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b - h_b, fx.sigma, fx.r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b + h_b, fx.sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b - h_b, fx.sigma, fx.r0))
                / (4.0 * h_a * h_b);
            let cross_asigma = (npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b, fx.sigma + h_sigma, fx.r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b, fx.sigma - h_sigma, fx.r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b, fx.sigma + h_sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b, fx.sigma - h_sigma, fx.r0))
                / (4.0 * h_a * h_sigma);
            let cross_ar0 = (npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b, fx.sigma, fx.r0 + h_r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a + h_a, fx.b, fx.sigma, fx.r0 - h_r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b, fx.sigma, fx.r0 + h_r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a - h_a, fx.b, fx.sigma, fx.r0 - h_r0))
                / (4.0 * h_a * h_r0);
            let cross_bsigma = (npv_f64(&fx, fixed_rate_f64, fx.a, fx.b + h_b, fx.sigma + h_sigma, fx.r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a, fx.b + h_b, fx.sigma - h_sigma, fx.r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a, fx.b - h_b, fx.sigma + h_sigma, fx.r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a, fx.b - h_b, fx.sigma - h_sigma, fx.r0))
                / (4.0 * h_b * h_sigma);
            let cross_br0 = (npv_f64(&fx, fixed_rate_f64, fx.a, fx.b + h_b, fx.sigma, fx.r0 + h_r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a, fx.b + h_b, fx.sigma, fx.r0 - h_r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a, fx.b - h_b, fx.sigma, fx.r0 + h_r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a, fx.b - h_b, fx.sigma, fx.r0 - h_r0))
                / (4.0 * h_b * h_r0);
            let cross_sigmar0 = (npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma + h_sigma, fx.r0 + h_r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma + h_sigma, fx.r0 - h_r0)
                - npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma - h_sigma, fx.r0 + h_r0)
                + npv_f64(&fx, fixed_rate_f64, fx.a, fx.b, fx.sigma - h_sigma, fx.r0 - h_r0))
                / (4.0 * h_sigma * h_r0);

            let tol = |reference: f64| 1e-3 * reference.abs().max(1.0);
            assert!((hessian.d_ab - cross_ab).abs() < tol(cross_ab), "fixture {idx}: d_ab={} bump={cross_ab}", hessian.d_ab);
            assert!(
                (hessian.d_asigma - cross_asigma).abs() < tol(cross_asigma),
                "fixture {idx}: d_asigma={} bump={cross_asigma}",
                hessian.d_asigma
            );
            assert!((hessian.d_ar0 - cross_ar0).abs() < tol(cross_ar0), "fixture {idx}: d_ar0={} bump={cross_ar0}", hessian.d_ar0);
            assert!(
                (hessian.d_bsigma - cross_bsigma).abs() < tol(cross_bsigma),
                "fixture {idx}: d_bsigma={} bump={cross_bsigma}",
                hessian.d_bsigma
            );
            assert!(
                (hessian.d_br0 - cross_br0).abs() < tol(cross_br0),
                "fixture {idx}: d_br0={} bump={cross_br0}",
                hessian.d_br0
            );
            assert!(
                (hessian.d_sigmar0 - cross_sigmar0).abs() < tol(cross_sigmar0),
                "fixture {idx}: d_sigmar0={} bump={cross_sigmar0}",
                hessian.d_sigmar0
            );
        }
    }
}
