# PLAN_HYPERDUAL.md — torre de números duales genérica para pathwise de orden ≥ 2

> Documento de arquitectura y plan de implementación. Complementa a
> [PLAN_GREEKS.md](PLAN_GREEKS.md) (que resuelve "cualquier griega de cualquier métrica" y, para
> orden 2/cruzadas, se apoya SIEMPRE en bump-and-reval — ver su §5.2/§15: "AAD de segundo orden...
> fuera de alcance") extendiendo la ruta **pathwise** (`rust/crates/engine-core/src/payoff/dual.rs`)
> de un único número dual de primer orden a una **familia genérica** de números duales truncados,
> capaz de servir Delta/Vega/Rho (orden 1), Gamma/Volga (orden 2 puro) y Vanna/cross-gamma (orden 1
> cruzado) sin bump-and-reval — con la propiedad de que el coste extra de un orden más alto **solo
> se paga cuando ese orden se pide**, nunca de forma ambient. Este documento no declara
> implementadas las fases: cada una se migra a `PLAN.md` únicamente después de quedar construida y
> verificada de extremo a extremo, mismo criterio editorial que PLAN_PRODUCTS.md/PLAN_GREEKS.md.

## 0. Decisión ejecutiva

`rust/crates/engine-core/src/payoff/dual.rs::Dual` es hoy un struct concreto de dos `f64`
(`value`, `deriv`) que representa un número dual de primer orden (`v + d·ε`, `ε² = 0`). Es exacto
para Delta/Vega/Rho/DV01-tipo-modelo, pero **no generaliza**: no hay forma de pedirle una segunda
derivada ni una cruzada sin escribir, a mano, un tipo distinto y — peor — un intérprete de payoff
distinto, porque `eval_scalar_dual`/`eval_contract_dual` (`sensitivity.rs`) son hoy un **espejo
duplicado a mano** de `eval::eval_scalar`/`eval::eval_contract`, cada uno cableado al tipo numérico
concreto que evalúa (`f64` uno, `Dual` el otro — ver el doc-comment de `eval_scalar_dual`: "espejo
exacto de `eval::eval_scalar`... ver el doc-comment del módulo para el porqué de esta duplicación
deliberadamente pequeña en vez de generalizar `eval::eval_scalar` sobre un tipo numérico").

La decisión central de este documento es exactamente esa generalización que `sensitivity.rs`
pospuso deliberadamente: sustituir "un tipo concreto por intérprete duplicado" por **un trait
`DualNumber` pequeño** (las operaciones que `ScalarOp` realmente usa) implementado por una
**familia cerrada de structs concretos** — `Dual` (orden 1, sin cambios de comportamiento),
`Dual2` (orden 2 puro, nuevo), `HyperDual` (orden 1 cruzado — Vanna/cross-gamma, nuevo) y,
documentado pero no obligatoriamente implementado en la primera tanda, `Dual3`/`HyperDual2` (orden
3) — con el intérprete de payoff (`eval_scalar_dual`/`eval_contract_dual`) reescrito **una sola
vez**, genérico sobre ese trait, en vez de una copia por tipo.

**La propiedad que motiva el documento** (pregunta original: "¿cuesta esto algo si no lo pido?"):
NO. Rust monomorphiza genéricos — `eval_scalar_dual::<Dual>` y `eval_scalar_dual::<HyperDual>` son,
tras compilar, dos funciones de máquina **distintas y completas**, cada una con el tamaño de
struct y el número de multiplicaciones exactas de SU tipo, sin rastro del otro. El motor de Greeks
(`engine::greeks::compute_greek`, ver PLAN_GREEKS.md §5) ya decide en cada llamada, a partir de
`GreekRequest::order`, qué ruta tomar — extenderlo a elegir `Dual` vs `Dual2` vs `HyperDual` es
literalmente el mismo `match` que hoy elige `try_pathwise` vs `try_aad_reverse` vs bump-and-reval
(§5.4 de PLAN_GREEKS.md), no un mecanismo nuevo. Dicho de otro modo: **el tipo se genera en tiempo
de compilación (monomorphización), la selección de CUÁL usar ocurre en tiempo de ejecución** según
lo que pida `GreekRequest` — nunca se "genera código sobre la marcha" en sentido literal (eso
sería un JIT, fuera de alcance y sin sentido aquí), y nunca se paga el coste de `HyperDual` al
pedir una Delta con `Dual`.

### 0.1 Por qué crece el coste con el orden (y cuánto, con números reales)

Un número dual truncado de orden `d` en una única dirección es un polinomio truncado
`v + a₁·ε + a₂·ε² + ... + a_d·ε^d` (con `ε^{d+1} = 0`); una segunda dirección independiente para
capturar una derivada CRUZADA añade un segundo símbolo `η` (`η²=0`) y el término mixto `ε·η`.
Multiplicar dos de estos números es un **producto de Cauchy truncado**: cada componente de salida
`cₖ` necesita `k+1` multiplicaciones (`cₖ = Σᵢ aᵢ·b_{k-i}`), así que multiplicar dos números con
`n` componentes cuesta `Σₖ₌₀ⁿ⁻¹ (k+1) = n(n+1)/2` multiplicaciones — **crece con el cuadrado del
número de componentes**, no linealmente con el orden pedido:

| Estructura | Componentes | Representa | Multiplicaciones por `*` | Uso |
|---|---:|---|---:|---|
| `f64` | 1 | solo valor | 1 | valoración pura (intérprete `eval::eval_scalar` existente, sin cambios) |
| `Dual` (existente) | 2 | `v, d/dx` | 3 | Delta/Vega/Rho/DV01-tipo-modelo (orden 1) — **sin cambios** |
| `Dual2` (nuevo) | 3 | `v, d/dx, d²/dx²` | 6 | Gamma/Volga (orden 2 puro, una sola dirección) |
| `HyperDual` (nuevo) | 4 | `v, d/dx, d/dy, d²/dxdy` | ~9 | Vanna/cross-gamma (orden 1 cruzado, dos direcciones) |
| `Dual3` (documentado, fase opcional) | 4 | `v, d/dx, d²/dx², d³/dx³` | 10 | Speed (orden 3 puro) — mismo número de componentes que `HyperDual`, coste de multiplicación ligeramente distinto porque no hay término cruzado que anular |

(`HyperDual` con `ε²=η²=0` tiene 4 componentes pero solo 4 productos "sobreviven" el truncamiento
por dirección, no los `4·5/2=10` de un polinomio denso de 4 términos — de ahí el `~9` en vez de
`10`; `Dual3` sí es un polinomio denso de 4 términos en una sola variable, de ahí el `10` exacto.
Ambos números confirman la misma tendencia: **cada orden adicional (más componentes) es más caro
que el anterior**, y el motor solo instancia — y por tanto solo paga — el que el `GreekRequest`
concreto pide.)

## 1. Estado actual (puntos de integración)

- `rust/crates/engine-core/src/payoff/dual.rs::Dual` — struct concreto de 2 `f64`, aritmética vía
  `std::ops::{Add,Sub,Mul,Div,Neg}` y métodos propios (`abs`, `exp`, `ln`, `powf`, `min`, `max`),
  sin ningún trait que lo abstraiga. Usado por 4 parámetros de `Gbm` y 3 de `GbmP`
  (`GbmGreek`/`GbmPGreek`, `sensitivity.rs`).
- `rust/crates/engine-core/src/payoff/sensitivity.rs` — **intérprete duplicado a mano**:
  `eval_scalar_dual`/`eval_contract_dual` repiten nodo a nodo `eval::eval_scalar`/
  `eval::eval_contract` (mismos 16 `ScalarOp` — `Constant`, `Fixing`, `Current`, `Add`, `Sub`,
  `Mul`, `Div`, `Neg`, `Abs`, `Exp`, `Log`, `Pow`, `Min`, `Max`, `Clamp`, `EventValue` — y los
  mismos `ContractOp`), cableados a `Dual` en la firma (`-> Dual`, `&dyn DualObservablePath` con
  `fn value_at(...) -> Dual`). `GbmDualPath::param_duals()` decide, con un único `match` sobre
  `GbmGreek`, cuál de los 4 parámetros recibe `Dual::variable` (deriv=1) y cuáles `Dual::constant`
  (deriv=0) — la ruta simulada `W_t` en sí queda fija (`f64`, recuperada de la ruta ya simulada, ver
  el doc-comment del módulo).
- `rust/crates/engine-core/src/payoff/api.rs`/`api_p.rs::payoff_sensitivity_gbm[_p]` — capa pública
  que decide bump-and-reval vs pathwise según `contains_exercise` y llama a `evaluate_dual`.
- Puente cxx (`rust/crates/engine-ffi`) expone `payoff_sensitivity_gbm_q`/`_p` a C++.
- `cpp/engine/src/greeks.cpp::try_pathwise` + `pathwise_capabilities()` — tabla de capacidades
  (PLAN_GREEKS.md §5.4) que hoy solo lista `(GBM, PayoffPriceQ)`/`(GBM_P, PayoffForecastP)`, y solo
  se consulta cuando `request.order.order == 1 && !cross_factor` (ver el guard en
  `compute_greek`, greeks.cpp:660-681). Pedir Gamma (`order=2`) o Vanna (`cross_factor` presente)
  sobre estos mismos (modelo, métrica) hoy SIEMPRE cae a bump-and-reval — correcto (PLAN_GREEKS.md
  §5.2: "Gamma/derivadas cruzadas se sirven por bump-and-reval... AAD de segundo orden fuera de
  alcance"), pero deja sobre la mesa exactamente la ganancia que este documento persigue: una
  Gamma/Vanna pathwise EXACTA (sin ruido de bump finito) para GBM/GBM_P, con el mismo coste de UNA
  pasada por la ruta ya simulada que hoy tiene Delta.

## 2. Arquitectura objetivo

```text
                         ScalarOp (16 nodos, ya existentes, sin cambios)
                                        |
                                        v
                    eval_scalar<T: DualNumber>(payoff, idx, cursor, path, states) -> T
                    eval_contract<T: DualNumber>(...)              (UNA sola implementacion,
                    ObservablePath<T: DualNumber> { fn value_at(...) -> T }   generica)
                                        |
                    +-------------------+-------------------+------------------------+
                    |                   |                   |                        |
                    v                   v                   v                        v
                  Dual               Dual2             HyperDual              Dual3 (opcional,
              (orden 1, 2 comp,   (orden 2 puro,    (orden 1 cruzado,          fase 4, 4 comp,
               3 mults/mul,        3 comp,           4 comp, ~9 mults/mul,      10 mults/mul,
               SIN CAMBIOS de      6 mults/mul,       Vanna/cross-gamma)        Speed)
               comportamiento)     Gamma/Volga)

                                        |
                                        v
        engine::greeks::compute_greek (PLAN_GREEKS.md §5): mismo `match` que hoy elige
        try_pathwise/try_aad_reverse/bump_and_reval, ampliado con dos entradas nuevas de tabla
        de capacidades: pathwise2_capabilities() (order=2 puro) y pathwise_cross_capabilities()
        (order=1 con cross_factor) -- misma disciplina de verificacion obligatoria (PLAN_GREEKS.md
        §5.3): sin test diferencial contra bump-and-reval en verde, la combinacion no se sirve
        bajo method=Auto.
```

Tres piezas, mismo espíritu de separación que PLAN_GREEKS.md §2 (catálogo / motor genérico /
especializaciones verificadas):

1. **`DualNumber`**: trait mínimo, las operaciones que `ScalarOp` usa — no más.
2. **Intérprete genérico**: `eval_scalar`/`eval_contract`/`ObservablePath` parametrizados sobre
   `T: DualNumber`, UNA sola vez, en vez de un intérprete completo por tipo.
3. **Familia de tipos concretos** (`Dual`/`Dual2`/`HyperDual`/...): cada uno implementa el trait con
   su propia aritmética de producto de Cauchy truncado — el coste de cada uno es el propio, nunca
   el de otro.

## 3. Modelo de dominio

### 3.1 El trait `DualNumber`

```rust
// rust/crates/engine-core/src/payoff/dual.rs (ampliado, no en un modulo nuevo -- sigue siendo
// "el numero dual de payoff", ahora como familia en vez de un tipo unico)
pub(crate) trait DualNumber:
    Copy + Add<Output = Self> + Sub<Output = Self> + Mul<Output = Self> + Div<Output = Self> + Neg<Output = Self>
{
    /// Valor base (`f64`), sin derivada -- lo que hoy es el campo publico `.value` de `Dual`.
    fn re(self) -> f64;
    /// Constante: todas las componentes de derivada a cero (equivalente de `Dual::constant`).
    fn constant(value: f64) -> Self;

    // Las mismas 6 operaciones que `Dual` ya implementa a mano (dual.rs:38-88) -- ni una mas:
    // `ScalarOp` no tiene mas nodos que `Add/Sub/Mul/Div/Neg` (via std::ops) + estas 6.
    fn abs(self) -> Self;
    fn exp(self) -> Self;
    fn ln(self) -> Self;
    fn powf(self, other: Self) -> Self;
    fn min(self, other: Self) -> Self;
    fn max(self, other: Self) -> Self;
}
```

`Dual::variable(value)` (seedear la dirección que se deriva con derivada 1) **no** entra en el
trait: cada tipo tiene una noción distinta de "qué dirección seedear" (`Dual` una, `HyperDual` dos
posibles, `Dual2` una pero de orden 2) — se queda como constructor propio de cada struct, igual que
hoy, y lo decide `GbmDualPath::param_duals()`/su generalización (§3.4), no el trait.

### 3.2 `Dual` (orden 1) — sin cambios de comportamiento

`dual.rs::Dual` pasa a implementar `DualNumber` (impl mecánica, delega en los métodos que ya
tiene); el struct, sus campos públicos-de-crate (`value`→expuesto via `re()`, `deriv`) y toda su
aritmética **no cambian una línea de comportamiento** — mismos tests de `dual.rs` en verde sin
tocar sus aserciones (criterio de aceptación de la Fase 1, §7).

### 3.3 `Dual2` (orden 2 puro) — nuevo

```rust
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct Dual2 {
    pub(crate) value: f64,
    pub(crate) d1: f64,   // primera derivada
    pub(crate) d2: f64,   // segunda derivada (NO dividida por 2! -- ver nota de convencion abajo)
}
```

Convención (fijada aquí, ADR §7 Fase 0, para no repetir el error clásico de "¿es `f''` o `f''/2`"):
`Dual2` representa el polinomio de Taylor truncado `v + d1·ε + d2·ε²` de la serie
`f(x+ε) = f(x) + f'(x)·ε + f''(x)/2!·ε² + ...` -- **`d2` guarda el coeficiente `f''(x)/2`, no
`f''(x)` directamente** (igual que un desarrollo de Taylor estándar), y `Dual2::second_derivative()`
devuelve `2.0 * self.d2` para quien quiera la derivada real. Esta es la convención que hace que el
producto de Cauchy truncado (`(a0+a1ε+a2ε²)(b0+b1ε+b2ε²) mod ε³`) sea EXACTAMENTE la composición de
Taylor sin factores de corrección adicionales en cada operación intermedia (`exp`, `ln`, `powf`
también se derivan sobre esta misma convención, igual que hoy `Dual::exp`/`Dual::ln` ya asumen
`deriv` sin normalizar). Multiplicación (6 multiplicaciones, producto de Cauchy):

```rust
impl Mul for Dual2 {
    type Output = Dual2;
    fn mul(self, rhs: Dual2) -> Dual2 {
        Dual2 {
            value: self.value * rhs.value,
            d1: self.d1 * rhs.value + self.value * rhs.d1,
            d2: self.d2 * rhs.value + self.d1 * rhs.d1 + self.value * rhs.d2,
        }
    }
}
```

### 3.4 `HyperDual` (orden 1 cruzado) — nuevo

```rust
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct HyperDual {
    pub(crate) value: f64,
    pub(crate) dx: f64,     // d/dx
    pub(crate) dy: f64,     // d/dy
    pub(crate) dxy: f64,    // d^2/dxdy (mixta, SIN dividir -- misma convencion que Dual2.d2)
}
```

Álgebra `ε²=0`, `η²=0`, `ε·η` sí sobrevive (es el término que se busca). Multiplicación (los
términos `ε²·(...)` y `η²·(...)` se anulan solos, quedan 4 componentes de salida con 1+2+2+4=9
multiplicaciones):

```rust
impl Mul for HyperDual {
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
```

`HyperDual::variable_x(value)` / `::variable_y(value)` / `::constant(value)` como constructores
propios (no en el trait, §3.1): seedear `dx=1` para la dirección primaria (el `RiskFactor` de
`GreekRequest`) y `dy=1` para la secundaria (`GreekOrder::cross_factor`).

### 3.5 `Dual3` (orden 3 puro) — documentado, no obligatorio en la primera tanda

Mismo patrón que `Dual2` con una componente más (`d1, d2, d3`, convención de Taylor `f'''(x)/3!`
para `d3`), producto de Cauchy con 4 componentes → 10 multiplicaciones (§0.1). Se documenta aquí
para que la Fase 4 (§7) no tenga que rediseñar nada, no porque haya una demanda concreta hoy — ver
§9 "Fuera de alcance" para el criterio de cuándo merece la pena construirla.

## 4. Intérprete genérico

`eval_scalar_dual`/`eval_contract_dual`/`DualObservablePath` (`sensitivity.rs`) se generalizan a
`eval_scalar<T: DualNumber>`/`eval_contract<T: DualNumber>`/`ObservablePathT<T: DualNumber>` — el
CUERPO de la función no cambia ni una rama (sigue siendo el mismo `match` sobre los 16 `ScalarOp`,
con `Dual::constant`→`T::constant`, `.abs()`/`.exp()`/... ya vienen del trait sin cambiar el sitio
de la llamada). Se elimina la duplicación: donde hoy hay un intérprete para `f64`
(`eval::eval_scalar`) y uno para `Dual` (`sensitivity::eval_scalar_dual`), queda **un único**
`eval_scalar<T>` genérico reutilizado con `T=Dual`, `T=Dual2`, `T=HyperDual`; el intérprete de
`f64` (`eval::eval_scalar`, camino de valoración "normal", sin sensibilidad) se deja **intacto y
separado** — unificarlo también bajo `T: DualNumber` (haciendo `f64: DualNumber` trivial) es un
*stretch goal* opcional (§9), no un requisito: `eval::eval_scalar` es la ruta más caliente del
motor (se ejecuta una vez por paso de CADA ruta simulada) y tocarla sin necesidad real es el tipo
de riesgo que PLAN_PRODUCTS.md/PLAN_GREEKS.md evitan sistemáticamente ("aditivo, sin romper nada").

`GbmDualPath`/`GbmGreek` se generalizan de forma paralela y mínima:

- `GbmDualPath<T: DualNumber>` en vez de fijo a `Dual` — mismo cuerpo (`value_at` reconstruye
  `S_t(param)` con la misma fórmula cerrada de GBM), parametrizado.
- `GbmDualPath::param_duals() -> (T, T, T, T)` decide, según `GreekOrder`/`cross_factor` (no solo
  `GbmGreek`), qué construir: para `T=Dual`, exactamente como hoy (`variable`/`constant`); para
  `T=Dual2`, `variable` en la única dirección pedida; para `T=HyperDual`, `variable_x` en
  `risk_factor` y `variable_y` en `cross_factor`.

## 5. Selección en runtime (compila una vez por tipo, elige en cada llamada)

`engine::greeks::compute_greek` (`cpp/engine/src/greeks.cpp`) ya tiene el punto de decisión
correcto — el bloque que hoy (PLAN_GREEKS.md §11 Fase 7) comprueba `order.order==1 &&
!cross_factor` antes de intentar `try_pathwise`/`try_aad_reverse` (greeks.cpp:660-681). Se amplía
con dos ramas nuevas, mismo patrón exacto:

```text
si order == 2 y sin cross_factor y risk_factor.kind == ModelParameter:
    si method in {Auto, Pathwise} y (modelo, metrica) esta en pathwise2_capabilities():
        intentar try_pathwise2 (Dual2, via payoff_sensitivity2_gbm_q/_p del bridge cxx)
        # mismo fallback que hoy: Auto cae a bump-and-reval si no aplica; Pathwise explicito
        # sobre una combinacion no verificada es error inmediato (PLAN_GREEKS.md §5.4)

si order == 1 y cross_factor presente y ambos risk_factor/cross_factor.kind == ModelParameter:
    si method in {Auto, Pathwise} y (modelo, metrica) esta en pathwise_cross_capabilities():
        intentar try_pathwise_cross (HyperDual, via payoff_sensitivity_cross_gbm_q/_p)
```

Esto es exactamente el mismo `match`/tabla de capacidades que ya existe (§5.4 de PLAN_GREEKS.md),
ampliado con dos entradas — **no** un mecanismo de selección nuevo. La "generación en runtime" que
motivó la pregunta original es, con precisión: el compilador genera `try_pathwise2::<Dual2>` y
`try_pathwise_cross::<HyperDual>` como código de máquina en tiempo de compilación (monomorphización
de Rust); `compute_greek`, en tiempo de EJECUCIÓN, decide cuál de esas funciones ya compiladas
llamar según `GreekRequest`. Nunca se genera código nuevo al vuelo (eso sería un JIT — fuera de
alcance, sin motivo aquí) y nunca se ejecuta el camino de un tipo que no se pidió.

### 5.1 Guard rails heredados de PLAN_GREEKS.md (sin relajar ninguno)

- Un contrato con `ContractOp::Exercise` sigue cayendo a bump-and-reval para `Dual2`/`HyperDual`,
  exactamente por la misma razón que hoy lo hace para `Dual` (PLAN_GREEKS.md §5.1: re-decidir
  Longstaff-Schwartz bajo el parámetro perturbado no es pathwise-diferenciable) — `try_pathwise2`/
  `try_pathwise_cross` comparten el mismo chequeo `payoff_contains_exercise` que `try_pathwise`.
- Una métrica de tipo indicador/probabilidad (`PayoffHitProbabilityQ/P`) NUNCA declara soporte
  pathwise en ningún orden — la derivada pathwise exacta de una función indicador sigue siendo 0 en
  casi todo punto en la segunda derivada tanto como en la primera (PLAN_GREEKS.md §4.5). No cambia.
- Hull-White/Burn: este documento **no** toca la ruta AAD reverse-mode (`irs_hull_white_npv_all_greeks`).
  Burn no expone Hessiano de tensores de forma directa (PLAN_GREEKS.md §5.2/§15); `Dual2`/
  `HyperDual` son un mecanismo del intérprete de PAYOFF (`ScalarOp`), no de los tensores de
  Hull-White — Gamma/Vanna de Hull-White siguen sirviéndose por bump-and-reval, sin cambios.

## 6. Verificación obligatoria

Misma disciplina que PLAN_GREEKS.md §5.3: cada combinación (modelo, métrica, orden) que se active
bajo `method=Auto` necesita, antes de mezclarla, un test diferencial contra bump-and-reval con
tolerancia declarada:

- `Dual2` (Gamma de una call europea bajo GBM) contra `GreeksFase6Test.GammaOfACallMatchesClosedFormSecondDerivative`
  (ya existe como oráculo — comparar `Dual2` contra Black-Scholes cerrado Y contra bump-and-reval a
  la vez, triple verificación).
- `HyperDual` (Vanna de una call europea bajo GBM) contra `GreeksFase6Test.VannaOfACallHasExpectedSignAndMatchesClosedFormMixedFiniteDifference`.
- Sin ese test en verde, la combinación se sirve solo bajo `method` explícito, nunca como default
  de `Auto` (mismo criterio exacto que ya aplica a Fase 7 de PLAN_GREEKS.md).

## 7. Plan por fases

Cada fase termina con build limpio, tests unitarios y de integración; no se encadenan fases sin un
resultado verificable (mismo criterio que PLAN_GREEKS.md/PLAN_PRODUCTS.md).

### Fase 0 — ADRs y semántica congelada — DONE

- fijar el trait `DualNumber` (§3.1: exactamente las operaciones que `ScalarOp` usa, ni una más);
- fijar la convención de Taylor sin normalizar en `Dual2`/`HyperDual` (§3.3: `d2` guarda
  `f''(x)/2`, no `f''(x)`) y el método `second_derivative()`/`cross_derivative()` que aplica el
  factor de corrección al leer el resultado final;
- fijar que `f64` y las rutas AAD de Hull-White quedan fuera de esta generalización (§5.1).

**Aceptación**: ADRs escritos, sin decisiones semánticas implícitas en el código.

**ADR-HD-01 (trait `DualNumber`)**: exactamente las 8 operaciones de §3.1 (`re`, `constant`, `abs`,
`exp`, `ln`, `powf`, `min`, `max`, más los supertraits `Copy + Add + Sub + Mul + Div + Neg`) — ni
`variable()` ni ningún constructor de "seedeo de dirección" entran en el trait, porque cada tipo de
la familia seedea de forma distinta (§3.1/§3.4) y generalizarlo obligaría a un enum de "direcciones"
sin uso real fuera de `GbmDualPath`. Implementado como trait `pub(crate) DualNumber` en
`rust/crates/engine-core/src/payoff/dual.rs`; `Dual` lo implementa delegando en sus propios métodos
inherentes (que Rust sigue resolviendo en las llamadas concretas ya existentes, así que su
comportamiento observable no cambia — Fase 1, §7).

**ADR-HD-02 (convención de Taylor sin normalizar)**: `Dual2::d2` y `HyperDual::dxy` guardan el
coeficiente de Taylor (`f''(x)/2!` y `∂²f/∂x∂y` sin normalizar respectivamente, igual que el
desarrollo `f(x+ε) = f(x) + f'(x)ε + f''(x)/2!·ε² + ...`), NUNCA la derivada ya multiplicada por el
factor de corrección — ver §3.3 para la prueba de por qué esta convención es la que hace que el
producto de Cauchy truncado sea exactamente la composición de Taylor sin corregir en cada operación
intermedia. `Dual2::second_derivative()` devuelve `2.0 * d2`; `HyperDual::cross_derivative()`
devuelve `dxy` tal cual (la derivada mixta NO lleva factorial, a diferencia de `d2`). Estos dos
métodos son el ÚNICO punto donde se aplica la corrección — nunca dentro de la aritmética (`Mul`,
`exp`, `ln`, `powf`), que opera siempre sobre coeficientes de Taylor sin normalizar.

**ADR-HD-03 (fuera de alcance, sin excepción)**: `f64` (intérprete de valoración `eval::eval_scalar`)
y las rutas AAD reverse-mode de Hull-White (`Autodiff<CpuBackend>` de Burn, tensores, no escalares
de payoff) NO implementan `DualNumber` en esta generalización — ver §5.1/§9/§10. Gamma/Vanna de
Hull-White siguen sirviéndose exclusivamente por bump-and-reval.

### Fase 1 — trait `DualNumber` + intérprete genérico (sin nuevos tipos) — DONE

- extraer el trait de la aritmética que `Dual` ya tiene (impl mecánica, sin cambiar `Dual`);
- genericizar `eval_scalar_dual`/`eval_contract_dual`/`DualObservablePath` sobre `T: DualNumber`;
- `GbmDualPath<T>` genérico, `GbmDualPath<Dual>` como único caso instanciado en esta fase.

**Aceptación**: todos los tests existentes de `dual.rs`/`sensitivity.rs` pasan sin tocar una
aserción — refactor puro, cero cambio de comportamiento observable (mismo criterio que
`IModel::to_params()` en PLAN_GREEKS.md Fase 1: "aditivo, mecánico, no cambia ningún comportamiento
existente"). Confirmado: `cargo test -p engine-core` en verde (104/104 en `payoff::*`), `cargo build
--workspace` limpio (incluye `engine-ffi`).

**Nota de implementación (no contradice el diseño, lo precisa)**: `GbmDualPath::param_duals()` no
es un metodo generico sobre `T` con un cuerpo compartido (el propio §4 ya anticipaba que el seedeo
difiere por tipo) — se implementa via un trait auxiliar `GbmParamDuals<T: DualNumber>` (una
`impl GbmParamDuals<Dual> for GbmDualPath<'_, Dual>` en Fase 1, una por cada tipo nuevo en Fase
2/3), lo que permite que `value_at`/`rate_dual` SI sean genericos y escritos una unica vez (`impl<T:
DualNumber> ObservablePathT<T> for GbmDualPath<'_, T> where GbmDualPath<'_, T>: GbmParamDuals<T>`).
Reduce la duplicacion de Fase 2/3 al minimo: cada tipo nuevo solo aporta su propio `param_duals()`,
no una copia de `value_at`. Instrumentacion añadida en Fase 1 para §8.4: `dual.rs::mul_f64` cuenta
multiplicaciones reales en builds de test (cero coste en release, `#[inline(always)]` + `#[cfg(test)]`).

### Fase 2 — `Dual2` (Gamma/Volga pathwise)

- `Dual2` + su aritmética (§3.3);
- `GbmDualPath<Dual2>`/`GbmDualPath::param_duals` extendido a orden 2;
- `payoff::api::payoff_sensitivity2_gbm_q`/`_p` (nueva función pública Rust, mismo patrón que
  `payoff_sensitivity_gbm_q`/`_p`: decide `Dual2` vs fallback si `contains_exercise`);
- puente cxx (`engine-ffi`) + `pathwise2_capabilities()`/`try_pathwise2` en `greeks.cpp` (§5);
- test diferencial obligatorio (§6) antes de activar bajo `method=Auto`.

**Aceptación**: Gamma de una call europea vía `Dual2` coincide con Black-Scholes cerrado Y con el
bump-and-reval de `GreeksFase6Test` dentro de tolerancia; `method=auto` la prefiere sobre
bump-and-reval para `(GBM, PayoffPriceQ)`/`(GBM_P, PayoffForecastP)` sin contrato `Exercise`.

### Fase 3 — `HyperDual` (Vanna/cross-gamma pathwise)

- `HyperDual` + su aritmética (§3.4);
- `GbmDualPath<HyperDual>` con `param_duals` seedeando dos direcciones (`risk_factor`/`cross_factor`);
- `payoff::api::payoff_sensitivity_cross_gbm_q`/`_p`;
- puente cxx + `pathwise_cross_capabilities()`/`try_pathwise_cross` en `greeks.cpp`;
- test diferencial obligatorio (§6).

**Aceptación**: Vanna de una call europea vía `HyperDual` coincide con el estencil de 4 puntos de
`GreeksFase6Test.VannaOfACallHasExpectedSignAndMatchesClosedFormMixedFiniteDifference` dentro de
tolerancia, con una única pasada por ruta en vez de 4 evaluaciones bumpeadas.

### Fase 4 — orden 3 (`Dual3`/`HyperDual` de 3 direcciones) — condicional a demanda real

- mismo patrón exacto que Fases 2-3, generalizado a `Dual3` (Speed) y, si hiciera falta una
  cruzada de tres direcciones, un tipo de 8 componentes;
- **no se activa por defecto en ningún `method=Auto`** hasta que exista un caso de uso concreto
  (Speed/Color no aparecen en ningún requisito de PLAN_GREEKS.md hoy) — se documenta el diseño
  (§3.5) para que construirlo, si algún día se pide, sea mecánico, pero esta fase queda marcada
  explícitamente como "bajo demanda", no como parte obligatoria del Definition of Done (§9).

**Aceptación** (si se activa): mismo criterio que Fases 2-3, con oráculo cerrado (tercera derivada
de Black-Scholes) donde exista.

### Fase 5 (opcional, stretch) — unificar también el intérprete `f64`

- `impl DualNumber for f64` (trivial: `abs`/`exp`/`ln`/`powf`/`min`/`max` ya existen en `f64`,
  `constant`/`re` son identidad);
- sustituir `eval::eval_scalar`/`eval::eval_contract` por `eval_scalar::<f64>`/`eval_contract::<f64>`.

**No es un requisito de este plan** (§9): es la ruta más caliente del motor (una vez por paso de
cada ruta Monte Carlo simulada) y remplazarla sin necesidad de negocio es exactamente el riesgo que
PLAN_PRODUCTS.md/PLAN_GREEKS.md evitan de forma sistemática. Solo se acomete si un perfilado real
muestra que la duplicación de código (no de coste en runtime) es un problema de mantenimiento, y
con un benchmark antes/después que demuestre cero regresión de rendimiento en el camino caliente.

## 8. Estrategia de pruebas

### 8.1 Unitarias de aritmética (`dual.rs`)

- `Dual2`/`HyperDual` reproducen la regla de la cadena para cada operación (`exp`, `ln`, `powf`,
  `abs`, `min`, `max`) contra diferencias finitas de segundo orden/mixtas (mismo patrón que el test
  existente `chain_rule_matches_finite_differences_for_a_representative_expression` en `dual.rs`,
  extendido a segunda derivada/cruzada).
- Identidad `Dual2::second_derivative() == 2.0 * d2` y `HyperDual::cross_derivative() == dxy`
  (documentan la convención de Taylor sin normalizar, §3.3).

### 8.2 Financieras (oráculo independiente, mismo criterio que PLAN_GREEKS.md §12.1)

- Gamma/Vanna de una call/put europea contra Black-Scholes cerrado (ya usado como oráculo en
  `test_gbm_measures.cpp`/`test_greeks.cpp`).
- Relación Theta-Gamma-Vega de Black-Scholes como test de consistencia cruzada, ahora con Gamma
  pathwise en vez de bump-and-reval en uno de los tres términos.

### 8.3 Diferenciales (obligatorias antes de activar `method=Auto`, §6)

- `Dual2`/`HyperDual` vs bump-and-reval (`GreeksFase6Test`) para cada entrada de
  `pathwise2_capabilities()`/`pathwise_cross_capabilities()`.
- Un contrato con `ContractOp::Exercise` sigue cayendo al fallback — test de no-regresión explícito
  (mismo criterio que `GreeksFase7Test.AutoFallsBackToBumpAndRevalForAContractWithExercise`).

### 8.4 Robustez

- coste medido (no solo estimado): un micro-benchmark en `dual.rs` que cuenta multiplicaciones
  reales de `Dual`/`Dual2`/`HyperDual` sobre la misma expresión, confirmando la tabla de §0.1 no se
  degrada silenciosamente si alguien "optimiza" la aritmética de forma incorrecta.

## 9. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Generalizar el intérprete introduce una regresión sutil en la ruta `Dual` ya probada | Fase 1 es refactor puro (mismo comportamiento, mismos tests, cero tipo nuevo) antes de tocar nada de Gamma/Vanna |
| Confundir la convención de Taylor (`f''` vs `f''/2`) en `Dual2`/`HyperDual` | ADR fijado en Fase 0 (§3.3) antes de escribir una sola línea de aritmética, con métodos `second_derivative()`/`cross_derivative()` explícitos que aplican la corrección una única vez, en el borde |
| Combinatoria de tipos si se generaliza a N direcciones sin límite | familia CERRADA y con nombre (`Dual`, `Dual2`, `HyperDual`, `Dual3` documentado) — no un `Taylor<const N: usize>` genérico sin límite; orden 3+ es explícitamente "bajo demanda" (§7 Fase 4), no un default |
| Tocar `eval::eval_scalar` (ruta caliente) sin necesidad real | Fase 5 (unificar con `f64`) es explícitamente opcional y requiere benchmark de no-regresión antes de aceptarse |
| Activar `Dual2`/`HyperDual` bajo `method=Auto` sin verificación | misma disciplina de PLAN_GREEKS.md §5.3: test diferencial obligatorio por entrada de tabla de capacidades antes de mezclar (§6) |

## 10. Fuera de alcance inicial

- Hessiano completo de Hull-White vía AAD reverse-mode de Burn (forward-over-reverse) — sigue
  fuera de alcance por la misma razón que PLAN_GREEKS.md §5.2/§15 ya documentó; este plan es
  exclusivamente sobre el intérprete de payoff (`ScalarOp`), no sobre los tensores de Hull-White.
  Es un problema autoral distinto, con distinta solución.
- Orden 3+ como default de producción (§7 Fase 4: documentado, no activado salvo demanda real).
- Unificar el intérprete `f64` bajo el mismo trait (§7 Fase 5: stretch opcional).
- Un `Taylor<const N: usize>` genérico sin límite de orden — se prefiere una familia cerrada y
  nombrada (mismo argumento que PLAN_GREEKS.md §14 usa contra "explosión de combinaciones": tipos
  con nombre y coste conocido, no una plantilla abierta que alguien podría instanciar con N=50 por
  error).

## 11. Definition of Done

Esta generalización se considera implantada, no solo prototipada, cuando:

- `Dual` sigue funcionando sin cambio de comportamiento observable (Fase 1 es un refactor puro);
- Gamma y Vanna/cross-gamma de `(GBM, PayoffPriceQ)`/`(GBM_P, PayoffForecastP)` sin `Exercise` se
  sirven por pathwise (`Dual2`/`HyperDual`) bajo `method=Auto`, verificadas diferencialmente contra
  bump-and-reval con tolerancia declarada (§6/§8.3);
- el coste de cada tipo (§0.1) está confirmado por un micro-benchmark real, no solo calculado a
  mano (§8.4);
- pedir `Dual2`/`HyperDual` nunca afecta el coste ni el comportamiento de una petición de `Dual`
  (orden 1) — cero acoplamiento en runtime entre tipos de la familia, solo comparten el trait;
- un contrato con `ContractOp::Exercise` o una métrica indicador siguen cayendo a bump-and-reval en
  CUALQUIER orden, sin excepción (§5.1);
- suites Rust y C++ (`dual.rs`, `sensitivity.rs`, `test_greeks.cpp`) están en verde.

---

*Decisión central: un número dual truncado es, para el intérprete de payoff, "cuántas componentes
de Taylor se propagan por operación" — más componentes es estrictamente más caro (producto de
Cauchy, crece con el cuadrado del número de componentes), y el motor de Greeks solo instancia el
tipo concreto que el `GreekRequest` pide. Generalizar `Dual` a una familia con un trait común no es
gratis en líneas de código, pero sí lo es en runtime para quien no pide el orden extra.*
