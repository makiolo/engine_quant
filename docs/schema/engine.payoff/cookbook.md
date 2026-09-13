# Cookbook de `engine.payoff/v1`

Recetas para construir los patrones de PLAN_PRODUCTS.md §4/§9 con `engine_typed.payoff`
(`import engine_typed as q`), más el catálogo de errores reales que produce `ValidationVisitor`
(`cpp/engine/src/payoff/validation_visitor.cpp`) cuando un contrato no es válido. Todos los
ejemplos tienen un fixture JSON equivalente en `docs/schema/engine.payoff/examples/`.

## 1. Patrones de autoría

### Call/put europea

Plantilla directa, no hace falta tocar el AST (ver `examples/call.json`):

```python
call = q.european_call("AAPL_CALL_100", "EQ.SPOT.AAPL", strike=100.0, notional=1_000.0, maturity=1.0)
put = q.european_put("AAPL_PUT_100", "EQ.SPOT.AAPL", strike=100.0, notional=1_000.0, maturity=1.0)
```

Equivalente construido a mano con los builders crudos (lo que hace la plantilla por dentro):

```python
call_contract = q.when(
    1.0,
    q.cashflow("USD", 1_000.0 * q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100.0, 0)),
)
call = q.PayoffProduct(id="AAPL_CALL_100", contract=call_contract)
```

### FX forward multi-moneda

Plantilla (`examples/forward.json` es una versión de una sola pata; esto es el forward
FX real de §9.4, dos cashflows en monedas distintas):

```python
fwd = q.fx_forward("EURUSD_FWD", currency_for="EUR", currency_dom="USD",
                    notional_for=1_000_000.0, strike=1.10, maturity=1.0)
```

`sign=-1.0` invierte ambas patas (vender en vez de comprar la divisa extranjera).

### IRS fijo-flotante single-curve

Plantilla, réplica exacta de `templates::irs_swap` en C++ (pata flotante intercambiando
nocional en `start`/`payment_times[-1]`, pata fija como `Give` porque el titular la paga —
ADR-P0-02):

```python
swap = q.irs("SWAP_2Y_3PCT", notional=1_000_000.0, fixed_rate=0.03,
             payment_times=[1.0, 2.0], accruals=[1.0, 1.0])
```

No hay proyección de índice flotante: es la réplica single-curve documentada en
`irs_templates.hpp` (PV(flotante) = N·(DF(start) − DF(T_last))), la misma limitación que la
ruta legacy `IrSwapProduct`.

### Barrera (up-and-in call)

No hay plantilla todavía (§4.2): se construye con `trigger` directo (ver `examples/barrier.json`):

```python
barrier = q.PayoffProduct(
    id="AAPL_UP_AND_IN_CALL_100_120",
    contract=q.trigger(
        id="UI",
        monitoring_times=[0.25, 0.5, 0.75, 1.0],
        condition=q.greater_equal(q.current("EQ.SPOT.AAPL"), 120.0),
        monitoring="discrete",
        settlement="at_scheduled_payment",
        priority=0,
        latch=True,
        on_hit=q.when(1.0, q.cashflow("USD", q.maximum(q.fixing("EQ.SPOT.AAPL", 1.0) - 100.0, 0))),
        on_miss=q.zero(),
    ),
)
```

### Take profit / stop loss

Grupo `FirstOf` (§4.3): dos `trigger` con el mismo `latch=True` y una guarda cruzada
`Not(EventOccurred(el_otro))` para exclusión mutua "primer hit gana" (ADR-P0-08). Ver
`examples/tp_sl.json` para el árbol completo con `metric = spot/entry_price - 1`.

### Asiática (media aritmética)

Nodo `average` (§3.2), sin plantilla dedicada — construcción directa:

```python
asian = q.PayoffProduct(
    id="AAPL_ASIAN_CALL_100",
    contract=q.when(
        1.0,
        q.cashflow(
            "USD",
            1_000.0 * q.maximum(
                q.average("EQ.SPOT.AAPL", schedule=[0.25, 0.5, 0.75, 1.0], weights=[0.25, 0.25, 0.25, 0.25])
                - 100.0,
                0,
            ),
        ),
    ),
)
```

`schedule` y `weights` deben tener la misma longitud (ver catálogo de errores más abajo).

### Ejercicio bermuda

Nodo `exercise` (§10, Fase 9): declara el derecho, la política de decisión (Longstaff-Schwartz)
vive en el pricer, no en el AST. Ver `examples/exercise.json`:

```python
bermuda_put = q.PayoffProduct(
    id="AAPL_BERMUDA_PUT_100",
    contract=q.exercise(
        id="EX",
        dates=[0.25, 0.5, 0.75],
        exercise_value=q.maximum(100.0 - q.current("EQ.SPOT.AAPL"), 0),
        continuation=q.when(1.0, q.cashflow("USD", q.maximum(100.0 - q.fixing("EQ.SPOT.AAPL", 1.0), 0))),
    ),
)
```

## 2. Catálogo de errores de `ValidationVisitor`

`PayoffProduct` (C++, y por tanto `eng.create_product("Payoff", trade.to_params())` en Python,
`engine_abi_create_product("Payoff", ...)` en la C ABI, y `ENGINE.CREATE_PRODUCT("Payoff", ...)`
en Excel) valida el árbol completo antes de construir nada y agrega **todos** los errores
encontrados en un único mensaje (uno por línea, con la ruta de nodo cuando aplica), nunca se
detiene en el primero. La tabla usa el texto exacto que emite `ValidationVisitor`
(`validation_visitor.cpp`); el campo entre comillas simples de cada mensaje varía según el nodo.
Cada línea real va precedida de la ruta del nodo (`NodePath`, p.ej. `root.child.amount`) seguida
de `": "` -- la tabla omite ese prefijo porque depende de dónde esté el nodo en el árbol
concreto, no del tipo de error.

| Condición | Mensaje | Cómo solucionarlo |
|---|---|---|
| `Cashflow` con `currency=""` | `moneda vacia en Cashflow` | Usar un código ISO de 3 letras (`"USD"`, `"EUR"`, ...). |
| `Cashflow` fuera de un `When`/`Trigger AtHit` | `Cashflow sin instante activo (falta un When/Trigger AtHit envolvente, ADR-P0-08)` | Envolver el `Cashflow` en `q.when(t, ...)`, o colgarlo de `on_hit` de un `Trigger` con `settlement="at_hit"`. |
| `Current`/`RunningMin`/`RunningMax` fuera de un instante activo | `Current sin instante activo (...)` / `RunningMin sin instante activo (ADR-P0-08)` / `RunningMax sin instante activo (ADR-P0-08)` | Igual que el caso anterior: estos nodos solo tienen sentido dentro de un `When` o de la rama `on_hit` de un `Trigger AtHit`. |
| `Fixing`/`Before`/`After`/`DiscountFactor` con un `time`/`from`/`to` no finito (`NaN`/`inf`) | `fecha no finita en '<campo>'` | Pasar siempre un `float` finito; no propagar `NaN` desde un cálculo previo. |
| `Trigger.monitoring_times`, `Exercise.dates` o `Average.schedule` vacío | `'<campo>' no puede estar vacio` | Dar al menos una fecha. |
| Ese mismo schedule desordenado o con una fecha no finita | `'<campo>' debe estar estrictamente ordenado ascendente` / `fecha no finita en '<campo>'` | Ordenar el schedule estrictamente ascendente (sin fechas repetidas: dos fechas iguales dentro de `kTimeToleranceYearFraction`, ADR-P0-01, ya cuentan como "no asciende"). |
| `Average` con `schedule`/`weights` de distinta longitud | `'schedule' y 'weights' deben tener la misma longitud` | Emparejar ambas listas 1 a 1. |
| Dos `Trigger`/`Exercise` con el mismo `id` | `EventId '<id>' duplicado` | Cada evento del árbol necesita un `EventId` único, incluso entre ramas distintas de un `Both`. |
| `EventTime`/`EventValue`/`EventOccurred` que referencia un `id` que ningún `Trigger`/`Exercise` define | `referencia a EventId '<id>' sin Trigger/Exercise que lo defina` | Comprobar que el `id` coincide exactamente (sensible a mayúsculas) con el de un `Trigger`/`Exercise` del mismo árbol. |
| `FxConversion` con `from_currency`/`to_currency` vacío | `moneda de origen vacia en FxConversion` / `moneda de destino vacia en FxConversion` | Igual que `Cashflow`: código ISO de 3 letras en ambos lados. |
| `Eq` con `tolerance` negativa, `NaN` o infinita | `Eq requiere una tolerancia finita >= 0` | Pasar una tolerancia explícita ≥ 0 (nunca comparar floats con `==` directo, ADR-P0-01). |
| `All`/`Any` sin operandos | `All requiere al menos un operando` / `Any requiere al menos un operando` | Añadir al menos un `Predicate` a `operands`. |
| Árbol anidado más de 1024 niveles | `profundidad maxima del arbol excedida` | Aplanar el árbol o revisar si hay una construcción recursiva accidental; el límite es una defensa (ADR-P0-07), no una expectativa de uso normal. |
| Un hijo `None`/nulo en cualquier campo (`child`, `left`, `condition`, ...) | `nodo nulo en '<campo>'` | Asegurarse de que todo builder recibe un nodo válido, no `None`. |
| Contrato raíz `None` | `contrato raiz nulo` | Pasar un `Contract` real a `PayoffProduct(contract=...)`. |

Ejemplo de excepción agregada (la que se ve típicamente en Python al llamar
`eng.create_product("Payoff", ...)` con un contrato inválido):

```text
PayoffProduct: 2 error(es) de validacion:
  - root: Cashflow sin instante activo (falta un When/Trigger AtHit envolvente, ADR-P0-08)
  - root: moneda vacia en Cashflow
```
