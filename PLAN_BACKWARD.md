# PLAN_BACKWARD.md — Hessiano (forward-over-reverse) y Hessiano-vector (reverse-over-forward)

> Documento de arquitectura y plan de implementación, hermano de [PLAN_HYPERDUAL.md](PLAN_HYPERDUAL.md)
> (que motiva este documento: allí se abandonó `Dual2`/`HyperDual` para el motor de payoff porque
> daban Gamma/Vanna matemáticamente incorrectas en payoffs con kink, y se sustituyeron por
> *likelihood ratio*). Este documento responde a la pregunta de seguimiento: ¿tiene sentido añadir
> **forward-over-reverse** (Hessiano completo) y **reverse-over-forward** (producto Hessiano-vector,
> "HVP") como dos modos nuevos de un motor con tres modos en total (forward/reverse/mixto), pensando
> en carteras con muchos factores de riesgo? Y, si es así, ¿los números duales de segundo orden
> (`Dual2`/`HyperDual`) tienen un sitio en ese diseño híbrido?
>
> Como en PLAN_HYPERDUAL.md: este documento no declara implementadas las fases; cada una se
> considera cerrada solo tras quedar construida y verificada de extremo a extremo. **Este documento
> contiene además una investigación con hallazgos verificados numéricamente (§4) sobre cómo
> extender el mecanismo de likelihood ratio a Hessianos locales y a activos correlacionados — es la
> pieza central para responder "cómo se solucionan los kinks" en el caso multi-parámetro.**

## 0. Decisión ejecutiva

La respuesta corta, adelantada aquí porque cambia el orden natural de lectura: **forward-over-reverse
y reverse-over-forward son estrategias de COSTE COMPUTACIONAL para obtener información de segundo
orden con muchos inputs — no cambian NADA sobre la corrección matemática frente a un payoff con
kink.** El problema que forzó abandonar `Dual2`/`HyperDual` en PLAN_HYPERDUAL.md §5.0 (derivar dos
veces una función que fija la rama de un `Max`/`Min`/`If`/`Trigger` da una segunda derivada
pathwise degenerada) es idéntico sea cual sea el ORDEN en que se componen forward y reverse — ambos
siguen aplicando la regla de la cadena dos veces sobre el mismo grafo con la rama fija. Componer
forward-sobre-reverse o reverse-sobre-forward abarata el CÓMO se propaga la derivada (una pasada
reverse da el gradiente completo de golpe; forward sobre eso da una columna del Hessiano por
dirección), pero no toca el QUÉ se está derivando.

Esto separa el proyecto en **tres frentes independientes**, con validez y coste muy distintos (tabla
completa en §3):

1. **Motor de payoff Monte Carlo (GBM, kinks)** — el sitio donde nació la pregunta. Aquí NO hace
   falta AD de ningún tipo, ni forward ni reverse ni mixto: el Hessiano LOCAL de un único trade
   (todas las segundas derivadas de sus 3-4 parámetros de modelo) se obtiene **reutilizando la
   MISMA tanda de rutas simuladas**, evaluando el payoff una vez por ruta y multiplicando por
   distintos pesos cerrados de verosimilitud — es decir, el motor de payoff YA tiene, sin construir
   ningún grafo de AD, la propiedad que se le pide a reverse-mode ("una pasada cara, muchas
   derivadas parciales baratas"). §4 generaliza y VERIFICA esto, incluido el caso de activos
   correlacionados (relevante para baskets/spreads).
2. **Hull-White / IRS (sin kinks)** — aquí SÍ hay hoy un reverse-mode real (Burn `Autodiff`, un
   `.backward()` da las 4-6 griegas de primer orden). Un spike de este documento (§1.2) confirma
   que Burn 0.21 **no anida** `Autodiff<Autodiff<_>>` para dar un Hessiano (`x.grad(&grads2)` sale
   `None`) — no es pereza, es una limitación real y verificada de la versión pinneada. La vía
   práctica NO es pelearse con Burn: es resucitar `Dual2`/`HyperDual` (exactamente los tipos
   abandonados en PLAN_HYPERDUAL.md) para una implementación paralela, escalar, del pricer
   CERRADO de Hull-White — aquí SÍ tiene sentido, porque no hay ramas y el número de parámetros es
   pequeño (4-6), justo el caso para el que el forward-over-forward es barato (§5).
3. **Curva/pilares de una cartera grande (muchos factores, sin kinks)** — el caso "muchos inputs"
   real de este código hoy (una curva puede tener 20-50 pilares). Aquí SÍ valdría la pena un
   forward-over-reverse/reverse-over-forward genuino, pero requiere construir un grafo diferenciable
   de descuento que **hoy no existe en absoluto** (el descuento de curva es bump-and-reval puro, ver
   §1.1) — es arquitectura nueva, no una extensión de algo que ya está medio construido.
   `price_many`/`price_batch` (la idea natural para "muchos trades") tampoco ayudan hoy: agrupan
   llamadas por calendario y vectorizan 4 medidas conocidas, pero para `"Greek"` caen a un bucle por
   trade sin compartir ningún cómputo de curva (§6.1). Esto se separa en dos niveles: **Nivel A**
   (reverse-mode de curva por UN trade, sin necesitar cartera — mecánico, sin bloqueantes) y
   **Nivel B** (fusionar MUCHOS trades en un grafo compartido — bloqueado por que no existe un
   concepto de `Portfolio`, aunque `ARCHITECTURE_REVIEW.md` ya lo propone en su Fase 4, sin empezar).
   Se documenta como Fase de investigación/futuro (§6), no se implementa en este plan.

**Sobre `Dual`/`DualNumber`**: se conserva sin cambios (Delta/Vega/Rho del motor de payoff, orden 1,
PLAN_HYPERDUAL.md Fase 1). Es además la pieza que se reutiliza para el Hessiano cerrado de
Hull-White (§5) — un dual de orden 1/2, no de orden 2 puro reciclado del plan anterior; se
construyen tipos `Dual2`/`HyperDual` NUEVOS, propios de ese pricer escalar, no una resurrección
literal de los tipos borrados (que vivían acoplados al intérprete `ScalarOp` del payoff).

## 1. Estado actual

### 1.1 Lo que ya existe (verificado en este documento, no asumido)

- **Motor de payoff (`rust/crates/engine-core/src/payoff/`)**: código escalar `f64` puro, sin cinta,
  sin grafo de tensores. `Dual` (orden 1, `dual.rs`) para Delta/Vega/Rho; `payoff::lrm` (sin
  aritmética dual, solo pesos cerrados de densidad) para Gamma/Vanna de una única fecha terminal.
  Cero infraestructura de reverse-mode.
- **Hull-White (`rust/crates/engine-core/src/api.rs:515-548`, `999-1038`)**:
  `irs_hull_white_npv_all_greeks`/`_2f_...` construyen el NPV como `Tensor<Autodiff<CpuBackend>, 1>`
  (parámetros con `.require_grad()`), llaman a `price.backward()` **una única vez** y leen TODAS las
  griegas de primer orden (`a`, `b`, `sigma`, `r0`, ...) de la misma cinta (`Gradients`). Reverse-mode
  real, ya en producción, ya de "una pasada, muchas derivadas" — pero solo orden 1. `CpuBackend =
  burn::backend::NdArray<f64>` (`backend.rs:15`), Burn fijado en `0.21.0` con features
  `["std","ndarray","autodiff"]` (`engine-core/Cargo.toml`). No existe ningún código de
  Hessiano/forward-over-reverse/HVP/`Autodiff<Autodiff<_>>` en el repo.
- **PLAN_GREEKS.md §15** documenta la exclusión: *"Gamma/cross vía AAD reverse-mode de segundo orden
  (Hessiano completo) queda fuera de alcance inicial: Burn no expone segundo orden de forma directa y
  forward-over-reverse es significativamente más trabajo; Gamma se calcula vía bump-and-reval también
  para HullWhite."* — diferido por esfuerzo, nunca verificado como imposible. Este documento hace esa
  verificación (§1.2).
- **Descuento de curva** (`bump_market_parallel`/`bump_market_pillar`, `RiskFactorKind::CurveParallel`/
  `CurvePillar`): reconstruye `MarketSnapshot` y repite la valoración completa — bump-and-reval puro,
  sin ningún grafo diferenciable por debajo, para NINGÚN modelo (ni Hull-White ni payoff). Un
  Hessiano/HVP de pilares de curva no tiene ninguna base de AD sobre la que apoyarse hoy.
- **Escala de "portfolio" real hoy**: `compute_all_greeks` (`greeks.cpp:1009+`) y `price_batch`/
  `price_batch_generic` (`cpp/engine/src/price.cpp:379-469`) recorren **un trade/factor a la vez**,
  cada uno valorado de forma independiente (solo deduplicado por *fingerprint*, no por un grafo
  compartido). "Cartera grande" hoy significa N evaluaciones independientes, no un tape compartido —
  importante para no sobrevender el beneficio de un reverse-mode que nadie ha conectado todavía a un
  batching real (§6).

### 1.2 Spike: `Autodiff<Autodiff<CpuBackend>>` no propaga el gradiente de segundo orden en Burn 0.21

Código del spike (no comiteado, ejecutado y descartado; reproducible si hace falta volver a
verificarlo tras actualizar Burn):

```rust
type Inner = Autodiff<NdArray<f64>>;
type Outer = Autodiff<Inner>;   // "forward-over-reverse" conceptual: dos capas de Autodiff

let x: Tensor<Outer, 1> = Tensor::from_data(TensorData::from([2.0_f64]), &device).require_grad();
let y = x.clone() * x.clone() * x.clone();      // f(x) = x^3, f'(2)=12, f''(2)=6*2=12

let grads = y.backward();
let dx = x.grad(&grads).unwrap();                        // f'(x) -- coincide con 12.0, OK
let dx_tensor: Tensor<Outer, 1> = Tensor::from_inner(dx).require_grad();

let grads2 = dx_tensor.backward();
x.grad(&grads2)   // <-- None. La segunda derivada NO se propaga.
```

`Tensor::from_inner(dx).require_grad()` crea una hoja NUEVA, desconectada del grafo exterior de
`x` — `.grad()` solo devuelve el tensor del backend INTERIOR, sin metadatos que permitan seguir
derivando respecto de `x` en la capa EXTERIOR. Confirmado además por búsqueda externa: no hay
mención de soporte de segundo orden/"double backward"/`create_graph` en `burn-autodiff` (ni en el
código fuente de la 0.21.0, ni en el repositorio/discusiones públicas del proyecto a fecha de este
documento). **Conclusión operativa: no se puede dar por hecho un Hessiano de Hull-White "gratis" vía
Burn hoy** — cualquier Fase que lo necesite debe o (a) evitar Burn para la segunda derivada (§5), o
(b) tratarse como bloqueada a que Burn añada la funcionalidad (sin compromiso de roadmap conocido).

## 2. La pregunta de fondo, respondida una vez y citada después

**¿Arreglan forward-over-reverse/reverse-over-forward el problema de los kinks?** No, y conviene
decirlo una sola vez con precisión para no repetir el error de PLAN_HYPERDUAL.md: da igual si la
composición es forward-forward (`Dual2`), forward-reverse o reverse-forward — las tres aplican la
regla de la cadena DOS VECES sobre la función "payoff evaluado en una ruta con la rama de
`Max`/`Min`/`If`/`Trigger` ya decidida". Esa función es afín a trozos en cualquier parámetro que
mueva la frontera entre ramas, así que su segunda derivada pathwise es 0 en casi toda ruta
independientemente del mecanismo de AD usado para calcularla. El único mecanismo que da la respuesta
correcta es diferenciar la DENSIDAD en vez del payoff (likelihood ratio/Malliavin, PLAN_HYPERDUAL.md
§5.1) — que **no es AD** en el sentido de propagar tangentes/adjuntos por un grafo de cómputo, es
una reponderación de valores ya evaluados. Todo lo que sigue en este documento respeta esto: se
propone AD (forward-over-reverse, reverse-over-forward) únicamente donde NO hay kinks (Hull-White,
curva de descuento); donde SÍ los hay (payoff de opciones), se generaliza el mecanismo de
verosimilitud, no el de AD.

## 3. Los tres frentes, uno al lado del otro

| Frente | ¿Kinks? | ¿Cuántos parámetros? | Mecanismo correcto | Estado |
|---|---|---|---|---|
| Payoff Monte Carlo (opciones, barreras) | Sí | Pocos por trade (3-4 de un único GBM) | Reponderar UNA tanda de rutas con distintos pesos cerrados (§4) — no es AD | §4: investigado y verificado numéricamente; falta implementar (Fase 1) |
| Hull-White / IRS (cerrado, sin Monte Carlo) | No | Pocos (4 en 1F, 6 en 2F) | Forward-over-forward con `Dual2`/`HyperDual` NUEVOS sobre el pricer cerrado (Burn no sirve, §1.2) | §5: diseñado: falta implementar (Fase 2/3) |
| Curva/pilares de una cartera grande | No (si solo hay cashflows lineales) | Muchos (20-50 pilares típico) | Forward-over-reverse/reverse-over-forward genuinos sobre un grafo de descuento diferenciable que HOY NO EXISTE | §6: solo investigación/futuro, fuera de alcance de este plan |

## 4. Investigación: generalizar likelihood ratio a Hessianos locales (multi-parámetro y multi-activo)

Esta sección es la respuesta directa a "cómo se solucionan los kinks" para el caso que de verdad
importa en este motor (opciones con Monte Carlo GBM) — y la buena noticia es que no hace falta AD
en absoluto, ni forward ni reverse, para conseguir la propiedad de "una pasada cara, muchas
derivadas baratas" que motivó la pregunta original.

### 4.1 El Hessiano local de un único trade sale de LA MISMA tanda de rutas

`payoff::lrm` ya calcula, hoy, Gamma y Vanna con la misma estructura: simular una tanda de rutas,
evaluar el payoff `f64` normal en cada una (`present_value_per_path`), y promediar
`present_value_per_path * weight(Z, params)`. El dato caro (simular + evaluar el payoff) es
**exactamente el mismo** para Gamma, Vanna, y (nuevo) Volga (`σσ`) — solo cambia el `weight(Z,...)`,
que es una fórmula cerrada evaluada sobre la MISMA `Z` ya recuperada por ruta. Es decir: **el motor
de payoff ya tiene, de fábrica, la propiedad "una pasada, muchas parciales" que se le pedía a
reverse-mode** — solo hay que dejar de tirar la tanda de rutas después de calcular una única Greek y
reutilizarla para las demás.

Esto es un cambio de implementación pequeño y de bajo riesgo (Fase 1, §9): en vez de que
`payoff_sensitivity2_gbm_q`/`payoff_sensitivity_cross_gbm_q` simulen cada uno su propia tanda
(hoy lo hacen, con seeds correlados pero llamadas independientes), una única función
`payoff_local_hessian_gbm_q` simula UNA tanda y devuelve `{gamma_ss, volga_vv, vanna_sv}` (y,
documentado pero opcional, `gamma_rr`/`gamma_qq`/cruzadas con `rate`/`dividend_yield` si aparece
demanda — mismo criterio de "familia cerrada, extensible mecánicamente" que PLAN_HYPERDUAL.md §10).

### 4.2 Generalización a activos correlacionados — derivación y verificación numérica

El motor de payoff hoy solo soporta un `Gbm` de un único observable. Para cuando exista un payoff
multi-activo (basket, spread, rainbow — no implementado hoy, pero es la extensión natural de "cartera
con muchos factores"), la pregunta relevante es: ¿el truco de verosimilitud generaliza a activos
CORRELACIONADOS, y sigue dando "una pasada, muchas parciales"? Sí, con una fórmula general derivada
y verificada aquí.

**Marco general.** Para `n` activos lognormales con Brownianos correlacionados (matriz de
correlación `ρ` fija, no un parámetro a derivar), la densidad conjunta de `S_T` en un punto `s`
tiene la forma `ln f(s;θ) = A(θ) - ½ ε(θ)ᵀ P ε(θ)`, donde `ε(θ)` es el vector de residuos
estandarizados (`ε_k = (ln(s_k/s0_k) - μ_k(θ)T)/(σ_k(θ)√T)`, evaluado en el punto observado `s`,
como función de los parámetros `θ`), `P = ρ⁻¹` (la matriz de precisión, constante), y `A(θ)` recoge
los términos normalizadores que dependen de `θ` solo a través de `σ_k` (nunca de `s0_k`). El peso de
Hessiano para cualquier par de parámetros `(θ_i, θ_j)` es exactamente la misma identidad que en el
caso univariante (PLAN_HYPERDUAL.md §5.1), ahora con productos matriciales:

```text
weight_ij(ε) = (∂ln f/∂θ_i)(∂ln f/∂θ_j) + ∂²ln f/∂θ_i∂θ_j

∂ln f/∂θ_i        = A'_i(θ) - εᵀ P (∂ε/∂θ_i)
∂²ln f/∂θ_i∂θ_j   = A''_ij(θ) - (∂ε/∂θ_j)ᵀ P (∂ε/∂θ_i) - εᵀ P (∂²ε/∂θ_i∂θ_j)
```

`Gamma = E[valor_presente(ruta) · weight_ii]`, `Hessiano cruzado_ij = E[valor_presente(ruta) ·
weight_ij]` — de nuevo sin derivar el payoff, solo reponderando. Para `n=1` (un único activo) esta
fórmula se reduce exactamente a `gamma_weight`/`vanna_weight`/`volga_weight` ya implementados y
verificados en `payoff::lrm`.

**Verificación numérica (cuadratura 2D determinista, sin ruido de Monte Carlo)**: dos activos GBM
correlacionados (`s0=100` ambos, `σ₁=0.2`, `σ₂=0.25`, `r=0.05`, `q₁=0`, `q₂=0.01`, `ρ=0.4`,
`T=1`), payoff basket `h = max(0.5·S₁+0.5·S₂-100, 0)`:

| Cantidad | Diferencia finita (oráculo) | Fórmula de verosimilitud | Diferencia |
|---|---:|---:|---:|
| Gamma cruzada `∂²V/∂s0₁∂s0₂` | 0.00515544 | 0.00516135 | 6e-6 (ruido de truncamiento de la diferencia finita) |
| Gamma "propia" `∂²V/∂s0₁²` (fórmula univariante SIN corregir) | 0.00548248 | 0.01109859 | **5.6e-3 — MAL, factor ~2** |
| Gamma "propia" `∂²V/∂s0₁²` (fórmula general con corrección de correlación) | 0.00548248 | 0.00548559 | 3e-6 |

**Hallazgo no obvio, documentado aquí para no repetir el error**: en presencia de correlación, ni
siquiera la Gamma "propia" de un activo (mantener fijo todo excepto `s0₁`) coincide con la fórmula
univariante de un único activo aislado — hay un término de corrección que depende de `ρ` y del
residuo del OTRO activo (`ε₂`), porque se está derivando la densidad CONJUNTA, no la marginal. Un
motor multi-activo que reutilizara ingenuamente el peso univariante por activo (en vez de la matriz
de precisión completa) daría Gammas propias incorrectas en cualquier cartera con activos
correlacionados — exactamente el tipo de error silencioso que la disciplina de verificación de este
proyecto (PLAN_GREEKS.md §5.3, PLAN_HYPERDUAL.md §6) existe para atrapar.

### 4.3 Lo que sigue sin resolver: dependencia de trayectoria

La derivación de §4.2 sigue usando la densidad MARGINAL en una única fecha terminal — igual que
`payoff::lrm` hoy (PLAN_HYPERDUAL.md §5.2). Generalizar a un payoff con dependencia de trayectoria
(barreras, triggers, `Exercise`, monitorización discreta) exigiría la densidad conjunta de TODA la
trayectoria simulada (Malliavin calculus sobre el proceso completo, Fournié et al. 1999 y extensiones
posteriores para barreras/asiáticas) — un cuerpo de literatura real y una técnica viable en
principio, pero un desarrollo de investigación sustancial, no una extensión mecánica de §4.1/§4.2.
Se documenta como **fuera de alcance de este plan** (§12), no como "trabajo futuro planificado".

## 5. Frente Hull-White: Hessiano cerrado con números duales de segundo orden (donde SÍ tienen sitio)

Dado que Burn no anida autodiff (§1.2) y que el pricer de Hull-White es una fórmula CERRADA (bono
cupón cero `P(t,T) = A(t,T)·exp(-B(t,T)·r(t))`, sin `Max`/`Min`/`If` en el camino de valoración de un
swap — a diferencia del motor de payoff, aquí no hay ninguna razón para sospechar un kink; se
confirma explícitamente como ADR antes de escribir código, ver §9 Fase 0), el camino pragmático es
exactamente la idea original de PLAN_HYPERDUAL.md, aplicada donde SÍ es válida:

- **`Dual2`/`HyperDual` NUEVOS**, implementados como una segunda pasada escalar del pricer cerrado
  de Hull-White (no del motor de payoff — sin relación con `payoff::dual`), en un módulo propio
  (p.ej. `rust/crates/engine-core/src/models/hull_white_dual.rs`). Mismo diseño de aritmética que el
  `Dual2`/`HyperDual` documentados (y luego revertidos) en PLAN_HYPERDUAL.md §3.3/§3.4 — la
  convención de Taylor sin normalizar, el producto de Cauchy truncado, todo aplica sin cambios,
  porque aquí la función a derivar SÍ es analítica sin ramas.
- **Por qué forward-over-forward y no forward-over-reverse aquí**: con 4 parámetros (1F) o 6 (2F),
  el Hessiano completo tiene 10/21 pares distintos — barato de sobra evaluando `HyperDual`
  (2 direcciones) por cada par, sin necesitar componer con reverse-mode en absoluto. Esto es
  justo la afirmación original de PLAN_HYPERDUAL.md §0 ("forward sirve cuando hay pocos inputs"),
  aplicada correctamente esta vez porque el modelo no tiene kinks.
- **HVP (`H·v`) para Hull-White**: con `N≤6`, no hace falta el truco de Pearlmutter (reverse-over-forward)
  — basta multiplicar la matriz Hessiana ya calculada por el vector `v` (coste trivial, `O(N²)` una
  vez, reutilizable para cualquier `v`). El HVP "genuino" (sin montar nunca el Hessiano completo) solo
  se justifica cuando `N` es grande — ver §6.
- **Qué pasa si Burn añade doble-`backward` en el futuro**: esta Fase queda igualmente válida (el
  pricer cerrado con `Dual2`/`HyperDual` no depende de Burn); si además Burn lo soporta, se podría
  AÑADIR una segunda ruta vía tensores como optimización, verificada contra esta como oráculo — no
  es necesario esperar a Burn para tener un Hessiano de Hull-White correcto.

## 6. Frente curva/pilares de una cartera grande — dónde forward-over-reverse/reverse-over-forward pagarían de verdad

Este es el único frente donde forward-over-reverse/reverse-over-forward genuinos (en el sentido con
el que se preguntó originalmente: composición de AD para abaratar MUCHOS parámetros compartidos por
MUCHOS trades) tendrían un beneficio real y correcto (sin kinks: los cashflows de un IRS son lineales
en la curva de descuento). Ampliado aquí porque merece un análisis serio, aunque no se implemente
todavía: la idea del usuario es apoyarse en `price_many` para valorar muchos trades a la vez, sin que
exista hoy un concepto de `Portfolio`.

### 6.1 Lo que `price_many` hace HOY (verificado, no lo que parece que hace por el nombre)

- `price_many` (`cpp/engine/src/price.cpp:521-565`) agrupa trades por `(type_name(),
  calendar_group_key)` — `calendar_group_key` (línea 255-268) es una clave de texto construida a
  partir de `start()`/`payment_times()`/`accruals()` de un IRS — y llama a `price_batch` **una vez
  por grupo** (línea 559), repartiendo los resultados de vuelta por índice original. El agrupamiento
  existe para que el camino vectorizado de `price_batch` (que exige un calendario común) se pueda
  aplicar a sub-lotes de una lista heterogénea — **no para compartir ningún cómputo de curva entre
  trades**.
- `price_batch` (`price.cpp:433-507`), incluso dentro de un grupo de calendario homogéneo, solo
  vectoriza 4 tipos de medida conocidos (PV/DV01/...) vía `evaluate_batch_registered_measure`
  (línea 480); para cualquier otra medida — **incluida `"Greek"`**, que es como se pediría un
  Hessiano/HVP — cae explícitamente a un bucle por trade (línea 486-490, una llamada a
  `measure->evaluate(...)` por trade). `price_batch_generic` (línea 379-429, la ruta para
  `PayoffProduct`/no-IRS) está documentado en el propio código como "sin vectorización -- correcto,
  no optimizado" (línea 360) y solo deduplica por *fingerprint* de AST idéntico, nunca por cómputo de
  curva compartido.
- `MarketSnapshot`/`Curve` (`engine/market.hpp:26-92`) son `std::vector<double> pillars_,
  zero_rates_` — vectores planos. **Cero infraestructura de grafo, para NINGÚN modelo**, confirmado
  de nuevo aquí. `bump_market_parallel`/`bump_market_pillar` siguen siendo bump-and-reval puro.
- **Conclusión sin rodeos**: `price_many`/`price_batch` HOY son un mecanismo de *conveniencia de
  llamada* (agrupar N trades en una petición) y de *vectorización numérica* para 4 medidas concretas
  — no son, ni de lejos, un grafo computacional compartido. Apoyarse en ellos para un Hessiano de
  cartera significaría, literalmente hoy, seguir hasta el bucle por trade de la línea 486-490 y
  ejecutar el Hessiano de este documento (§7) trade a trade — que es exactamente lo que YA se puede
  hacer sin ningún cambio (agregando `HessianReport`s por trade, ver §6.3 Nivel A).

### 6.2 No hay concepto de `Portfolio` — pero ya se pensó, en `ARCHITECTURE_REVIEW.md`

Confirmado por grep: no existe ningún tipo `Portfolio`/`Book`/`NettingSet` en `cpp/` ni en `rust/`
(el único uso de la palabra es un comentario suelto en `payoff/api.rs:1313` describiendo
`Both(call, Give(call))` como "portfolio", no un tipo). Pero `ARCHITECTURE_REVIEW.md` — un documento
de revisión especulativa de arquitectura, no implementado — **ya propone exactamente esto** como su
"Fase 4 — Medidas, cartera y netting" (§8, líneas 392-401):

> *"Añadir `Portfolio`, `NettingSet` y colateral como niveles distintos del trade. **Compartir
> escenarios, curves y subplanes entre trades.** [...] Criterio de salida: una cartera heterogénea
> se calcula sin llamar a `price` una vez por trade y mantiene trazabilidad hasta cada cashflow."*

Es decir: el prerrequisito real de un Hessiano/HVP de cartera fusionado (§6.3 Nivel B) no es "escribir
un grafo Burn de curva" en aislado — es la **Fase 4 de `ARCHITECTURE_REVIEW.md`**, que este documento
NO redefine ni sustituye. `ARCHITECTURE_REVIEW.md` además ya anticipa el riesgo exacto que le
preocuparía a un Hessiano fusionado sobre una cartera grande, en su propia tabla de riesgos (§10,
línea 458): *"AAD costoso sobre todo el árbol | Selección explícita de targets y evaluación por
bloques"* — un Hessiano/HVP de cartera, cuando exista, hereda esa misma mitigación (nunca un único
grafo global sin límite; bloques por netting set/moneda/clase de riesgo).

### 6.3 Dos niveles de beneficio, con prerrequisitos MUY distintos

Separar esto en dos niveles es la parte nueva de este análisis — no hace falta esperar a la Fase 4 de
`ARCHITECTURE_REVIEW.md` para sacar ALGO de valor:

**Nivel A — reverse-mode de curva POR TRADE, sin necesitar `Portfolio` (candidato de Fase futura,
independiente de este documento y de la Fase 4 de `ARCHITECTURE_REVIEW.md`)**: construir el pipeline
`curva → factores de descuento → PV` de UN trade como grafo `Autodiff<CpuBackend>` (exactamente el
mismo patrón que ya usa Hull-White para sus parámetros de modelo, `api.rs:515-548`, aplicado ahora a
los pilares de `MarketSnapshot` en vez de a `a/b/sigma/r0`). Con esto, **un único trade con 50
pilares obtiene su DV01 vectorial completo en una pasada** en vez de 50 bump-and-reval — una mejora
real y ya suficiente para el caso "un trade, muchos factores de riesgo de curva" que motivó la
pregunta original, SIN portfolio, SIN fusionar trades, SIN esperar nada. `price_many`/`price_batch`
seguirían llamando a esto una vez por trade (igual que hoy), simplemente cada llamada sería más
barata. El Hessiano/HVP de curva de un solo trade (forward-over-forward con `Dual2`/`HyperDual` si
`N` de pilares relevantes es pequeño tras localizar el soporte de cashflows, o forward-over-reverse
genuino si es grande) se apoya en este mismo grafo, sin necesitar Fase 4.

**Nivel B — fusión real de MUCHOS trades en un único grafo (necesita la Fase 4 de
`ARCHITECTURE_REVIEW.md`, no solo el Nivel A)**: para que UNA sola pasada `backward()` dé la
sensibilidad AGREGADA de una cartera completa a sus pilares compartidos (en vez de sumar N pasadas
per-trade del Nivel A), los trades deben componerse en una única expresión diferenciable — eso es
literalmente lo que `Portfolio(children)` de `ARCHITECTURE_REVIEW.md` propone. Sin esa composición,
"fusionar" trades en un grafo Burn compartido significaría reconstruir a mano, en este documento, la
misma pieza de arquitectura que ya está diseñada (mejor, con más contexto de netting/colateral) en
otro sitio — trabajo duplicado, no una atajo. El Hessiano/HVP de cartera del Nivel B hereda además la
advertencia de la propia `ARCHITECTURE_REVIEW.md` (§10): evaluación por bloques, nunca un grafo
global sin límite, porque un Hessiano NxN de una cartera con miles de cashflows y decenas de pilares
crece mal si se intenta de una sola vez.

### 6.4 `Portfolio`: objeto first-class de orquestación (esto SÍ se planifica, Fase 6 de §9)

Los Niveles A/B de §6.3 dan el mecanismo de cálculo; falta el sitio donde vivan. En vez de dejar
"cartera" como un vector de `IProduct` que cada llamante ensambla a mano cada vez (que es lo que
hace `price_many` hoy, sin identidad ni estado propio), se añade un objeto `Portfolio` de primera
clase a nivel API — **conceptualmente, hoy, una lista de trades**, sin más semántica que eso; se
complicará (netting, colateral, monedas, multi-modelo) en un plan futuro dedicado, no aquí.

**Importante, para no confundir dos ideas con el mismo nombre**: este `Portfolio` vive en la capa de
**orquestación/cliente** (una colección de trades YA CONSTRUIDOS, cada uno un `IProduct` opaco e
independiente, que se valoran/arriesgan juntos por conveniencia y agregación) — es un nivel distinto
del `Portfolio(children)` que `ARCHITECTURE_REVIEW.md` §3.2/§8 propone como primitiva DENTRO del AST
de un contrato (composición de cashflows para que un netting set con opcionalidad cruzada se evalúe
como un único programa). Uno no sustituye al otro: el de orquestación agrega RESULTADOS de trades
valorados por separado (válido para lo que se pide aquí — Greeks de un libro sin colateral/netting
real); el del AST fusionaría los propios CASHFLOWS antes de valorar (necesario para netting con
opcionalidad, `Exercise` compartido, etc.). Si algún día se implementa el segundo, el primero puede
convertirse en una fachada delgada sobre él — no hay incompatibilidad, solo una decisión de qué
construir primero.

**Alcance inicial (deliberadamente mínimo, mismo criterio "no construir por adelantado" de PLAN.md)**:

- Una lista de trades (`IProduct`), sin netting, sin colateral, sin agrupar por moneda o
  contraparte — exactamente la "lista de Trades" que se pidió.
- Un `Portfolio` se valora bajo **un único `IModel`/`MarketSnapshot`** por llamada, igual que
  `price_many` hoy (§6.1) — una cartera con instrumentos que necesitan modelos DISTINTOS (p.ej.
  swaps bajo Hull-White y opciones bajo GBM a la vez) queda fuera de esta primera versión ("luego lo
  complicaremos más", explícitamente pospuesto, no diseñado aquí).
- `Portfolio.price(...)` es un envoltorio fino sobre el `price_many` YA EXISTENTE — no cambia su
  comportamiento, solo le da identidad (crear una vez, invocar varias veces con distintos
  escenarios/mercados) en vez de reconstruir el vector de trades en cada llamada.
- `Portfolio.hessian(...)`/`Portfolio.hvp(...)` son la pieza NUEVA: agregan por **suma** los
  `HessianReport`/`HvpReport` (§7) de cada trade — matemáticamente exacto HOY, sin ningún grafo
  fusionado, porque si `V(θ) = Σ_k V_k(θ)` (misma curva/modelo compartido, cada trade con su propio
  Hessiano), entonces `∂²V/∂θ_i∂θ_j = Σ_k ∂²V_k/∂θ_i∂θ_j` — suma trivial, sin aproximar nada. Esto
  ya resuelve "una API de una llamada para el Hessiano de N factores de UNA cartera" sin esperar a
  ningún grafo de Burn compartido; el Nivel A/B de §6.3, cuando exista, se convierte en una
  OPTIMIZACIÓN interna de estos dos métodos (menos pasadas de cómputo para el mismo resultado), sin
  cambiar la firma pública — el llamante no distingue si por debajo hubo 1 pasada reverse-mode
  fusionada o N sumas independientes.

**Diseño de API, mismo patrón que el resto del motor (handles opacos + tabla larga, §8)**:

```cpp
// engine/portfolio.hpp (nuevo)
class Portfolio {
public:
    Portfolio() = default;
    void add(std::shared_ptr<const IProduct> trade);
    std::size_t size() const;
    const std::vector<std::shared_ptr<const IProduct>>& trades() const;

    // Envoltorio fino sobre price_many() ya existente -- mismo Registries/IModel/MarketSnapshot
    // para TODOS los trades (alcance inicial, ver arriba).
    PriceBatchResult price(
        const Registries&, const std::vector<std::string>& measures,
        const IModel&, const MarketSnapshot&, const PricingContext&, const ExecutionContext&
    ) const;

    // NUEVO -- suma de HessianReport/HvpReport por trade (§7), exacto sin grafo fusionado.
    greeks::HessianReport hessian(
        const Registries&, const std::string& metric_name, const Params& metric_params,
        const IModel&, const MarketSnapshot&, const PricingContext&, const ExecutionContext&,
        const std::vector<greeks::RiskFactor>& factors = {}
    ) const;
    greeks::HvpReport hvp(
        const Registries&, const std::string& metric_name, const Params& metric_params,
        const IModel&, const MarketSnapshot&, const PricingContext&, const ExecutionContext&,
        const std::vector<greeks::RiskFactor>& factors, const std::vector<double>& direction
    ) const;

private:
    std::vector<std::shared_ptr<const IProduct>> trades_;
};
```

```c
/* abi.h -- mismo molde EXACTO que EngineProduct/engine_abi_create_product/engine_abi_free_product */
typedef struct EnginePortfolio EnginePortfolio;

ENGINE_ABI_API EnginePortfolio* engine_abi_create_portfolio(void);
ENGINE_ABI_API void engine_abi_portfolio_add_trade(EnginePortfolio* portfolio, const EngineProduct* trade);
ENGINE_ABI_API size_t engine_abi_portfolio_size(const EnginePortfolio* portfolio);
ENGINE_ABI_API void engine_abi_free_portfolio(EnginePortfolio* portfolio);

/* engine_abi_portfolio_price/_hessian/_hvp: mismo shape que engine_abi_price/_hessian/_hvp (§8.3)
   pero tomando EnginePortfolio* en vez de EngineProduct* -- misma tabla larga de salida. */
```

```python
# engine_typed/portfolio.py -- nuevo
class Portfolio:
    def __init__(self, trades: list[Product] | None = None):
        self.trades: list[Product] = list(trades) if trades else []

    def add(self, trade: Product) -> "Portfolio":
        self.trades.append(trade)
        return self

    def price(self, measures, model, market, pricing, execution) -> PriceBatchResult: ...
    def hessian(self, metric_name, model, market, pricing, execution,
                metric_params=None, risk_factors: list[str] | None = None) -> HessianReport: ...
    def hvp(self, metric_name, model, market, pricing, execution, direction: dict[str, float],
            metric_params=None) -> HvpReport: ...
```

```text
Excel (engine_excel.cpp), handles igual que el resto de objetos (crear una vez, reusar por celda):
=ENGINE.PORTFOLIO.CREATE()                              -> handle
=ENGINE.PORTFOLIO.ADD(portfolio, trade)                 -> ok
=ENGINE.PORTFOLIO.PRICE(portfolio, medidas, modelo, mercado, contexto, ejecucion)   -> tabla larga
=ENGINE.PORTFOLIO.HESSIAN(portfolio, metrica, ..., [factores_de_riesgo])            -> tabla larga
=ENGINE.PORTFOLIO.HVP(portfolio, metrica, ..., direccion)                          -> tabla larga
```

### 6.5 Qué se decide aquí, ahora

- **El objeto `Portfolio` mínimo (§6.4) SÍ se planifica** — Fase 6 en §9. Es aditivo, de bajo riesgo
  (envoltorio sobre `price_many` + suma de `HessianReport`/`HvpReport` ya diseñados en §7), y da la
  ventaja de "una llamada, Greeks de N factores de una cartera" **sin esperar a ningún grafo
  fusionado** — la suma es exacta, no una aproximación provisional.
- **Nivel A** (reverse-mode de curva POR TRADE, §6.3) sigue siendo una Fase futura independiente
  (candidata a `PLAN_CURVE_GREEKS.md` o una fase de PLAN_GREEKS.md) — cuando exista, `Portfolio.
  hessian()/hvp()` la adoptan por debajo SIN cambiar su firma pública (mismo criterio de "la
  interfaz no debe delatar el mecanismo interno" que ya sigue `GreekResult::method_used`).
- **Nivel B** (fusionar los trades de un `Portfolio` en un único grafo Burn, en vez de sumar N
  resultados independientes) sigue dependiendo de la Fase 4 de `ARCHITECTURE_REVIEW.md` — con la
  diferencia de que ahora SÍ hay un sitio natural (`Portfolio`) donde esa fusión, cuando llegue,
  se implementaría como optimización interna. No se diseña esa fusión aquí.
- **Multi-modelo, netting, colateral, monedas** (las complicaciones que el propio pedido pospone
  explícitamente) quedan fuera de este documento — el `Portfolio` mínimo asume un único
  `IModel`/`MarketSnapshot` compartido, igual que `price_many` hoy.

## 7. Modelo de dominio (C++, extiende `engine::greeks` sin romper nada existente)

Mismo espíritu que `GreekRequest`/`GreekResult`/`compute_greek` (`greeks.hpp`) — un Hessiano no es una
`GreekOrder` con un único `cross_factor` (eso ya existe, orden 1 cruzado), es un conjunto de pares;
un HVP no es un escalar, es un vector. Se añaden dos structs y dos funciones nuevas, sin tocar
`GreekRequest`/`GreekResult`/`compute_greek` existentes:

```cpp
// engine/greeks.hpp (ampliación)

struct HessianEntry {
    RiskFactor factor_i;
    RiskFactor factor_j;              // factor_i == factor_j -> entrada diagonal (Gamma/Volga)
    double value = 0.0;
    std::optional<double> std_error;  // presente si method_used == LikelihoodRatio (Monte Carlo)
    GreekMethod method_used = GreekMethod::BumpAndReval;
    payoff::ProbabilityMeasure measure = payoff::ProbabilityMeasure::DeterministicScenario;
};

struct HessianReport {
    std::vector<HessianEntry> entries;   // solo triangulo superior (simetrico) + diagonal
    std::vector<std::string> skipped;    // mismo criterio "best effort" que GreeksReport
};

// factors vacio = misma enumeracion de candidatos que compute_all_greeks (Fase 1: ModelParameter);
// no vacio = exactamente esos factores, en ese orden (para pedir un sub-Hessiano concreto).
HessianReport compute_hessian(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    const std::vector<RiskFactor>& factors = {}
);

struct HvpComponent {
    RiskFactor factor;
    double value = 0.0;   // componente de H*v en la posicion de `factor`
    GreekMethod method_used = GreekMethod::BumpAndReval;
};

struct HvpReport {
    std::vector<HvpComponent> components;
    std::vector<std::string> skipped;
};

// direction.size() == factors.size(), mismo orden -- Hv = H * direction.
HvpReport compute_hvp(
    const Registries& registries, const std::string& metric_name, const Params& metric_params,
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution,
    const std::vector<RiskFactor>& factors, const std::vector<double>& direction
);
```

`GreekMethod` gana dos valores nuevos (no reemplaza los existentes): `LikelihoodRatioHessian`
(Fase 1, motor de payoff) y `AadForwardOverForward` (Fase 2/3, Hull-White vía `Dual2`/`HyperDual`
nuevos) — nombres deliberadamente distintos de `Pathwise`/`AadReverse` para que
`GreekResult::method_used`/`HessianEntry::method_used` sigan siendo auto-explicativos (PLAN_GREEKS.md
§13) sobre CUÁL de los tres mecanismos (verosimilitud, forward-over-forward cerrado, bump-and-reval)
produjo cada número — nunca "aad" a secas para algo que no pasó por Burn.

### 7.1 Tabla de capacidades (mismo patrón que `pathwise2_capabilities()`)

```text
hessian_capabilities():
    {"GBM", "PayoffPriceQ"}      -> LikelihoodRatioHessian (Fase 1, solo factores en {spot,volatility})
    {"GBM_P", "PayoffForecastP"} -> LikelihoodRatioHessian (idem)
    {"HullWhite1F", "HullWhiteModelNpv"} -> AadForwardOverForward (Fase 2)
    {"HullWhite2F", "HullWhiteModelNpv"} -> AadForwardOverForward (Fase 2, excluye pares con "rho"
                                             hasta que se derive esa parcial -- mismo criterio que
                                             PLAN_GREEKS.md §5.2 con rho fuera de AAD reverse)
```

Cualquier combinación fuera de esta tabla cae a un estencil de bump-and-reval genérico
`(N+1)` evaluaciones extra reutilizando el patrón de 3/4 puntos que `compute_greek` ya implementa
para una única entrada (§8, Fase 4 opcional) — nunca un error si `method=Auto`.

## 8. Diseño de API — Excel y Python

Investigado el patrón EXISTENTE antes de diseñar (no se inventa un mecanismo de marshalling nuevo):
hoy, un único Greek NO tiene una función dedicada — es un `Measure` más (`Greek`, con
`.to_params()` aplanado a `{"metric","risk_factor","order","method",...}`) que pasa por el pipeline
genérico `Engine.price(product, [measure], ...)`; el barrido (`all_greeks`) SÍ es una función
dedicada porque produce una lista de longitud variable, y en Excel/C ABI toda salida
multi-valor (batches, grids, buckets de curva) usa la MISMA convención: **tabla en formato largo**,
nunca una matriz anidada. Un Hessiano (pares `(i,j)`) y un HVP (un vector) encajan exactamente en
esa convención sin inventar nada — una fila por entrada.

### 8.1 Python (`clients/python/src/engine_typed/`)

```python
# greeks.py -- nuevo, sigue el patron de Greek/all_greeks ya existente en el mismo modulo

@dataclass
class HessianEntry:
    factor_i: str
    factor_j: str
    value: float
    std_error: float | None
    method_used: str
    measure: str

@dataclass
class HessianReport:
    entries: list[HessianEntry]
    skipped: list[str]

@dataclass
class HvpComponent:
    factor: str
    value: float
    method_used: str

@dataclass
class HvpReport:
    components: list[HvpComponent]
    skipped: list[str]

# Engine (engine.py), junto a Engine.all_greeks ya existente:
class Engine:
    def hessian(
        self, product, metric_name: str, model, market, pricing, execution,
        metric_params: dict | None = None, risk_factors: list[str] | None = None,
    ) -> HessianReport: ...

    def hvp(
        self, product, metric_name: str, model, market, pricing, execution,
        direction: dict[str, float],   # {"model.spot": 1.0, "model.volatility": 0.0, ...}
        metric_params: dict | None = None,
    ) -> HvpReport: ...
```

`risk_factors=None` reutiliza la MISMA enumeración de candidatos que `all_greeks` (§7,
`compute_hessian` con `factors={}`); `direction` es un dict disperso (factores ausentes = 0) para
que pedir un HVP no obligue a enumerar todos los factores del modelo.

### 8.2 Excel (`clients/excel/`)

Dos UDFs nuevas junto a `ENGINE.ALL_GREEKS` (`engine_excel.cpp`), mismo patrón de argumentos
(`trade, metrica, parametros_metrica, modelo, mercado, contexto, ejecucion, ...`) y misma
convención de tabla larga que el resto del fichero:

```text
=ENGINE.HESSIAN(trade, metrica, parametros_metrica, modelo, mercado, contexto, ejecucion, [factores_de_riesgo])
    -> tabla: [RiskFactorI, RiskFactorJ, Value, Method, Measure, StdError]
       (una fila por par i<=j; factores_de_riesgo opcional, rango vertical de strings "model.spot" etc.
        vacio = enumeracion automatica, igual que ENGINE.ALL_GREEKS)

=ENGINE.HVP(trade, metrica, parametros_metrica, modelo, mercado, contexto, ejecucion, direccion)
    -> tabla: [RiskFactor, Value, Method]
       (direccion: rango de 2 columnas [RiskFactor, Peso], filas ausentes = peso 0)
```

### 8.3 C ABI (`cpp/engine/include/engine/abi.h`)

Mismo molde exacto que `EngineGreekResultEntry`/`engine_abi_all_greeks` (struct owned-copy +
función que rellena `out_entries`/`out_count` + `free`):

```c
typedef struct EngineHessianEntry {
    char* risk_factor_i;
    char* risk_factor_j;
    double value;
    int has_std_error; double std_error;
    char* method_used;
    char* measure;
} EngineHessianEntry;

ENGINE_ABI_API int engine_abi_hessian(
    const EngineProduct* product, const char* metric_name,
    const EngineParam* metric_params, size_t n_metric_params,
    const EngineModel* model, const EngineMarketSnapshot* market,
    const EnginePricingContext* pricing, const EngineExecutionContext* execution,
    const char** risk_factors, size_t n_risk_factors,   /* NULL/0 = enumeracion automatica */
    EngineHessianEntry** out_entries, size_t* out_n_entries,
    char*** out_skipped, size_t* out_n_skipped
);
ENGINE_ABI_API void engine_abi_free_hessian(
    EngineHessianEntry* entries, size_t n_entries, char** skipped, size_t n_skipped
);

/* engine_abi_hvp: mismo molde, con (const char** direction_factors, const double* direction_weights,
   size_t n_direction) en vez de risk_factors, y EngineHvpComponent{risk_factor,value,method_used}. */
```

## 9. Plan por fases

### Fase 0 — ADRs

- Confirmar (revisando el pricer cerrado de Hull-White línea a línea, no solo por inspección) que no
  hay ninguna rama (`if`/`min`/`max`) en el camino de `a`/`b`/`sigma`/`r0`/`eta`/`rho` hacia el NPV de
  un swap vainilla — condición necesaria para que §5 sea válido sin corrección de verosimilitud.
- Congelar los dos `GreekMethod` nuevos (`LikelihoodRatioHessian`, `AadForwardOverForward`) y sus
  reglas de tabla de capacidades (§7.1).
- Congelar la convención de Taylor de los `Dual2`/`HyperDual` NUEVOS de Hull-White (misma que
  PLAN_HYPERDUAL.md ADR-HD-02: coeficiente sin normalizar, corrección en el borde).

### 9.0 ADRs (Fase 0, cerrados)

**ADR-BW-01 (verificación línea a línea: el pricer cerrado de Hull-White no tiene ramas
dependientes de `a`/`b`/`sigma`/`r0`/`eta`/`rho`)**: se revisó línea a línea el camino completo desde
los parámetros de modelo hasta el NPV de un swap vainilla, en los cuatro sitios que lo componen, y
**no se encontró ninguna rama** (`if`/`min`/`max`/`clamp` o equivalente) cuya decisión dependa de
`a`/`b`/`sigma`/`r0`/`eta`/`rho` — es decir, mover cualquiera de estos parámetros infinitesimalmente
nunca cruza una frontera de rama en este camino. Referencias concretas revisadas:
`rust/crates/engine-core/src/models/hull_white.rs::b_factor` (líneas 51-55), `::a_factor` (57-69) y
`::zero_coupon_bond` (75-80) — solo `mul_scalar`/`add_scalar`/`div`/`exp`/`neg`/multiplicación entre
tensores, cero `if`/`min`/`max`; `rust/crates/engine-core/src/models/hull_white_2f.rs::b_factor`
(58-62), `::variance_term` (66-75, incluye `powf_scalar(-1.0)` con exponente CONSTANTE, no una rama)
y `::cross_term` (79-91) y `::zero_coupon_bond` (96-106) — misma conclusión, cero ramas;
`rust/crates/engine-core/src/products/irs.rs::IrSwap::npv` (46-68) — sin ningún `if`/`min`/`max` en
absoluto, solo sumas/productos de bonos cero-cupón y un `.reduce()` sobre las fechas de pago (fechas
fijas, no parámetros de modelo); `rust/crates/engine-core/src/api.rs::irs_hull_white_npv_all_greeks`
(515-548) e `::irs_hull_white_2f_npv_all_greeks` (999-1038), junto con sus constructores auxiliares
`build_irs_swap` (38-62) y `build_irs_swap_2f` (556-580). Estos dos constructores SÍ contienen un
`if use_par_rate { ... } else { ... }` (líneas 49-53 y 567-571 respectivamente), pero no cuenta como
rama relevante: `use_par_rate` es un `bool` fijo del llamante (no una función continua de
`a`/`b`/`sigma`/`r0`/`eta`/`rho`), la decisión se evalúa UNA vez sobre el modelo "plano" (`plain_model`,
sin `require_grad()`) para fijar una constante (`fixed_rate`) ANTES de construir el modelo
diferenciado (`diff_model`), y esa constante no vuelve a evaluarse dentro de la cinta que se deriva
— mover `a`/`b`/`sigma`/`r0`/`eta`/`rho` con `use_par_rate` fijo nunca cruza esta rama. Los únicos
otros `if`/comparaciones del módulo (`IrSwap::is_reset_date`, `IrSwap::remaining_from`,
`debug_assert!(t <= self.start + 1e-9)`) comparan fechas/tiempos fijos entre sí o son aserciones que
no participan en el valor de NPV, nunca parámetros de modelo — fuera del criterio de "kink relevante"
del enunciado de Fase 0. **Conclusión: Fase 0 pasa sin bloqueante — §5 (Hessiano cerrado de
Hull-White vía `Dual2`/`HyperDual` nuevos) es válido tal como está diseñado, no hace falta ninguna
corrección de verosimilitud para este frente.**

**ADR-BW-02 (`GreekMethod::LikelihoodRatioHessian`/`AadForwardOverForward` y regla de la tabla de
capacidades de §7.1)**: se congelan dos valores nuevos de `GreekMethod` — `LikelihoodRatioHessian`
(Fase 1, motor de payoff, likelihood ratio sin AD) y `AadForwardOverForward` (Fase 2/3, Hull-White
vía `Dual2`/`HyperDual` nuevos) — deliberadamente distintos de `Pathwise`/`AadReverse` ya existentes,
para que `HessianEntry::method_used`/`HvpComponent::method_used` sigan siendo auto-explicativos
(PLAN_GREEKS.md §13) sobre CUÁL de los tres mecanismos (verosimilitud, forward-over-forward cerrado,
bump-and-reval) produjo cada número. La tabla de capacidades de `hessian_capabilities()` (§7.1) queda
fijada exactamente como: `{"GBM","PayoffPriceQ"}` y `{"GBM_P","PayoffForecastP"}` →
`LikelihoodRatioHessian` (solo factores en `{spot, volatility}`, Fase 1); `{"HullWhite1F",
"HullWhiteModelNpv"}` → `AadForwardOverForward` (Fase 2); `{"HullWhite2F","HullWhiteModelNpv"}` →
`AadForwardOverForward` excluyendo cualquier par que incluya `"rho"` (Fase 3, mismo criterio que
PLAN_GREEKS.md §5.2 con `rho` fuera de AAD reverse). Se congela además, sin excepción, que **cualquier
combinación `(modelo, métrica)` fuera de esta tabla cae a un estencil de bump-and-reval genérico
`(N+1)`/3-4 puntos** (el mismo patrón que `compute_greek` ya usa para una única entrada) cuando
`method=Auto` — nunca un error bajo `Auto`; solicitar explícitamente `method=LikelihoodRatioHessian`
o `method=AadForwardOverForward` sobre una combinación no soportada sí es un error inmediato con la
razón exacta (mismo criterio que el resto del motor de Greeks, PLAN_GREEKS.md §5.3/§5.4).

**ADR-BW-03 (convención de Taylor de los `Dual2`/`HyperDual` NUEVOS de Hull-White: misma convención
de PLAN_HYPERDUAL.md ADR-HD-02, reutilizada sin cambios)**: los `Dual2`/`HyperDual` NUEVOS que se
construyan en Fase 2/3 para el pricer cerrado de Hull-White (`models::hull_white_dual` o módulo
equivalente, §5) usan **exactamente** la misma convención de Taylor sin normalizar que
PLAN_HYPERDUAL.md ADR-HD-02: el campo de segunda derivada (`d2` en `Dual2`, `dxy` en `HyperDual`)
guarda el coeficiente de Taylor tal cual aparece en `f(x+ε) = f(x) + f'(x)ε + f''(x)/2!·ε² + ...`
(`f''(x)/2`, sin multiplicar por 2; la mixta `∂²f/∂x∂y` sin normalizar, sin factorial porque no lleva
ninguno en la serie de Taylor bivariada), y solo `second_derivative()`/`cross_derivative()` aplican la
corrección al leer el resultado final — nunca dentro de la aritmética (`Mul`, `exp`, `ln`, `powf`).
Esto NO es una convención nueva a definir: es la MISMA de ADR-HD-02, reutilizada sin modificación,
precisamente porque es la convención la que hace que el producto de Cauchy truncado sea exactamente
la composición de Taylor sin factores de corrección en cada operación intermedia — una propiedad
puramente algebraica del truncamiento, independiente de qué función se esté derivando. Que sea
correcto aplicarla aquí no es un supuesto nuevo: depende únicamente de que la función a derivar sea
analítica y sin ramas dependientes de los parámetros derivados, que es exactamente lo que ADR-BW-01
acaba de confirmar para el pricer cerrado de Hull-White (a diferencia del motor de payoff, donde la
misma convención — con la aritmética dual, no la convención en sí — daba una segunda derivada
degenerada por el kink del payoff, PLAN_HYPERDUAL.md §5.0). Los tipos concretos (`Dual2`/`HyperDual`
de Hull-White) son NUEVOS structs, propios de `models::hull_white_dual`, sin relación de código con
los `Dual2`/`HyperDual` abandonados de `payoff::dual` (que nunca llegaron a implementarse) — solo
comparten la convención de Taylor y el patrón de producto de Cauchy truncado, no ningún tipo ni
módulo.

### Fase 1 — Hessiano local del motor de payoff (likelihood ratio, sin AD)

- `payoff::lrm`: añadir `volga_weight` (ya derivado y verificado en PLAN_HYPERDUAL.md/esta
  investigación) y una función `local_hessian_weights(z, s0, sigma, t) -> {gamma, volga, vanna}` que
  reutiliza una única `Z` recuperada por ruta.
- `payoff::api::payoff_local_hessian_gbm_q`/`api_p::..._p`: UNA simulación, un `HashMap`/struct con
  las 3 entradas — sustituye (o complementa, mientras migran los llamantes) a las llamadas
  independientes de `payoff_sensitivity2_gbm_q`/`payoff_sensitivity_cross_gbm_q`.
- `greeks.cpp`: `try_hessian_likelihood_ratio` (patrón de `try_pathwise2`), cableado en
  `compute_hessian` para `(GBM,PayoffPriceQ)`/`(GBM_P,PayoffForecastP)`.
- Multi-activo (§4.2): documentar la fórmula general en el código (no implementar — no hay payoff
  multi-activo hoy); dejar como Fase 1.5 explícitamente condicionada a que aparezca un payoff
  basket/spread real.

**Aceptación**: Gamma/Volga/Vanna de una call vía `payoff_local_hessian_gbm_q` coinciden (dentro de
tolerancia) con las versiones actuales de un-Greek-por-llamada (`payoff_sensitivity2_gbm_q`/
`payoff_sensitivity_cross_gbm_q`, que se retiran una vez migrados los llamantes) Y con
Black-Scholes cerrado; el coste (paths simulados) de pedir las 3 entradas es el mismo que pedir 1
sola (verificado con un contador de simulaciones, mismo espíritu que `mul_count` de
PLAN_HYPERDUAL.md §8.4).

### Fase 2 — Hessiano cerrado de Hull-White 1F (`Dual2`/`HyperDual` nuevos)

- Nuevo módulo `models::hull_white_dual` (o similar): reimplementación escalar del pricer cerrado de
  bono cupón cero / NPV de swap, parametrizada sobre `T: DualNumber` (reutiliza el TRAIT de
  PLAN_HYPERDUAL.md Fase 1, no reutiliza `Dual2`/`HyperDual` del payoff — esos ya no existen).
  Verificación obligatoria: mismo NPV/Delta que la ruta Burn existente, dentro de tolerancia
  numérica (no solo el Hessiano — el valor y el gradiente de esta segunda implementación deben
  coincidir con la primera, o hay dos pricers divergentes, un riesgo peor que no tener Hessiano).
- `try_hessian_forward_over_forward` en `greeks.cpp`, cableado para `HullWhite1F`.

**Aceptación**: Hessiano 4x4 de Hull-White 1F coincide con un estencil de bump-and-reval de 3/4
puntos por par (10 pares) dentro de tolerancia declarada; NPV/Delta de la nueva implementación
escalar coinciden con los de la implementación Burn existente.

### Fase 3 — Hessiano cerrado de Hull-White 2F + HVP trivial

- Extiende Fase 2 a los 6 parámetros de 2F (21 pares), documentando `rho` fuera de la tabla si su
  parcial no se deriva en esta pasada (mismo criterio que AAD reverse hoy).
- `compute_hvp` para Hull-White: multiplicar la matriz Hessiana de Fase 2/3 por `direction` — sin
  Pearlmutter, sin necesitar reverse-mode (N pequeño, §5).

**Aceptación**: mismo criterio que Fase 2, extendido a 2F; `Hv` coincide con diferencias finitas
direccionales de segundo orden.

### Fase 4 (opcional) — estencil de bump-and-reval genérico para Hessiano/HVP fuera de tabla

- Para cualquier `(modelo,métrica)` no cubierta por Fase 1-3: `compute_hessian`/`compute_hvp` caen a
  N(N+1)/2 pares evaluados con el estencil de 3/4 puntos que `compute_greek` ya tiene por entrada —
  mecánico, sin necesitar tabla de capacidades nueva, mismo principio de "nunca fallar, degradar a
  bump-and-reval" que el resto del motor de Greeks.

### Fase 6 — `Portfolio` first-class (lista de trades, agregación por suma)

- `engine::Portfolio` (`engine/portfolio.hpp`, §6.4): contenedor de `IProduct`, envoltorio fino sobre
  `price_many` ya existente para `Portfolio::price`.
- `Portfolio::hessian`/`Portfolio::hvp`: suman `HessianReport`/`HvpReport` (§7) por trade — requiere
  que Fase 1-4 ya existan (se apoya en `compute_hessian`/`compute_hvp` por trade).
- C ABI (`engine_abi_create_portfolio`/`_add_trade`/`_size`/`_free`, `engine_abi_portfolio_price`/
  `_hessian`/`_hvp`), Python (`engine_typed.portfolio.Portfolio`), Excel
  (`ENGINE.PORTFOLIO.CREATE`/`.ADD`/`.PRICE`/`.HESSIAN`/`.HVP`) — mismo patrón de handles/tabla
  larga que el resto del motor (§8).
- Alcance explícitamente mínimo (§6.4): un único `IModel`/`MarketSnapshot` por `Portfolio`, sin
  netting/colateral/multi-moneda — "lista de trades", tal cual se pidió.

**Aceptación**: `Portfolio.hessian()` de una cartera de 2-3 trades bajo el MISMO modelo coincide,
entrada a entrada, con sumar a mano los `HessianReport` de `compute_hessian` llamado trade a trade
(identidad exacta, no una tolerancia estadística — es una suma, no una aproximación);
`Portfolio.price()` coincide con `price_many` sobre el mismo vector de trades; añadir/quitar un
trade del `Portfolio` no requiere reconstruir ni reconfigurar nada del lado de `IModel`/
`MarketSnapshot`.

### Fase 7 (fuera de alcance de este documento, solo referenciada) — curva/pilares y multi-activo

Ver §6 (Nivel A: reverse-mode de curva por trade, candidato de un plan de Greeks de curva propio; y
Nivel B: Hessiano/HVP de cartera fusionado, bloqueado por la Fase 4 de `ARCHITECTURE_REVIEW.md`
—`Portfolio`/`NettingSet`, hoy sin empezar) y §4.3 (Malliavin de trayectoria completa). No se
planifica aquí por falta de caso de negocio concreto ni de la pieza de arquitectura de la que
depende (Nivel B) — ver §12.

## 10. Verificación obligatoria

Misma disciplina que PLAN_GREEKS.md §5.3/PLAN_HYPERDUAL.md §6: ninguna entrada de
`hessian_capabilities()` se sirve bajo `method=Auto` sin un test diferencial en verde:

- Fase 1: `local_hessian_weights` verificado por CUADRATURA DETERMINISTA (como `gamma_weight`/
  `vanna_weight` en PLAN_HYPERDUAL.md) antes de Monte Carlo end-to-end.
- Fase 2/3: la implementación escalar `Dual2`/`HyperDual` de Hull-White verificada, PRIMERO, en
  valor y gradiente (orden 0 y 1) contra la implementación Burn ya en producción — si estas dos
  divergen, el Hessiano de una tercera fuente no sirve de nada; SEGUNDO, el Hessiano contra
  bump-and-reval con tolerancia declarada.
- Cualquier combinación sin test en verde se sirve solo bajo `method` explícito, nunca bajo `Auto`.

## 11. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Asumir que forward-over-reverse/reverse-over-forward arreglan los kinks por sí solos | §2, dicho una vez con precisión y citado — ninguna Fase de este plan aplica AD de ningún orden al motor de payoff |
| Confundir "hay pocos inputs, forward vale" (Hull-White) con "hay pocos inputs por trade pero muchos trades" (portfolio) | §1.1 deja explícito que hoy no hay grafo compartido entre trades — el ahorro de Fase 1/2/3 es POR TRADE, agregación de cartera sigue siendo una suma de resultados independientes, ya soportada |
| Depender de que Burn añada doble-`backward` sin confirmarlo | §1.2: spike ejecutado y documentado, no se asume nada sobre Burn futuro; Fase 2/3 no dependen de Burn en absoluto |
| Implementar `Dual2`/`HyperDual` de Hull-White con el mismo bug de "asumir que no hay kinks" sin comprobarlo | Fase 0 exige revisar el pricer cerrado línea a línea antes de escribir aritmética dual — no se repite el error de PLAN_HYPERDUAL.md de asumir sin verificar |
| La generalización multi-activo de §4.2 esconde un error de signo/factor como el que casi se comete con la Gamma "propia" correlacionada | Documentado explícitamente en §4.2 con el valor numérico exacto del error (factor ~2) que se habría colado sin la verificación por cuadratura — cualquier implementación futura de §4.2 DEBE repetir esa verificación antes de confiar en la fórmula |
| Construir el grafo diferenciable de curva (§6) sin un caso de negocio real | §6/§12: explícitamente fuera de alcance hasta que exista una necesidad concreta (número de pilares, frecuencia de recálculo) que justifique el coste |
| Confundir el `Portfolio` de orquestación (Fase 6, §6.4) con el `Portfolio(children)` del AST de `ARCHITECTURE_REVIEW.md` y duplicar esfuerzo | §6.4 los distingue explícitamente por capa (orquestación de trades ya construidos vs. composición de cashflows dentro de un contrato) y deja escrito que uno puede convertirse en fachada del otro, nunca que compitan |
| `Portfolio.hessian()`/`.hvp()` sumando resultados de trades que en realidad necesitan modelos distintos (cartera heterogénea) | Fase 6 exige explícitamente UN `IModel`/`MarketSnapshot` compartido — una cartera multi-modelo se rechaza o queda fuera de alcance, nunca se suma incorrectamente en silencio |

## 12. Fuera de alcance

- Grafo diferenciable de descuento de curva y Hessiano/HVP de pilares (§6) — el Nivel A (reverse-mode
  por trade) no tiene caso de negocio confirmado todavía (fixtures de test usan 1-3 pilares, no
  20-50); el Nivel B (fusión de cartera) depende además de la Fase 4 de `ARCHITECTURE_REVIEW.md`
  (`Portfolio`/`NettingSet`), que no está ni empezada.
- Malliavin calculus de trayectoria completa para payoffs path-dependientes (barreras, triggers,
  `Exercise`) (§4.3) — investigación real, no planificado.
- Motor de payoff multi-activo (baskets/spreads/rainbow) — no existe hoy (`Gbm` es de un único
  observable); §4.2 deja la fórmula lista para cuando exista, no construye el modelo multi-activo.
- Parcial de `rho` de Hull-White 2F (tanto en AAD reverse existente como en el Hessiano cerrado de
  Fase 3) — mismo criterio que PLAN_GREEKS.md §5.2, sin cambios.
- Esperar/depender de que Burn añada soporte de segundo orden — si ocurre, es una optimización
  posterior verificable contra Fase 2/3 como oráculo, no un prerrequisito de este plan.
- Complicaciones de `Portfolio` más allá de "lista de trades bajo un único modelo" — netting,
  colateral, multi-moneda, multi-modelo, y la fusión con el `Portfolio(children)` del AST de
  `ARCHITECTURE_REVIEW.md` — pospuestas explícitamente a un plan futuro dedicado (§6.4/§6.5).

## 13. Definition of Done

- Fase 1 (payoff, likelihood ratio): Gamma/Volga/Vanna de una call vía una única simulación
  compartida, verificadas por cuadratura determinista y Monte Carlo end-to-end, coste de simulación
  medido igual con 1 o 3 salidas pedidas.
- Fase 2/3 (Hull-White, `Dual2`/`HyperDual` nuevos): Hessiano 1F/2F correcto contra bump-and-reval,
  con la implementación escalar verificada primero en valor/gradiente contra la ruta Burn existente;
  HVP trivial (multiplicación matriz-vector) disponible.
- Ninguna combinación se sirve bajo `method=Auto` sin su test diferencial correspondiente (§10).
- El motor de payoff sigue sin usar AD de ningún tipo para Gamma/Vanna/Volga — el Hessiano local
  llega por reponderación, no por diferenciación repetida.
- API Python/Excel/C ABI nuevas (§8) siguen exactamente las convenciones ya establecidas (measure
  spec + tabla larga) — cero mecanismo de marshalling nuevo.
- Fase 6 (`Portfolio`): objeto first-class disponible en C++/Python/Excel/C ABI, `Portfolio.price()`
  coincide con `price_many` sobre el mismo vector de trades, `Portfolio.hessian()`/`.hvp()`
  coinciden EXACTAMENTE (no con tolerancia) con la suma manual de `compute_hessian`/`compute_hvp`
  trade a trade — la ventaja de "una llamada para el Hessiano de una cartera" está disponible sin
  esperar a ningún grafo fusionado (Nivel A/B de §6.3 quedan como optimización interna futura, sin
  cambiar esta interfaz pública).

---

*Decisión central: la pregunta "¿forward, reverse, o un híbrido?" es una pregunta sobre CÓMO abaratar
una derivada de segundo orden con muchos parámetros — nunca sobre si esa derivada, calculada por
diferenciación repetida, es la respuesta correcta. Para el motor de payoff (con kinks), la respuesta
correcta nunca fue "elegir mejor el orden de composición de AD": fue diferenciar la densidad en vez
del payoff, un mecanismo que además resulta que ya da "una pasada, muchas derivadas" gratis, sin
necesitar ningún grafo de AD. Para Hull-White (sin kinks, pocos parámetros), forward-over-forward
con números duales de segundo orden — la idea original de PLAN_HYPERDUAL.md — es correcta y barata
en su sitio correcto. El verdadero caso "muchos inputs, sin kinks" (pilares de curva) es real pero
no tiene hoy ninguna infraestructura de AD sobre la que apoyarse — construirla es un proyecto propio,
no una fase de este documento. Lo que SÍ se construye ya, en la misma dirección: un `Portfolio`
first-class que da la ventaja de "una llamada, Greeks de una cartera" hoy mismo, por simple suma
exacta de resultados por trade — dejando el sitio correcto, sin comprometer la interfaz pública, para
cuando el Nivel A o el Nivel B lleguen a existir.*
