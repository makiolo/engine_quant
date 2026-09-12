# Revisión arquitectónica: hacia una librería quant extensible y composable

## 1. Resumen ejecutivo

La evolución propuesta es viable, pero no conviene reescribir el motor actual. El repositorio ya
contiene varias piezas difíciles de construir: un núcleo numérico en Rust genérico sobre Burn, una
selección CPU/GPU, autodiferenciación, Monte Carlo vectorizado, batching, calibración, un registry
explícito y fachadas para C++, Python, C y Excel.

La principal limitación no es de rendimiento, sino de **representación del producto**. Hoy
`IRSwap` es una estructura específica y su valoración contiene directamente la fórmula del
producto (`rust/crates/engine-core/src/products/irs.rs`). Para hacer que cualquier producto sea
composable, el producto debe convertirse en un **programa declarativo**: un AST/IR inmutable que
describe eventos, observaciones de mercado, condiciones y pagos. Un compilador de ese programa
generará un plan de eventos y una representación de cashflows que el motor podrá evaluar en una
fecha, en una malla de escenarios o sobre una cartera completa.

La recomendación es mantener las capas actuales y añadir una nueva frontera:

```text
Python / C ABI / Excel / Rust
              │
        ProductSpec / JSON
              │
   AST tipado e inmutable de producto
              │
  compilación: validación + schedule + dependencias
              │
    EventPlan + Cashflow IR columnar
              │
  evaluación vectorizada sobre escenarios
              │
       descuento, agregación y riesgo
```

El AST debe ser un **IR de datos**, no una jerarquía de objetos virtuales ejecutada dentro del
bucle Monte Carlo. Los nodos pueden ser ricos semánticamente, pero el hot path debe operar sobre
arrays contiguos y kernels de Rust.

## 2. Qué existe actualmente

### 2.1 Componentes consolidados

| Área | Estado actual | Evidencia |
| --- | --- | --- |
| Orquestación | Registry explícito para modelos, productos, measures y calibradores | `cpp/engine/include/engine/registry.hpp`, `bootstrap.hpp` |
| Productos | `IRSwap` con soporte escalar y lote homogéneo | `rust/crates/engine-core/src/products/irs.rs` |
| Modelos | Hull-White 1F y 2F mediante `ShortRateModel<B>` | `rust/crates/engine-core/src/models/` |
| Mercado | `Curve`/`MarketSnapshot`, discounting e inputs de crédito | `rust/crates/engine-core/src/curve.rs`, `cpp/engine/include/engine/market.hpp` |
| Monte Carlo | Euler-Maruyama, paths vectorizados y perfiles EE/PFE | `rust/crates/engine-core/src/kernel.rs`, `exposure.rs` |
| Sensibilidades | Autodiff de Burn y comparación contra bump-and-reval | `rust/crates/engine-core/src/backend.rs`, `tests/aad_vs_bump_reval.rs` |
| Batching | `price_batch`, `price_many` y `price_grid` | `cpp/engine/include/engine/price.hpp`, `src/price.cpp` |
| Clientes | Python tipado/dinámico, C ABI, C++ y Excel | `clients/`, `cpp/engine/include/engine/abi.h` |
| Distribución | CMake + Corrosion + Cargo; CI Windows/Linux | `CMakeLists.txt`, `.github/workflows/ci.yml` |

Es una base adecuada para el objetivo. En particular, la genericidad `B: Backend` permite
reutilizar un evaluador entre CPU, GPU y autodiff, y el batching ya establece el patrón correcto
de compartir escenarios entre trades.

### 2.2 Límites actuales relevantes

1. **Producto y valoración están acoplados.** `IrSwap::npv` conoce el calendario y la fórmula.
   No existe una representación intermedia común para un swap, un bono, un forward o una opción.
2. **No hay un modelo explícito de eventos.** `pricing_date` es actualmente metadato; no existen
   calendarios, day-count, fixing, settlement, estados de ejercicio o reglas de observación.
3. **No se materializan cashflows como resultado primario.** Se calcula NPV directamente y la
   exposición revalora el IRS en fechas de reset. Esto dificulta auditar, explicar y recombinar
   productos.
4. **El mercado es principalmente un snapshot.** `MarketSnapshot` contiene una curva y crédito,
   pero no un grafo de curvas, fixings, superficies, FX, dividendos, colateral o convenciones.
5. **El Monte Carlo está especializado.** `exposure.rs` sabe cómo simular Hull-White y cómo
   revalorar el IRS; falta separar escenarios, evaluación de producto y agregación de riesgo.
6. **`Params` es útil como frontera dinámica, pero débil como contrato interno.** Es un mapa de
   valores sin esquema, versionado, unidades ni diagnóstico estructural.
7. **Las interfaces C++ son extensibles, pero el hot path no debe depender de polimorfismo.** El
   registry resuelve tipos al construir, pero una evaluación por nodos virtuales en millones de
   escenarios sería costosa y difícil de trasladar a GPU.
8. **La cartera todavía no es una entidad de primera clase.** Existen lotes de trades, pero no
   netting sets, colateral, dependencias compartidas, cachés de escenarios ni agregación por
   moneda/contrapartida.

## 3. Arquitectura objetivo

### 3.1 Separar cuatro responsabilidades

La arquitectura debería distinguir explícitamente:

1. **Especificación:** qué producto se desea construir.
2. **Compilación:** validación, expansión de schedules y resolución de dependencias.
3. **Evaluación:** ejecución del programa para un estado de mercado y un escenario.
4. **Medición:** descuento, exposición, CVA, sensibilidades y agregación.

Un producto no debería saber si se ejecutará para una fecha puntual, para 100.000 paths o dentro
de un netting set. La misma especificación debe poder compilarse a distintos planes de evaluación.

### 3.2 Product AST/IR

El AST debe ser inmutable, serializable y versionable. Conviene distinguir tres familias:

#### Expresiones de valor

Expresiones puras que producen un escalar, vector de escenarios o serie temporal:

- constantes y parámetros del trade;
- suma, resta, producto, división y negación;
- `max`, `min`, `abs`, `if`;
- observaciones de mercado: discount factor, forward, fixing, FX spot, volatilidad;
- observaciones de estado: factor de modelo, índice, barrera o fixing ya realizado;
- funciones de fecha: fracción de año, tiempo restante, fecha de pago.

#### Comandos/eventos

Eventos que transforman el estado del programa o producen obligaciones:

- `At(date, body)`;
- `When(condition, then, otherwise)`;
- `Until(date, body)`;
- `Fix(index, date)`;
- `Pay(currency, amount, settlement)`;
- `Receive(currency, amount, settlement)`;
- `Exchange(currency_a, amount_a, currency_b, amount_b)`;
- `Exercise`/`Cancel` para productos con decisión;
- `Accrue` para intereses entre dos eventos.

#### Composición

Operadores que permiten construir productos sin introducir un tipo nuevo para cada combinación:

- `Add(program_a, program_b)`;
- `Scale(notional, program)`;
- `WithSchedule(schedule, body)`;
- `InCurrency(currency, program)`;
- `Portfolio(children)`;
- `NettingSet(children, collateral_spec)`.

Los nombres son orientativos. Lo importante es que un producto concreto sea una composición de
primitivas, y que `IRSwap` sea un constructor de alto nivel que genera ese AST. El constructor
puede conservar una API cómoda, pero la valoración no debe tratarlo como un caso especial.

### 3.3 Estado y semántica temporal

Cada evaluación debe distinguir:

- **tiempo de valoración** `t`;
- **estado histórico**, que contiene fixings y eventos ya liquidados;
- **estado simulado**, que contiene factores de riesgo en el escenario;
- **estado de contrato**, que contiene ejercicio, cancelación o barreras;
- **entorno de mercado**, que resuelve observaciones.

La semántica recomendada es funcional: un nodo recibe `(t, state, market, scenario)` y devuelve
un nuevo estado más una secuencia de efectos. No se debe mutar un objeto compartido desde varios
paths. Esto facilita reproducibilidad, paralelismo y autodiff.

Los eventos deben estar ordenados y tener reglas claras para:

- eventos simultáneos;
- fechas fuera de orden;
- calendarios y ajustes de días hábiles;
- fixings conocidos frente a fixings simulados;
- pagos en una moneda distinta de la moneda de valoración;
- ejercicio y cancelación;
- datos faltantes.

## 4. Cashflow IR

El resultado semántico de ejecutar un programa debe ser una colección de cashflows, no únicamente un
`f64` de NPV. Un registro lógico mínimo sería:

```text
Cashflow {
    payment_time,
    settlement_time,
    currency_id,
    amount_expression,
    sign,
    trade_id,
    leg_id,
    event_id,
    netting_set_id
}
```

En el runtime, `amount_expression` se compila a una columna de valores. Para un cálculo escalar
puede ser un `f64`; para Monte Carlo será una matriz lógica
`[scenario, cashflow]` o una forma equivalente por bloques.

El pipeline de valoración sería:

1. construir el AST;
2. validar tipos, fechas, monedas y referencias;
3. expandir schedules y producir un `EventPlan`;
4. compilar las expresiones a un `CashflowPlan`;
5. evaluar importes por escenario;
6. convertir cada importe a la moneda de valoración;
7. descontar con la curva apropiada;
8. agregar por trade, netting set, cartera y medida.

Esta separación aporta auditabilidad: se puede inspeccionar qué pagos generó un producto antes de
preguntar por PV. También permite reutilizar los mismos cashflows para PV, DV01, PFE, CVA,
contabilidad o reporting.

### Decisiones importantes

- Mantener `payment_time` y `settlement_time` separados.
- Representar currency y referencias de mercado mediante IDs internados, no strings en el hot
  path.
- Preservar el origen (`event_id`, `leg_id`) para explicar resultados.
- No materializar siempre una matriz completa `[paths, cashflows]`: usar evaluación por chunks y
  liberación temprana cuando el plan lo permita.
- Mantener una ruta determinista para PV y otra de escenarios para exposición, compartiendo el
  mismo plan.

## 5. Diseño orientado a datos

### 5.1 Representación

La representación pública puede ser un árbol cómodo, pero el runtime debe transformarlo a una
forma normalizada:

- nodos almacenados en arrays por tipo;
- referencias como índices enteros;
- strings, monedas, curvas e índices internados en tablas;
- schedules en columnas (`start`, `end`, `payment`, `accrual`, `calendar_id`);
- cashflows en estructuras SoA (Structure of Arrays);
- lotes agrupados por forma, moneda, modelo y conjunto de dependencias.

Esto evita punteros dispersos y permite a Burn procesar bloques completos. El AST es la forma de
autoría; el `EventPlan` y el `CashflowPlan` son la forma de ejecución.

### 5.2 Forma de los datos

Las dimensiones deben documentarse y ser consistentes. Una convención razonable es:

```text
[trade, event]                 plan estático
[scenario, trade, event]       evaluación de escenarios
[time, scenario, trade]        exposición a lo largo del tiempo
[netting_set, scenario, time]  agregación XVA
```

No se debe añadir una dimensión implícita. Toda transposición debe ocurrir en una frontera
conocida. Para GPU, los kernels deben consumir bloques homogéneos; para CPU, los bloques deben
ser suficientemente grandes para amortizar dispatch y asignaciones.

### 5.3 Caché y dependencias

El compilador debe producir un grafo de dependencias:

```text
Trade AST
  ├── schedule/calendar
  ├── market curves
  ├── fixings
  ├── model factors
  └── measure
```

La clave de caché debe incluir versión del AST, snapshot de mercado, parámetros del modelo,
configuración de ejecución y semilla cuando el resultado sea estocástico. Se pueden compartir:

- schedules entre trades con la misma convención;
- escenarios entre todos los trades de un netting set;
- discount factors y forwards;
- subexpresiones puras;
- resultados de una measure entre medidas que los consumen.

## 6. Frontera Rust/C++

### Rust: ejecución numérica

Rust debería ser propietario de:

- `ProductIR`, `EventPlan` y `CashflowPlan` compactos;
- evaluación de expresiones sobre `B: Backend`;
- simulación de factores y shocks;
- evaluación batch de cashflows;
- reducción, cuantiles y agregación;
- autodiff y kernels CPU/GPU.

El trait de modelo actual `ShortRateModel<B>` puede evolucionar hacia capacidades pequeñas y
componibles, por ejemplo discounting, forwarding, FX y volatilidad, sin crear un trait monolítico
para todos los activos. Un producto debería declarar qué capacidades requiere.

### C++: dominio y compatibilidad

C++ debería conservar:

- registry y bootstrap;
- validación de la API pública;
- objetos de compatibilidad (`Params`, `MarketSnapshot`, contextos);
- gestión de handles y ciclo de vida;
- agrupación de trades y traducción de resultados;
- ABI estable y clientes existentes.

No conviene trasladar el AST al C++ como una jerarquía de `virtual` nodes. C++ puede construir o
recibir el DTO y delegar su compilación a Rust. El registry puede registrar constructores de AST,
capabilities y schemas, no implementaciones de valoración duplicadas.

### API pública

Se recomienda añadir una API nueva sin romper `create_product` ni `price`:

```python
swap = q.IRSwap(
    notional=1_000_000,
    fixed_rate=0.025,
    schedule=q.Schedule.annual(start=0, maturity=5),
)

program = q.Portfolio([swap, q.Scale(-1, swap)])
plan = q.compile(program, market_schema=market.schema())
result = q.price(plan, measures=[q.PV(), q.DV01(), q.ExposureProfile()])
```

La API tipada debe generar el mismo DTO/IR que una entrada JSON o la fachada dinámica. Las APIs
anteriores deben seguir funcionando mediante un adaptador que traduzca `IRSwap` al nuevo IR.

## 7. Measures y Monte Carlo composables

Una measure no debería conocer un producto concreto. Debe declarar sus dependencias y consumir
artefactos del pipeline:

```text
PV              -> cashflows + discount factors
DV01            -> PV + bump plan
ExposureProfile -> scenario cashflows + positive-part + aggregation
PFE             -> ExposureProfile + quantile
CVA             -> ExposureProfile + default probabilities + discounting
```

Esto permite compartir una simulación y un perfil entre `EE`, `PFE95`, `CVA` y sensibilidades.
La API debe aceptar medidas parametrizadas, como ya permite `MeasureSpec`, pero los parámetros
deben validarse contra un schema versionado.

El motor Monte Carlo debe tener tres pasos separados:

1. **Scenario engine:** genera estados `[time, scenario]` usando un modelo.
2. **Program evaluator:** ejecuta cualquier `EventPlan` sobre esos estados.
3. **Risk reducer:** calcula cashflows, exposición, cuantiles, CVA y sensibilidades.

Para evitar explosión de memoria:

- ejecutar escenarios por chunks;
- mantener reproducibilidad por seed y rango de chunk;
- usar reducción online para medias y varianzas;
- usar algoritmos de cuantiles apropiados para streaming cuando la precisión lo permita;
- materializar la matriz completa únicamente para medidas que la necesiten.

## 8. Roadmap recomendado

### Fase 0 — Contratos y validación

- Definir el esquema versionado del AST y del `Cashflow`.
- Fijar semántica de fechas, signos, monedas y eventos simultáneos.
- Añadir golden tests que comparen el camino actual de `IRSwap` con el futuro.
- Separar tests de fórmula, tests de cashflows y tests de pricing.

**Criterio de salida:** un caso IRS tiene una especificación serializable y una explicación
determinista de sus cashflows, sin cambiar resultados actuales.

### Fase 1 — Cashflow IR y schedules

- Implementar `Schedule`, convenciones de day-count y calendario como datos.
- Añadir `CashflowTerm`/`ProductIR` en Rust.
- Implementar el constructor de IRS como compilador AST → cashflows.
- Mantener `IrSwap::npv` como adaptador de compatibilidad.
- Exponer inspección de cashflows en Python y C ABI.

**Criterio de salida:** PV del IRS coincide con el camino actual y los cashflows son auditables.

### Fase 2 — Evaluador genérico

- Implementar expresiones puras, condiciones y eventos básicos.
- Compilar referencias a mercado a IDs/capabilities.
- Crear un intérprete CPU de referencia, simple y correcto.
- Añadir validación estructural y errores con `node_id`/`event_id`.
- Comparar intérprete y kernels vectorizados con property tests.

**Criterio de salida:** un bono, un IRS y un forward se expresan con las mismas primitivas.

### Fase 3 — Vectorización y Monte Carlo

- Transformar el plan a SoA y agrupar por forma.
- Separar scenario engine, evaluator y reducer.
- Implementar evaluación por chunks y escenarios compartidos.
- Portar primero los kernels calientes a Burn; conservar el intérprete como oracle.
- Validar CPU/GPU y autodiff frente a referencias analíticas y bump-and-reval.

**Criterio de salida:** el IRS AST produce el mismo resultado escalar, batch y perfil EE/PFE que
la implementación existente, con memoria y dimensiones documentadas.

### Fase 4 — Medidas, cartera y netting

- Convertir medidas en un grafo de dependencias.
- Añadir `Portfolio`, `NettingSet` y colateral como niveles distintos del trade.
- Compartir escenarios, curves y subplanes entre trades.
- Agregar resultados por trade, netting set, counterparty y moneda.
- Añadir CVA/DVA/FVA cuando existan las convenciones de funding/collateral necesarias.

**Criterio de salida:** una cartera heterogénea se calcula sin llamar a `price` una vez por trade
y mantiene trazabilidad hasta cada cashflow.

### Fase 5 — Multi-asset y extensiones

- Introducir capabilities de FX, equity, commodity y volatilidad.
- Añadir forwards, opciones y productos con ejercicio.
- Añadir instrumentos de calibración como ASTs de mercado.
- Generalizar calibración a vectores de parámetros e instrumentos.

**Criterio de salida:** añadir un producto nuevo requiere componer primitivas o registrar un
backend de capability, no modificar todos los clientes.

### Fase 6 — Plugins y operación

- Estabilizar schema/ABI del IR antes de cargar código externo.
- Definir plugin manifest, versionado y compatibilidad de capabilities.
- Preferir plugins compilados (Rust/C ABI) para producción; reservar callbacks Python para
  prototipos fuera del hot path.
- Añadir serialización, observabilidad, límites de recursos y políticas de determinismo.

El plugin loader no debe ser la primera fase: sin un IR estable solo convertiría contratos internos
volátiles en una API difícil de mantener.

## 9. Decisiones que conviene tomar ahora

1. **El AST es declarativo e inmutable.** No permitir que productos personalizados ejecuten
   callbacks arbitrarios dentro de un path.
2. **Cashflow es una salida primaria.** NPV es una reducción del cashflow, no la representación
   del producto.
3. **La fecha es parte del dominio.** `pricing_date` debe dejar de ser solo metadato cuando se
   introduzcan schedules y fixings.
4. **El calendario es datos versionados.** No esconder reglas de calendario dentro de fórmulas.
5. **El modelo expone capabilities.** Evitar un único trait que intente representar todos los
   activos.
6. **El IR interno es tipado; `Params` queda en las fronteras.** Mantener `Params` para
   compatibilidad, pero validar y convertir una sola vez.
7. **Las dimensiones de los tensores son contrato.** Documentar `[scenario, trade, event]` y
   rechazar formas ambiguas.
8. **El intérprete es la referencia.** La implementación vectorizada debe poder contrastarse con
   una ruta sencilla y determinista.
9. **La caché depende de versiones y datos.** Nunca reutilizar un plan o resultado sin incluir
   AST, mercado, modelo, measure y seed relevantes.
10. **La extensión empieza por composición, no por plugins.** La mayoría de productos debería
    poder expresarse sin compilar una nueva biblioteca.

## 10. Riesgos y mitigaciones

| Riesgo | Mitigación |
| --- | --- |
| AST demasiado general y difícil de optimizar | Conjunto pequeño de primitivas; extensiones mediante capabilities |
| Semántica ambigua de fechas/fixings | EventPlan explícito y validación antes de evaluar |
| Pérdida de rendimiento por interpretación | Interpretar solo como oracle; compilar a SoA/kernels |
| Explosión de memoria en Monte Carlo | Chunks, reducciones online y límites por contexto |
| Incompatibilidad entre clientes | DTO/IR único y adaptadores; tests de equivalencia C++/Python/C/Excel |
| Resultados no reproducibles | Seed explícita, RNG por stream/chunk y metadatos de ejecución |
| Plugins inseguros o incompatibles | Manifest, ABI versionada, capabilities declaradas y sandbox operativo |
| Mezcla incorrecta de netting y trade | Modelar `Portfolio`/`NettingSet` como nodos distintos |
| AAD costoso sobre todo el árbol | Selección explícita de targets y evaluación por bloques |
| Cambiar fórmulas durante la migración | Golden tests y adaptadores; no retirar `IRSwap` hasta cerrar equivalencia |

## 11. Conclusión

El proyecto está bien posicionado para convertirse en una librería quant extensible: ya tiene
separación de clientes, backend numérico portable, Monte Carlo vectorizado, autodiff, batching y
un registry. La inversión prioritaria debe ser una **representación común de programas financieros
y cashflows**, no otra fachada ni otro registry.

El camino de menor riesgo es:

1. definir un AST/IR pequeño, tipado y versionado;
2. compilar el IRS actual a ese IR;
3. hacer que el cashflow sea inspeccionable y el NPV una reducción;
4. separar escenarios, evaluación y medidas;
5. vectorizar el plan en Rust y compartirlo entre productos;
6. añadir cartera, netting y multi-asset encima de esos contratos.

Con esa secuencia se conserva la compatibilidad actual y se obtiene una base composable para
productos orientados a programas, datos de mercado y cálculo pesado en Rust, sin convertir el
hot path en una colección de objetos virtuales ni duplicar lógica entre Python, C++, C y Excel.
