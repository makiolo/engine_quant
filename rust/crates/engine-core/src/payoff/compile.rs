//! AST(JSON `engine.payoff/v1`) -> `CompiledPayoff` (PLAN_PRODUCTS.md §8, ADR-P0-09). Consume
//! directamente el JSON canonico que ya produce `CanonicalVisitor::to_json` en C++ (o
//! `engine_typed.payoff` en Python) -- ver `docs/schema/engine.payoff/v1.schema.json` para la
//! superficie sintactica completa (que documenta mas nodos de los que este compilador acepta,
//! ver §0.1 y el doc-comment de `crate::payoff`: rechazar antes de simular un payoff que el
//! modelo/algoritmo no pueda evaluar es exactamente el comportamiento que pide ese parrafo).
//!
//! Parseo dinamico sobre `serde_json::Value` (no `#[derive(Deserialize)]` con un enum
//! discriminado): el subconjunto de nodos soportado es pequeno y cada uno necesita su propio
//! mensaje de error preciso (nombra el campo/tipo de nodo que falta o no se soporta), mas facil
//! de mantener como funciones explicitas que como un enum `serde` con variantes "no soportadas
//! todavia" mezcladas con las soportadas.

use serde_json::Value;

use super::ir::{
    BarrierDirection, BridgePattern, CompiledPayoff, ContractOp, MonitoringMode, PredicateOp, ScalarOp,
    SettlementMode, COMPILED_PAYOFF_VERSION,
};

/// Compila un documento `engine.payoff/v1` completo (con envoltorio `schema`/`id`/`contract`,
/// PLAN_PRODUCTS.md §7.2) a `CompiledPayoff`. `Err` es siempre un error de preflight: nunca se
/// evalua nada del contrato durante la compilacion.
pub fn compile(spec_json: &str) -> Result<CompiledPayoff, String> {
    let doc: Value = serde_json::from_str(spec_json)
        .map_err(|e| format!("payoff: JSON invalido: {e}"))?;
    let schema = doc
        .get("schema")
        .and_then(Value::as_str)
        .ok_or_else(|| "payoff: falta el campo 'schema'".to_string())?;
    if schema != "engine.payoff/v1" {
        return Err(format!(
            "payoff: schema no soportado '{schema}' (CompiledPayoff v{COMPILED_PAYOFF_VERSION} solo entiende 'engine.payoff/v1')"
        ));
    }
    let contract = doc
        .get("contract")
        .ok_or_else(|| "payoff: falta el campo 'contract'".to_string())?;

    let mut compiler = Compiler::default();
    let root = compiler.compile_contract(contract)?;
    let currency = compiler.currency.ok_or_else(|| {
        "payoff: el contrato no genera ningun Cashflow -- no hay moneda de reporting que inferir"
            .to_string()
    })?;
    Ok(CompiledPayoff {
        version: COMPILED_PAYOFF_VERSION,
        currency,
        observable_slots: compiler.observable_slots,
        event_slots: compiler.event_slots,
        scalar_ops: compiler.scalar_ops,
        predicate_ops: compiler.predicate_ops,
        contract_ops: compiler.contract_ops,
        root,
    })
}

#[derive(Default)]
struct Compiler {
    scalar_ops: Vec<ScalarOp>,
    predicate_ops: Vec<PredicateOp>,
    contract_ops: Vec<ContractOp>,
    observable_slots: Vec<String>,
    event_slots: Vec<String>,
    currency: Option<String>,
}

impl Compiler {
    fn observable_slot(&mut self, name: &str) -> usize {
        if let Some(i) = self.observable_slots.iter().position(|o| o == name) {
            return i;
        }
        self.observable_slots.push(name.to_string());
        self.observable_slots.len() - 1
    }

    fn event_slot(&mut self, name: &str) -> usize {
        if let Some(i) = self.event_slots.iter().position(|e| e == name) {
            return i;
        }
        self.event_slots.push(name.to_string());
        self.event_slots.len() - 1
    }

    /// CSE (common-subexpression elimination, PLAN_PRODUCTS.md §8 punto 4, Fase 11): mismo
    /// escaneo lineal que `observable_slot`/`event_slot` de arriba, pero sobre nodos ya
    /// construidos en vez de nombres. Ver el doc-comment de `ir.rs` para por que fusionar dos
    /// `ContractOp::Trigger` estructuralmente identicos (con estado por ruta) es seguro.
    fn intern_scalar(&mut self, op: ScalarOp) -> usize {
        if let Some(i) = self.scalar_ops.iter().position(|existing| existing == &op) {
            return i;
        }
        self.scalar_ops.push(op);
        self.scalar_ops.len() - 1
    }

    /// Ver `intern_scalar`.
    fn intern_predicate(&mut self, op: PredicateOp) -> usize {
        if let Some(i) = self.predicate_ops.iter().position(|existing| existing == &op) {
            return i;
        }
        self.predicate_ops.push(op);
        self.predicate_ops.len() - 1
    }

    /// Ver `intern_scalar`.
    fn intern_contract(&mut self, op: ContractOp) -> usize {
        if let Some(i) = self.contract_ops.iter().position(|existing| existing == &op) {
            return i;
        }
        self.contract_ops.push(op);
        self.contract_ops.len() - 1
    }

    /// Reconoce `condition` como una barrera simple (§4.2, Fase 6): UNICAMENTE
    /// `{"type":"greater_equal"/"less_equal", "left":{"type":"current","observable":X},
    /// "right":{"type":"constant","value":H}}`, la misma forma que emiten las plantillas de
    /// `barrier_templates.hpp` en C++ (`up_and_in`/`down_and_out`/`double_knock_out` usan
    /// `current(...)` a la izquierda y una constante a la derecha). Cualquier otra forma
    /// (constante a la izquierda, `fixing` en vez de `current`, un `All`/`Any` compuesto para
    /// doble barrera, etc.) se rechaza explicitamente en vez de aproximarla mal: `Monitoring::
    /// ContinuousApproximation` con una condicion asi no tiene sentido geometrico para la
    /// correccion de Brownian bridge de un unico nivel/direccion (PLAN_PRODUCTS.md §16: nunca
    /// ocultar una aproximacion).
    fn recognize_barrier_pattern(&mut self, condition: &Value) -> Result<BridgePattern, String> {
        let ty = node_type(condition)?;
        let direction = match ty {
            "greater_equal" => BarrierDirection::Up,
            "less_equal" => BarrierDirection::Down,
            other => {
                return Err(format!(
                    "payoff: 'continuous_approximation' no reconoce el predicado '{other}' -- solo \
                     'greater_equal'/'less_equal' de la forma 'current(observable) vs constante' \
                     (Brownian bridge, Fase 6); usa 'discrete' para cualquier otra condicion"
                ));
            }
        };
        let obs_side = field_value(condition, "left")?;
        let const_side = field_value(condition, "right")?;
        if node_type(obs_side)? != "current" {
            return Err(
                "payoff: 'continuous_approximation' requiere 'current(observable)' en 'left' de la condicion"
                    .to_string(),
            );
        }
        if node_type(const_side)? != "constant" {
            return Err(
                "payoff: 'continuous_approximation' requiere una 'constant' en 'right' de la condicion"
                    .to_string(),
            );
        }
        let observable = self.observable_slot(str_field(obs_side, "observable")?);
        let barrier = num_field(const_side, "value")?;
        Ok(BridgePattern { observable, barrier, direction })
    }

    fn compile_scalar(&mut self, node: &Value) -> Result<usize, String> {
        let ty = node_type(node)?;
        let op = match ty {
            "constant" => ScalarOp::Constant(num_field(node, "value")?),
            "fixing" => {
                let observable = self.observable_slot(str_field(node, "observable")?);
                let time = num_field(node, "time")?;
                ScalarOp::Fixing { observable, time }
            }
            "current" => {
                let observable = self.observable_slot(str_field(node, "observable")?);
                ScalarOp::Current { observable }
            }
            "add" => ScalarOp::Add(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "sub" => ScalarOp::Sub(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "mul" => ScalarOp::Mul(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "div" => ScalarOp::Div(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "neg" => ScalarOp::Neg(self.scalar_field(node, "operand")?),
            "abs" => ScalarOp::Abs(self.scalar_field(node, "operand")?),
            "exp" => ScalarOp::Exp(self.scalar_field(node, "operand")?),
            "log" => ScalarOp::Log(self.scalar_field(node, "operand")?),
            "pow" => ScalarOp::Pow(self.scalar_field(node, "base")?, self.scalar_field(node, "exponent")?),
            "min" => ScalarOp::Min(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "max" => ScalarOp::Max(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "clamp" => ScalarOp::Clamp {
                value: self.scalar_field(node, "value")?,
                low: self.scalar_field(node, "low")?,
                high: self.scalar_field(node, "high")?,
            },
            "event_value" => {
                let event = self.event_slot(str_field(node, "event")?);
                let observable = self.observable_slot(str_field(node, "observable")?);
                ScalarOp::EventValue { event, observable }
            }
            // Misma forma EXACTA que el AST de autoria ya existente (cpp/engine/include/engine/
            // payoff/expression.hpp, docs/schema/engine.payoff/v1.schema.json,
            // engine_typed.payoff.average/running_min/running_max en Python) -- ver el
            // doc-comment de ScalarOp::Average/RunningMin en ir.rs para el porque.
            "average" => {
                let observable = self.observable_slot(str_field(node, "observable")?);
                let schedule = array_field(node, "schedule")?
                    .iter()
                    .map(|v| v.as_f64().ok_or_else(|| "payoff: 'schedule' debe contener solo numeros".to_string()))
                    .collect::<Result<Vec<f64>, String>>()?;
                let weights = array_field(node, "weights")?
                    .iter()
                    .map(|v| v.as_f64().ok_or_else(|| "payoff: 'weights' debe contener solo numeros".to_string()))
                    .collect::<Result<Vec<f64>, String>>()?;
                // Mismo criterio que 'ValidationVisitor::check_schedule_ascending_and_finite' en
                // C++ (valida el mismo campo del mismo AST): 'schedule' no vacio y estrictamente
                // ascendente; ademas 'schedule'/'weights' de igual longitud (mismo chequeo que
                // 'ScalarEvalVisitor::visit(Average)', que fallaria en evaluacion si no
                // coincidieran -- aqui se adelanta a preflight, "Err es siempre un error de
                // preflight", ver el doc-comment de compile()).
                if schedule.is_empty() {
                    return Err("payoff: 'average' requiere al menos un instante en 'schedule'".to_string());
                }
                if schedule.len() != weights.len() {
                    return Err(format!(
                        "payoff: 'average': 'schedule' ({} elementos) y 'weights' ({} elementos) deben tener la misma longitud",
                        schedule.len(),
                        weights.len()
                    ));
                }
                for w in schedule.windows(2) {
                    if w[1] <= w[0] {
                        return Err("payoff: 'schedule' de 'average' debe ser estrictamente ascendente".to_string());
                    }
                }
                ScalarOp::Average { observable, schedule, weights }
            }
            "running_min" => {
                let observable = self.observable_slot(str_field(node, "observable")?);
                ScalarOp::RunningMin { observable }
            }
            "running_max" => {
                let observable = self.observable_slot(str_field(node, "observable")?);
                ScalarOp::RunningMax { observable }
            }
            other => return Err(unsupported_node("escalar", other)),
        };
        Ok(self.intern_scalar(op))
    }

    fn compile_predicate(&mut self, node: &Value) -> Result<usize, String> {
        let ty = node_type(node)?;
        let op = match ty {
            "greater" => PredicateOp::Greater(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "less" => PredicateOp::Less(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?),
            "greater_equal" => {
                PredicateOp::GreaterEqual(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?)
            }
            "less_equal" => {
                PredicateOp::LessEqual(self.scalar_field(node, "left")?, self.scalar_field(node, "right")?)
            }
            "eq" => PredicateOp::Eq {
                left: self.scalar_field(node, "left")?,
                right: self.scalar_field(node, "right")?,
                tolerance: num_field(node, "tolerance")?,
            },
            "all" => PredicateOp::All(self.predicate_array_field(node, "operands")?),
            "any" => PredicateOp::Any(self.predicate_array_field(node, "operands")?),
            "not" => PredicateOp::Not(self.predicate_field(node, "operand")?),
            "between" => PredicateOp::Between {
                value: self.scalar_field(node, "value")?,
                low: self.scalar_field(node, "low")?,
                high: self.scalar_field(node, "high")?,
                low_inclusive: bool_field(node, "low_inclusive")?,
                high_inclusive: bool_field(node, "high_inclusive")?,
            },
            "event_occurred" => PredicateOp::EventOccurred(self.event_slot(str_field(node, "event")?)),
            other => return Err(unsupported_node("predicado", other)),
        };
        Ok(self.intern_predicate(op))
    }

    fn compile_contract(&mut self, node: &Value) -> Result<usize, String> {
        let ty = node_type(node)?;
        let op = match ty {
            "zero" => ContractOp::Zero,
            "cashflow" => {
                let currency = str_field(node, "currency")?;
                match &self.currency {
                    None => self.currency = Some(currency.to_string()),
                    Some(existing) if existing == currency => {}
                    Some(existing) => {
                        return Err(format!(
                            "payoff: multi-moneda no soportado en CompiledPayoff v{COMPILED_PAYOFF_VERSION} (Fase 5): \
                             se encontro 'Cashflow' en '{currency}' junto a '{existing}'; requiere conversion FX \
                             explicita bajo Q (Fase 6+)"
                        ));
                    }
                }
                let amount = self.scalar_field(node, "amount")?;
                ContractOp::Cashflow { amount }
            }
            "give" => ContractOp::Give(self.contract_field(node, "child")?),
            "both" => ContractOp::Both(self.contract_array_field(node, "children")?),
            "scale" => ContractOp::Scale {
                factor: self.scalar_field(node, "factor")?,
                child: self.contract_field(node, "child")?,
            },
            "if" => ContractOp::If {
                condition: self.predicate_field(node, "condition")?,
                if_true: self.contract_field(node, "if_true")?,
                if_false: self.contract_field(node, "if_false")?,
            },
            "when" => ContractOp::When {
                time: num_field(node, "time")?,
                child: self.contract_field(node, "child")?,
            },
            "trigger" => {
                let event = self.event_slot(str_field(node, "id")?);
                let monitoring_times = array_field(node, "monitoring_times")?
                    .iter()
                    .map(|v| {
                        v.as_f64().ok_or_else(|| {
                            "payoff: 'monitoring_times' debe contener solo numeros".to_string()
                        })
                    })
                    .collect::<Result<Vec<f64>, String>>()?;
                let monitoring = match str_field(node, "monitoring")? {
                    "discrete" => MonitoringMode::Discrete,
                    "continuous_approximation" => MonitoringMode::ContinuousApproximation,
                    other => return Err(format!("payoff: 'monitoring' desconocido '{other}'")),
                };
                let settlement = match str_field(node, "settlement")? {
                    "at_hit" => SettlementMode::AtHit,
                    "at_scheduled_payment" => SettlementMode::AtScheduledPayment,
                    other => return Err(format!("payoff: 'settlement' desconocido '{other}'")),
                };
                let priority = num_field(node, "priority")? as i32;
                let latch = bool_field(node, "latch")?;
                // Reconocer el patron de barrera ANTES de compilar 'condition' a PredicateOp (que
                // tambien resuelve 'observable' a slot, de forma idempotente -- mismo indice en
                // ambos sitios): si 'monitoring' es continua y el patron no se reconoce, fallar
                // en preflight sin compilar el resto del Trigger.
                let bridge = if monitoring == MonitoringMode::ContinuousApproximation {
                    Some(self.recognize_barrier_pattern(field_value(node, "condition")?)?)
                } else {
                    None
                };
                // 'condition'/'on_hit'/'on_miss' se compilan DESPUES de resolver 'event' (arriba)
                // para que un 'event_occurred'/'event_value' anidado que se refiera a este mismo
                // evento (p.ej. el propio TP/SL referenciandose por id, aunque no es el caso de
                // TpSlSpec) resuelva al slot correcto.
                let condition = self.predicate_field(node, "condition")?;
                let on_hit = self.contract_field(node, "on_hit")?;
                let on_miss = self.contract_field(node, "on_miss")?;
                ContractOp::Trigger {
                    event,
                    monitoring_times,
                    condition,
                    monitoring,
                    bridge,
                    settlement,
                    priority,
                    latch,
                    on_hit,
                    on_miss,
                }
            }
            "exercise" => {
                // Mismo orden que 'trigger': resolver 'event' (id) antes de compilar el resto,
                // por si 'exercise_value'/'continuation' referencian este mismo EventId (§10).
                let event = self.event_slot(str_field(node, "id")?);
                let dates = array_field(node, "dates")?
                    .iter()
                    .map(|v| v.as_f64().ok_or_else(|| "payoff: 'dates' debe contener solo numeros".to_string()))
                    .collect::<Result<Vec<f64>, String>>()?;
                if dates.is_empty() {
                    return Err("payoff: 'exercise' requiere al menos una fecha en 'dates'".to_string());
                }
                for w in dates.windows(2) {
                    if w[1] <= w[0] {
                        return Err("payoff: 'dates' de 'exercise' debe ser estrictamente ascendente".to_string());
                    }
                }
                let exercise_value = self.scalar_field(node, "exercise_value")?;
                let continuation = self.contract_field(node, "continuation")?;
                ContractOp::Exercise { event, dates, exercise_value, continuation }
            }
            other => return Err(unsupported_node("contrato", other)),
        };
        Ok(self.intern_contract(op))
    }

    fn scalar_field(&mut self, node: &Value, field: &str) -> Result<usize, String> {
        self.compile_scalar(field_value(node, field)?)
    }
    fn predicate_field(&mut self, node: &Value, field: &str) -> Result<usize, String> {
        self.compile_predicate(field_value(node, field)?)
    }
    fn contract_field(&mut self, node: &Value, field: &str) -> Result<usize, String> {
        self.compile_contract(field_value(node, field)?)
    }

    fn predicate_array_field(&mut self, node: &Value, field: &str) -> Result<Vec<usize>, String> {
        array_field(node, field)?.iter().map(|v| self.compile_predicate(v)).collect()
    }
    fn contract_array_field(&mut self, node: &Value, field: &str) -> Result<Vec<usize>, String> {
        array_field(node, field)?.iter().map(|v| self.compile_contract(v)).collect()
    }
}

fn unsupported_node(category: &str, node_type: &str) -> String {
    format!(
        "payoff: nodo de {category} '{node_type}' no soportado en CompiledPayoff v{COMPILED_PAYOFF_VERSION} \
         (PLAN_PRODUCTS.md §12, PLAN_IMPROVE_NOTEBOOK.md Fase 2): requiere un observable de mercado \
         (DiscountFactor/FxConversion/Parameter/EventTime/Before/After) que ningun modelo Q de esta fase \
         evalua todavia -- Average/RunningMin/RunningMax SI estan soportados desde la Fase 2 de \
         PLAN_IMPROVE_NOTEBOOK.md"
    )
}

fn node_type(node: &Value) -> Result<&str, String> {
    node.get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("payoff: nodo sin campo 'type' valido: {node}"))
}

fn field_value<'a>(node: &'a Value, field: &str) -> Result<&'a Value, String> {
    node.get(field).ok_or_else(|| format!("payoff: falta el campo '{field}' en {node}"))
}

fn array_field<'a>(node: &'a Value, field: &str) -> Result<&'a Vec<Value>, String> {
    field_value(node, field)?
        .as_array()
        .ok_or_else(|| format!("payoff: el campo '{field}' deberia ser un array en {node}"))
}

fn num_field(node: &Value, field: &str) -> Result<f64, String> {
    field_value(node, field)?
        .as_f64()
        .ok_or_else(|| format!("payoff: el campo '{field}' deberia ser numerico en {node}"))
}

fn str_field<'a>(node: &'a Value, field: &str) -> Result<&'a str, String> {
    field_value(node, field)?
        .as_str()
        .ok_or_else(|| format!("payoff: el campo '{field}' deberia ser un string en {node}"))
}

fn bool_field(node: &Value, field: &str) -> Result<bool, String> {
    field_value(node, field)?
        .as_bool()
        .ok_or_else(|| format!("payoff: el campo '{field}' deberia ser booleano en {node}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mismo documento que docs/schema/engine.payoff/examples/call.json (call europea AAPL@100,
    // notional 1000): 1000 * max(Fixing(EQ.SPOT.AAPL, 1.0) - 100, 0), pagado en t=1.0 USD.
    const CALL_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "AAPL_CALL_100",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "mul",
                    "left": {"type": "constant", "value": 1000.0},
                    "right": {
                        "type": "max",
                        "left": {
                            "type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}
                        },
                        "right": {"type": "constant", "value": 0.0}
                    }
                }
            }
        }
    }"#;

    #[test]
    fn compiles_call_example_with_single_observable_slot() {
        let compiled = compile(CALL_JSON).expect("call.json deberia compilar");
        assert_eq!(compiled.version, COMPILED_PAYOFF_VERSION);
        assert_eq!(compiled.currency, "USD");
        assert_eq!(compiled.observable_slots, vec!["EQ.SPOT.AAPL".to_string()]);
        assert!(matches!(
            compiled.contract_ops[compiled.root],
            ContractOp::When { time, .. } if time == 1.0
        ));
    }

    #[test]
    fn rejects_wrong_schema() {
        let err = compile(r#"{"schema": "other/v1", "id": "x", "contract": {"type": "zero"}}"#)
            .expect_err("schema desconocido deberia fallar");
        assert!(err.contains("engine.payoff/v1"));
    }

    #[test]
    fn rejects_unsupported_node_before_evaluating_anything() {
        // 'average' YA se soporta como nodo ESCALAR desde PLAN_IMPROVE_NOTEBOOK.md Fase 2 (ver
        // los tests dedicados mas abajo) -- lo que sigue sin soportarse es un nodo de CONTRATO
        // inexistente, para seguir ejercitando el camino de error de 'unsupported_node'.
        let unsupported_contract_json = r#"{
            "schema": "engine.payoff/v1",
            "id": "x",
            "contract": {"type": "discount_factor"}
        }"#;
        let err = compile(unsupported_contract_json)
            .expect_err("'discount_factor' no es un nodo de contrato valido, deberia rechazarse en preflight");
        assert!(err.contains("discount_factor"));
    }

    // PLAN_IMPROVE_NOTEBOOK.md Fase 2: Average/RunningMin/RunningMax como nodos ESCALARES, misma
    // forma EXACTA que el AST de autoria C++/Python/docs/schema ya establecido (ver el
    // doc-comment de ScalarOp::Average/RunningMin en ir.rs). 'average' es una suma PONDERADA
    // (schedule+weights), no una media aritmetica con divisor implicito; 'running_min'/
    // 'running_max' solo llevan 'observable' (sin schedule propio).
    const AVERAGE_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "x",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "cashflow",
                "currency": "USD",
                "amount": {
                    "type": "average",
                    "observable": "EQ.SPOT.AAPL",
                    "schedule": [0.25, 0.5, 0.75, 1.0],
                    "weights": [0.25, 0.25, 0.25, 0.25]
                }
            }
        }
    }"#;

    #[test]
    fn compiles_average_with_schedule_in_required_times() {
        let compiled = compile(AVERAGE_JSON).expect("'average' deberia compilar (Fase 2)");
        assert_eq!(compiled.observable_slots, vec!["EQ.SPOT.AAPL".to_string()]);
        assert_eq!(compiled.required_times(), vec![0.25, 0.5, 0.75, 1.0]);
        assert!(matches!(
            compiled.scalar_ops.iter().find(|op| matches!(op, ScalarOp::Average { .. })),
            Some(ScalarOp::Average { schedule, weights, .. })
                if schedule == &vec![0.25, 0.5, 0.75, 1.0] && weights == &vec![0.25, 0.25, 0.25, 0.25]
        ));
    }

    #[test]
    fn rejects_average_with_empty_schedule() {
        let json = AVERAGE_JSON.replace(r#""schedule": [0.25, 0.5, 0.75, 1.0],"#, r#""schedule": [],"#)
            .replace(r#""weights": [0.25, 0.25, 0.25, 0.25]"#, r#""weights": []"#);
        let err = compile(&json).expect_err("'average' con 'schedule' vacio deberia rechazarse");
        assert!(err.contains("schedule"), "err={err}");
    }

    #[test]
    fn rejects_average_with_non_ascending_schedule() {
        let json = AVERAGE_JSON.replace(r#""schedule": [0.25, 0.5, 0.75, 1.0],"#, r#""schedule": [0.5, 0.25, 0.75, 1.0],"#);
        let err = compile(&json).expect_err("'average' con 'schedule' no ascendente deberia rechazarse");
        assert!(err.contains("ascendente"), "err={err}");
    }

    #[test]
    fn rejects_average_with_mismatched_schedule_and_weights_length() {
        let json = AVERAGE_JSON.replace(r#""weights": [0.25, 0.25, 0.25, 0.25]"#, r#""weights": [0.5, 0.5]"#);
        let err = compile(&json).expect_err("'average' con longitudes distintas deberia rechazarse");
        assert!(err.contains("misma longitud"), "err={err}");
    }

    // 'running_min'/'running_max' solo llevan 'observable' -- sin 'schedule' propio (ver el
    // doc-comment de ScalarOp::RunningMin): no contribuyen nada a required_times() por si
    // mismos, consultan en evaluacion lo que YA este ahi por otro camino, filtrado por cursor
    // (ver los tests de paridad en eval.rs).
    #[test]
    fn compiles_running_min_and_running_max_contributing_nothing_to_required_times_on_their_own() {
        for node_type in ["running_min", "running_max"] {
            let json = format!(
                r#"{{"schema": "engine.payoff/v1", "id": "x", "contract": {{"type": "when", "time": 1.0,
                    "child": {{"type": "cashflow", "currency": "USD",
                        "amount": {{"type": "{node_type}", "observable": "EQ.SPOT.AAPL"}}}}}}}}"#
            );
            let compiled = compile(&json).unwrap_or_else(|e| panic!("'{node_type}' deberia compilar (Fase 2): {e}"));
            assert_eq!(compiled.observable_slots, vec!["EQ.SPOT.AAPL".to_string()]);
            // El unico instante requerido viene del 'when' envolvente, no de running_min/max.
            assert_eq!(compiled.required_times(), vec![1.0]);
        }
    }

    // PLAN_PRODUCTS.md §10, Fase 9: derecho de ejercicio americano/bermuda.
    const EXERCISE_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "PUT_BERMUDA",
        "contract": {
            "type": "exercise",
            "id": "EX",
            "dates": [0.5, 1.0],
            "exercise_value": {
                "type": "max",
                "left": {"type": "sub",
                    "left": {"type": "constant", "value": 100.0},
                    "right": {"type": "current", "observable": "EQ.SPOT.XYZ"}},
                "right": {"type": "constant", "value": 0.0}
            },
            "continuation": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD",
                "amount": {"type": "max",
                    "left": {"type": "sub",
                        "left": {"type": "constant", "value": 100.0},
                        "right": {"type": "fixing", "observable": "EQ.SPOT.XYZ", "time": 1.0}},
                    "right": {"type": "constant", "value": 0.0}}}}
        }
    }"#;

    #[test]
    fn compiles_exercise_with_event_slot_and_dates_in_required_times() {
        let compiled = compile(EXERCISE_JSON).expect("exercise deberia compilar (Fase 9)");
        assert_eq!(compiled.event_slots, vec!["EX".to_string()]);
        assert_eq!(compiled.required_times(), vec![0.5, 1.0]);
        assert!(matches!(
            compiled.contract_ops[compiled.root],
            ContractOp::Exercise { ref dates, .. } if dates == &vec![0.5, 1.0]
        ));
    }

    #[test]
    fn rejects_exercise_with_empty_dates() {
        let json = EXERCISE_JSON.replace(r#""dates": [0.5, 1.0],"#, r#""dates": [],"#);
        let err = compile(&json).expect_err("dates vacio deberia rechazarse");
        assert!(err.contains("dates"));
    }

    #[test]
    fn rejects_exercise_with_non_ascending_dates() {
        let json = EXERCISE_JSON.replace(r#""dates": [0.5, 1.0],"#, r#""dates": [1.0, 0.5],"#);
        let err = compile(&json).expect_err("dates no ascendente deberia rechazarse");
        assert!(err.contains("ascendente"));
    }

    // Mismo documento que docs/schema/engine.payoff/examples/barrier.json (up-and-in AAPL,
    // barrera 120, call 100 a T=1): confirma que el compilador acepta 'trigger' (Fase 6).
    const BARRIER_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "AAPL_UP_AND_IN_CALL_100_120",
        "contract": {
            "type": "trigger",
            "id": "UI",
            "monitoring_times": [0.25, 0.5, 0.75, 1.0],
            "condition": {
                "type": "greater_equal",
                "left": {"type": "current", "observable": "EQ.SPOT.AAPL"},
                "right": {"type": "constant", "value": 120.0}
            },
            "monitoring": "discrete",
            "settlement": "at_scheduled_payment",
            "priority": 0,
            "latch": true,
            "on_hit": {
                "type": "when",
                "time": 1.0,
                "child": {
                    "type": "cashflow",
                    "currency": "USD",
                    "amount": {
                        "type": "max",
                        "left": {
                            "type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}
                        },
                        "right": {"type": "constant", "value": 0.0}
                    }
                }
            },
            "on_miss": {"type": "zero"}
        }
    }"#;

    #[test]
    fn compiles_trigger_with_event_slot_and_monitoring_times_in_required_times() {
        let compiled = compile(BARRIER_JSON).expect("barrier.json deberia compilar (Fase 6)");
        assert_eq!(compiled.event_slots, vec!["UI".to_string()]);
        assert_eq!(compiled.required_times(), vec![0.25, 0.5, 0.75, 1.0]);
        assert!(matches!(
            compiled.contract_ops[compiled.root],
            ContractOp::Trigger { priority: 0, latch: true, .. }
        ));
    }

    #[test]
    fn compiles_continuous_approximation_with_a_recognized_barrier_pattern() {
        // BARRIER_JSON usa exactamente la forma que reconoce recognize_barrier_pattern:
        // greater_equal(current(obs), constant(H)) -- la misma que emite barrier_templates.hpp.
        let json = BARRIER_JSON.replace("\"discrete\"", "\"continuous_approximation\"");
        let compiled = compile(&json).expect("un patron de barrera reconocido debe compilar (Fase 6)");
        match &compiled.contract_ops[compiled.root] {
            ContractOp::Trigger { monitoring, bridge, .. } => {
                assert_eq!(*monitoring, MonitoringMode::ContinuousApproximation);
                let pattern = (*bridge).expect("bridge deberia reconocerse para este patron");
                assert_eq!(pattern.barrier, 120.0);
                assert_eq!(pattern.direction, BarrierDirection::Up);
                assert_eq!(compiled.observable_slots[pattern.observable], "EQ.SPOT.AAPL");
            }
            other => panic!("se esperaba ContractOp::Trigger, se obtuvo {other:?}"),
        }
    }

    #[test]
    fn rejects_continuous_approximation_with_an_unrecognized_condition() {
        // Mismo BARRIER_JSON pero con los lados de la condicion intercambiados (constante a la
        // izquierda, current a la derecha) -- geometricamente equivalente pero no la forma
        // canonica que recognize_barrier_pattern sabe interpretar (§16: nunca ocultar una
        // aproximacion aplicandola a un patron que no se reconoce con certeza).
        let json = BARRIER_JSON.replace("\"discrete\"", "\"continuous_approximation\"").replace(
            r#""condition": {
                "type": "greater_equal",
                "left": {"type": "current", "observable": "EQ.SPOT.AAPL"},
                "right": {"type": "constant", "value": 120.0}
            },"#,
            r#""condition": {
                "type": "less_equal",
                "left": {"type": "constant", "value": 120.0},
                "right": {"type": "current", "observable": "EQ.SPOT.AAPL"}
            },"#,
        );
        let err = compile(&json).expect_err("condicion con los lados intercambiados no deberia reconocerse");
        assert!(err.contains("continuous_approximation") || err.contains("current"));
    }

    // CSE (common-subexpression elimination, PLAN_PRODUCTS.md §8 punto 4, Fase 11): dos ramas de
    // 'both' con subarboles JSON estructuralmente identicos pero como nodos SEPARADOS (no el mismo
    // objeto Rust) deben compartir indice tras compilar.
    const DOUBLE_CASHFLOW_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "DOUBLE_CASHFLOW",
        "contract": {
            "type": "when",
            "time": 1.0,
            "child": {
                "type": "both",
                "children": [
                    {"type": "cashflow", "currency": "USD", "amount": {"type": "mul",
                        "left": {"type": "constant", "value": 1000.0},
                        "right": {"type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}}}},
                    {"type": "cashflow", "currency": "USD", "amount": {"type": "mul",
                        "left": {"type": "constant", "value": 1000.0},
                        "right": {"type": "sub",
                            "left": {"type": "fixing", "observable": "EQ.SPOT.AAPL", "time": 1.0},
                            "right": {"type": "constant", "value": 100.0}}}}
                ]
            }
        }
    }"#;

    #[test]
    fn cse_deduplicates_two_structurally_identical_cashflow_subtrees() {
        let compiled = compile(DOUBLE_CASHFLOW_JSON).expect("both con hijos identicos deberia compilar");
        // Un unico subarbol de 'amount' compilado (constant(1000), fixing, constant(100), sub,
        // mul): 5 nodos escalares, no 10.
        assert_eq!(compiled.scalar_ops.len(), 5);
        // Un unico ContractOp::Cashflow (no dos), y 'Both' referencia el MISMO indice dos veces.
        assert_eq!(
            compiled.contract_ops.iter().filter(|op| matches!(op, ContractOp::Cashflow { .. })).count(),
            1
        );
        let when_child = match &compiled.contract_ops[compiled.root] {
            ContractOp::When { child, .. } => *child,
            other => panic!("se esperaba ContractOp::When, se obtuvo {other:?}"),
        };
        match &compiled.contract_ops[when_child] {
            ContractOp::Both(children) => {
                assert_eq!(children.len(), 2);
                assert_eq!(children[0], children[1], "ambos hijos de Both deberian compartir indice");
            }
            other => panic!("se esperaba ContractOp::Both, se obtuvo {other:?}"),
        }
    }

    #[test]
    fn cse_sharing_a_cashflow_index_still_pays_it_twice_when_evaluated() {
        // La correccion semantica de 'Both' (duplica pagos identicos, ADR-P0-02) no debe romperse
        // por compartir representacion en el IR: evaluar debe seguir produciendo DOS cashflows.
        use crate::payoff::eval::{evaluate, ObservablePath};

        struct ConstantSpot(f64);
        impl ObservablePath for ConstantSpot {
            fn value_at(&self, _slot: usize, _time: f64) -> f64 {
                self.0
            }
        }

        let compiled = compile(DOUBLE_CASHFLOW_JSON).unwrap();
        let ledger = evaluate(&compiled, &ConstantSpot(120.0));
        assert_eq!(ledger.len(), 2, "Both debe seguir pagando dos veces aunque comparta el subarbol");
        for cf in &ledger {
            assert_eq!(cf.payment_time, 1.0);
            assert_eq!(cf.amount, 1000.0 * 20.0);
        }
    }

    // CSE tambien debe deduplicar un observable 'current' repetido en dos ramas de un 'if'.
    const IF_WITH_REPEATED_CURRENT_JSON: &str = r#"{
        "schema": "engine.payoff/v1",
        "id": "IF_REPEATED_CURRENT",
        "contract": {
            "type": "if",
            "condition": {"type": "greater",
                "left": {"type": "current", "observable": "EQ.SPOT.AAPL"},
                "right": {"type": "constant", "value": 100.0}},
            "if_true": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD",
                "amount": {"type": "current", "observable": "EQ.SPOT.AAPL"}}},
            "if_false": {"type": "when", "time": 1.0, "child": {"type": "cashflow", "currency": "USD",
                "amount": {"type": "current", "observable": "EQ.SPOT.AAPL"}}}
        }
    }"#;

    #[test]
    fn cse_deduplicates_repeated_current_observable_across_if_branches() {
        let compiled = compile(IF_WITH_REPEATED_CURRENT_JSON).expect("if deberia compilar");
        let current_indices: Vec<usize> = compiled
            .scalar_ops
            .iter()
            .enumerate()
            .filter(|(_, op)| matches!(op, ScalarOp::Current { .. }))
            .map(|(i, _)| i)
            .collect();
        assert_eq!(
            current_indices.len(),
            1,
            "el 'current' de la condicion y los dos de las ramas deberian compartir un unico indice"
        );
    }

    #[test]
    fn rejects_multi_currency_contracts() {
        let json = r#"{
            "schema": "engine.payoff/v1",
            "id": "x",
            "contract": {
                "type": "both",
                "children": [
                    {"type": "cashflow", "currency": "USD", "amount": {"type": "constant", "value": 1.0}},
                    {"type": "cashflow", "currency": "EUR", "amount": {"type": "constant", "value": 1.0}}
                ]
            }
        }"#;
        let err = compile(json).expect_err("multi-moneda no deberia soportarse en Fase 5");
        assert!(err.contains("multi-moneda"));
    }
}
