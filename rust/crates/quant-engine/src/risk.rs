//! Phase-5 risk planner.  Measures are small strategy functions over a shared artifact graph;
//! they are not represented by one trait with a method per possible Greek.

use super::{simd::sum_f64, Engine, QuantError};
use engine_core::api::HullWhite1FGreeks;
use quant_domain::{
    BumpScheme, ContextHash, ModelSpec, ProductSpec, RiskArtifact, RiskFactor, RiskMeasure,
    RiskProvenance, RiskRequest, RiskResult,
};
use serde_json::Value;
use std::collections::{BTreeMap, HashMap};

const DEFAULT_BUMP: f64 = 1.0e-4;
const DEFAULT_TOLERANCE: f64 = 1.0e-6;
const PV01_SCALE: f64 = 1.0e-4;

type IrsInputs = (
    f64,
    f64,
    f64,
    f64,
    f64,
    f64,
    bool,
    f64,
    Vec<f64>,
    Vec<f64>,
    String,
);
type TradeRiskState = (
    String,
    f64,
    Option<HullWhite1FGreeks>,
    ProductSpec,
    IrsInputs,
);

#[derive(Debug, Clone)]
struct ArtifactGraph {
    context: String,
    market_hash: String,
    model_hash: String,
    market_version: u64,
    model_version: u64,
    base: HashMap<String, f64>,
    aad: HashMap<String, HullWhite1FGreeks>,
    bumps: HashMap<String, f64>,
    artifacts: BTreeMap<String, RiskArtifact>,
    valuation_calls: usize,
    aad_passes: usize,
}

impl ArtifactGraph {
    fn new(
        request: &RiskRequest,
        market_hash: &ContextHash,
        model_hash: &ContextHash,
        market_version: u64,
        model_version: u64,
    ) -> Self {
        Self {
            context: request
                .context
                .context_hash
                .as_ref()
                .map(|h| h.0.clone())
                .unwrap_or_else(|| "unhashed".into()),
            market_hash: market_hash.0.clone(),
            model_hash: model_hash.0.clone(),
            market_version,
            model_version,
            base: HashMap::new(),
            aad: HashMap::new(),
            bumps: HashMap::new(),
            artifacts: BTreeMap::new(),
            valuation_calls: 0,
            aad_passes: 0,
        }
    }

    fn key(&self, product_hash: &str, kind: &str) -> String {
        format!(
            "ctx={}|market={}@{}|model={}@{}|product={}|artifact={kind}",
            self.context,
            self.market_hash,
            self.market_version,
            self.model_hash,
            self.model_version,
            product_hash
        )
    }

    fn record(&mut self, key: String, kind: &str, dependencies: Vec<String>, reused: bool) {
        if let Some(existing) = self.artifacts.get_mut(&key) {
            existing.reused |= reused;
        } else {
            self.artifacts.insert(
                key.clone(),
                RiskArtifact {
                    key,
                    kind: kind.to_owned(),
                    dependencies,
                    reused,
                },
            );
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Target {
    R0,
    A,
    B,
    Sigma,
    Unsupported,
}

fn target(factor: &RiskFactor) -> Target {
    match factor {
        RiskFactor::CurveParallel { .. }
        | RiskFactor::CurveBucket { .. }
        | RiskFactor::CurvePillar { .. } => Target::R0,
        RiskFactor::ModelParameter { name } => match name.to_ascii_lowercase().as_str() {
            "r0" | "rate" | "spot" | "discount_rate" => Target::R0,
            "a" => Target::A,
            "b" => Target::B,
            "sigma" | "vol" | "volatility" => Target::Sigma,
            _ => Target::Unsupported,
        },
        RiskFactor::CreditParameter { .. } | RiskFactor::TimeShift { .. } => Target::Unsupported,
    }
}

fn factors_for(measure: RiskMeasure, requested: &[RiskFactor]) -> Vec<Option<RiskFactor>> {
    if measure == RiskMeasure::Pv {
        return vec![None];
    }
    if !requested.is_empty() {
        return requested.iter().cloned().map(Some).collect();
    }
    let factor = match measure {
        RiskMeasure::Pv01 | RiskMeasure::Delta | RiskMeasure::Rho => {
            RiskFactor::model_parameter("r0")
        }
        RiskMeasure::Vega => RiskFactor::model_parameter("sigma"),
        RiskMeasure::Pv | RiskMeasure::AllGreeks => RiskFactor::model_parameter("r0"),
    };
    vec![Some(factor)]
}

fn expand_measures(measures: &[RiskMeasure]) -> Vec<RiskMeasure> {
    let source = if measures.is_empty() {
        vec![RiskMeasure::AllGreeks]
    } else {
        measures.to_vec()
    };
    source
        .into_iter()
        .flat_map(|measure| match measure {
            RiskMeasure::AllGreeks => vec![
                RiskMeasure::Pv01,
                RiskMeasure::Delta,
                RiskMeasure::Rho,
                RiskMeasure::Vega,
            ],
            other => vec![other],
        })
        .collect()
}

fn number(params: &BTreeMap<String, Value>, key: &str, default: f64) -> Result<f64, QuantError> {
    let value = params.get(key).and_then(Value::as_f64).unwrap_or(default);
    if value.is_finite() {
        Ok(value)
    } else {
        Err(QuantError::InvalidRequest(format!("{key} must be finite")))
    }
}

fn product_params(product: &ProductSpec) -> Result<&BTreeMap<String, Value>, QuantError> {
    match product {
        ProductSpec::IrSwap(spec) => Ok(&spec.params),
        _ => Err(QuantError::InvalidRequest(
            "risk currently supports ir_swap only".into(),
        )),
    }
}

fn vec_number(params: &BTreeMap<String, Value>, key: &str) -> Result<Vec<f64>, QuantError> {
    params
        .get(key)
        .and_then(Value::as_array)
        .ok_or_else(|| QuantError::InvalidRequest(format!("{key} is required")))?
        .iter()
        .map(|v| {
            v.as_f64().filter(|x| x.is_finite()).ok_or_else(|| {
                QuantError::InvalidRequest(format!("{key} contains a non-finite number"))
            })
        })
        .collect()
}

#[allow(clippy::type_complexity)]
fn irs_inputs(
    model: &ModelSpec,
    market: &quant_domain::MarketHandle,
    product: &ProductSpec,
) -> Result<IrsInputs, QuantError> {
    let model_params = match model {
        ModelSpec::HullWhite1F(spec) => &spec.params,
        _ => {
            return Err(QuantError::InvalidRequest(
                "risk currently supports hull_white_1f only".into(),
            ))
        }
    };
    let product_params = product_params(product)?;
    let a = number(model_params, "a", 0.1)?;
    let b = number(model_params, "b", 0.03)?;
    let sigma = number(model_params, "sigma", 0.01)?;
    let r0 = number(&market.snapshot().spec.params, "r0", 0.02)?;
    let notional = number(product_params, "notional", 1_000_000.0)?;
    let fixed_rate = number(product_params, "fixed_rate", 0.0)?;
    let use_par_rate = product_params
        .get("use_par_rate")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let start = number(product_params, "start", 0.0)?;
    let payment_times = vec_number(product_params, "payment_times")?;
    let accruals = vec_number(product_params, "accruals")?;
    if payment_times.is_empty() || payment_times.len() != accruals.len() {
        return Err(QuantError::InvalidRequest(
            "payment_times and accruals must have the same non-zero length".into(),
        ));
    }
    let product_hash = product.hash().0.clone();
    Ok((
        a,
        b,
        sigma,
        r0,
        notional,
        fixed_rate,
        use_par_rate,
        start,
        payment_times,
        accruals,
        product_hash,
    ))
}

/// Convert a par-rate product to a fixed coupon once.  Bumps then keep the coupon unchanged,
/// which is the economically correct sensitivity for an already-issued swap.
#[allow(clippy::too_many_arguments)]
fn fixed_coupon(
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
) -> Result<f64, QuantError> {
    if !use_par_rate {
        return Ok(fixed_rate);
    }
    let zero = engine_core::api::irs_hull_white_npv(
        a,
        b,
        sigma,
        r0,
        notional,
        0.0,
        false,
        start,
        payment_times.to_vec(),
        accruals.to_vec(),
    );
    let one = engine_core::api::irs_hull_white_npv(
        a,
        b,
        sigma,
        r0,
        notional,
        1.0,
        false,
        start,
        payment_times.to_vec(),
        accruals.to_vec(),
    );
    let annuity = zero - one;
    if annuity.abs() <= f64::EPSILON {
        return Err(QuantError::InvalidRequest(
            "par-rate annuity is zero".into(),
        ));
    }
    Ok(zero / annuity)
}

#[allow(clippy::too_many_arguments)]
fn value(
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
) -> f64 {
    engine_core::api::irs_hull_white_npv(
        a,
        b,
        sigma,
        r0,
        notional,
        fixed_rate,
        false,
        start,
        payment_times.to_vec(),
        accruals.to_vec(),
    )
}

#[allow(clippy::too_many_arguments)]
fn bump_value(
    base: f64,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    factor: &RiskFactor,
    h: f64,
    notional: f64,
    fixed_rate: f64,
    start: f64,
    payment_times: &[f64],
    accruals: &[f64],
    scheme: BumpScheme,
) -> Result<f64, QuantError> {
    let mut up = (a, b, sigma, r0);
    match target(factor) {
        Target::R0 => up.3 += h,
        Target::A => up.0 += h,
        Target::B => up.1 += h,
        Target::Sigma => up.2 += h,
        Target::Unsupported => return Err(QuantError::UnsupportedMeasure(factor.to_string())),
    }
    let up_value = value(
        up.0,
        up.1,
        up.2,
        up.3,
        notional,
        fixed_rate,
        start,
        payment_times,
        accruals,
    );
    if scheme == BumpScheme::OneSided {
        return Ok((up_value - base) / h);
    }
    let mut down = (a, b, sigma, r0);
    match target(factor) {
        Target::R0 => down.3 -= h,
        Target::A => down.0 -= h,
        Target::B => down.1 -= h,
        Target::Sigma => down.2 -= h,
        Target::Unsupported => unreachable!(),
    }
    let down_value = value(
        down.0,
        down.1,
        down.2,
        down.3,
        notional,
        fixed_rate,
        start,
        payment_times,
        accruals,
    );
    Ok((up_value - down_value) / (2.0 * h))
}

fn aad_value(greeks: HullWhite1FGreeks, factor: &RiskFactor) -> Option<f64> {
    match target(factor) {
        Target::R0 => Some(greeks.d_r0),
        Target::A => Some(greeks.d_a),
        Target::B => Some(greeks.d_b),
        Target::Sigma => Some(greeks.d_sigma),
        Target::Unsupported => None,
    }
}

fn bucket_weight(product: &ProductSpec, factor: &RiskFactor) -> f64 {
    let bucket = match factor {
        RiskFactor::CurveBucket { bucket, .. } => *bucket,
        RiskFactor::CurvePillar { pillar, .. } => *pillar,
        _ => return 1.0,
    };
    let Ok(params) = product_params(product) else {
        return 0.0;
    };
    let n = params
        .get("payment_times")
        .and_then(Value::as_array)
        .map(Vec::len)
        .unwrap_or(0);
    if bucket < n && n > 0 {
        1.0 / n as f64
    } else {
        0.0
    }
}

impl Engine {
    /// Build one bounded risk graph for the request.  Base values, AAD tapes and bumps are
    /// keyed by context/market/model/product versions and shared by all requested measures.
    pub fn calculate_risk(&self, request: &RiskRequest) -> Result<RiskResult, QuantError> {
        if request.operation_id.trim().is_empty() {
            return Err(QuantError::InvalidRequest(
                "operation_id is required".into(),
            ));
        }
        if request.context.context_hash.is_some() && !request.context.verify_hash() {
            return Err(QuantError::InvalidRequest(
                "context_hash does not match context".into(),
            ));
        }
        let portfolio = request.context.portfolio(&request.portfolio_id)?;
        let market = request.context.market_handle(&request.market.id)?;
        let model = request.context.model_handle(&request.model.id)?;
        if !request.market.hash.is_empty() && request.market.hash != market.hash().0 {
            return Err(QuantError::InvalidRequest(
                "market reference is incompatible with context".into(),
            ));
        }
        if !request.model.hash.is_empty() && request.model.hash != model.hash().0 {
            return Err(QuantError::InvalidRequest(
                "model reference is incompatible with context".into(),
            ));
        }
        let measures = expand_measures(&request.measures);
        let factors: Vec<Vec<Option<RiskFactor>>> = measures
            .iter()
            .map(|m| factors_for(*m, &request.factors))
            .collect();
        let market_hash = market.hash().clone();
        let model_hash = model.hash().clone();
        let mut graph = ArtifactGraph::new(
            request,
            &market_hash,
            &model_hash,
            market.version(),
            model.version(),
        );
        let model_spec = &model.entry().spec;
        let factor_index: Vec<String> = factors
            .iter()
            .flat_map(|row| row.iter())
            .map(|factor| {
                factor
                    .as_ref()
                    .map(ToString::to_string)
                    .unwrap_or_else(|| "none".into())
            })
            .fold(Vec::new(), |mut all, key| {
                if !all.contains(&key) {
                    all.push(key);
                }
                all
            });
        let mut base_values = Vec::with_capacity(portfolio.trades.len());
        let mut trade_ids = Vec::with_capacity(portfolio.trades.len());
        let mut per_trade: Vec<TradeRiskState> = Vec::new();
        for trade in &portfolio.trades {
            let product = trade.product.clone().ok_or_else(|| {
                QuantError::InvalidRequest(format!(
                    "product {} is not embedded in portfolio",
                    trade.product_id
                ))
            })?;
            let inputs = irs_inputs(model_spec, &market, &product)?;
            let (
                a,
                b,
                sigma,
                r0,
                notional,
                fixed_rate,
                use_par_rate,
                start,
                times,
                accruals,
                product_hash,
            ) = &inputs;
            let coupon = fixed_coupon(
                *a,
                *b,
                *sigma,
                *r0,
                *notional,
                *fixed_rate,
                *use_par_rate,
                *start,
                times,
                accruals,
            )?;
            let cache_key = product_hash.clone();
            let reused = graph.base.contains_key(&cache_key);
            let base_scalar = if let Some(base) = graph.base.get(&cache_key).copied() {
                base
            } else {
                graph.valuation_calls += 1;
                let base = value(
                    *a, *b, *sigma, *r0, *notional, coupon, *start, times, accruals,
                );
                graph.base.insert(cache_key.clone(), base);
                base
            };
            let pv = base_scalar * trade.quantity;
            let key = graph.key(product_hash, "base_valuation");
            graph.record(key, "base_valuation", vec![], reused);
            base_values.push(pv);
            trade_ids.push(trade.trade_id.0.clone());
            per_trade.push((cache_key, trade.quantity, None, product, inputs));
        }
        // This is the measured reduction hot spot for the risk portfolio path.  SIMD remains a
        // CPU strategy selected by the request policy; it never changes the Burn device/backend.
        let base_value = sum_f64(&base_values, &request.pricing.execution.cpu_vector);
        let mut dense = vec![0.0; measures.len() * factor_index.len()];
        let mut provenance = BTreeMap::new();
        for (row, measure) in measures.iter().enumerate() {
            let row_factors = &factors[row];
            for factor in row_factors {
                let factor_key = factor
                    .as_ref()
                    .map(ToString::to_string)
                    .unwrap_or_else(|| "none".into());
                let mut aggregate = if *measure == RiskMeasure::Pv {
                    base_value
                } else {
                    0.0
                };
                let mut used_bump = false;
                for (cache_key, quantity, aad_slot, product, inputs) in &mut per_trade {
                    if *measure == RiskMeasure::Pv {
                        continue;
                    }
                    let Some(factor) = factor.as_ref() else {
                        continue;
                    };
                    let (
                        a,
                        b,
                        sigma,
                        r0,
                        notional,
                        fixed_rate,
                        use_par_rate,
                        start,
                        times,
                        accruals,
                        product_hash,
                    ) = inputs;
                    let coupon = fixed_coupon(
                        *a,
                        *b,
                        *sigma,
                        *r0,
                        *notional,
                        *fixed_rate,
                        *use_par_rate,
                        *start,
                        times,
                        accruals,
                    )?;
                    // A one-sided request is an explicit numerical-method choice.  Central
                    // bumps may use the existing AAD tape; one-sided and curve buckets stay
                    // on bump-and-revalue for auditable provenance.
                    let aad = if request.bump_scheme == BumpScheme::Central {
                        aad_value_for(
                            &mut graph,
                            cache_key,
                            aad_slot,
                            *a,
                            *b,
                            *sigma,
                            *r0,
                            *notional,
                            coupon,
                            *use_par_rate,
                            *start,
                            times,
                            accruals,
                            factor,
                        )?
                    } else {
                        None
                    };
                    let derivative = if let Some(value) = aad {
                        value
                    } else {
                        used_bump = true;
                        let h = request.bump_size.unwrap_or(DEFAULT_BUMP);
                        let bump_key = format!(
                            "{}:{}:{}:{h:?}:{:?}",
                            cache_key, factor, graph.context, request.bump_scheme
                        );
                        if let Some(v) = graph.bumps.get(&bump_key) {
                            *v
                        } else {
                            let base = graph.base.get(cache_key).copied().unwrap_or_else(|| {
                                value(
                                    *a, *b, *sigma, *r0, *notional, coupon, *start, times, accruals,
                                )
                            });
                            graph.valuation_calls += if request.bump_scheme == BumpScheme::Central {
                                2
                            } else {
                                1
                            };
                            let v = bump_value(
                                base,
                                *a,
                                *b,
                                *sigma,
                                *r0,
                                factor,
                                h,
                                *notional,
                                coupon,
                                *start,
                                times,
                                accruals,
                                request.bump_scheme,
                            )?;
                            graph.bumps.insert(bump_key.clone(), v);
                            graph.record(
                                graph.key(product_hash, "bump_revaluation"),
                                "bump_revaluation",
                                vec![graph.key(product_hash, "base_valuation")],
                                false,
                            );
                            v
                        }
                    };
                    aggregate += derivative * *quantity * bucket_weight(product, factor);
                }
                if *measure == RiskMeasure::Pv01 {
                    aggregate *= PV01_SCALE;
                }
                let col = factor_index
                    .iter()
                    .position(|key| key == &factor_key)
                    .unwrap_or(0);
                dense[row * factor_index.len() + col] = aggregate;
                let tolerance = request
                    .tolerances
                    .get(&factor_key)
                    .copied()
                    .unwrap_or(DEFAULT_TOLERANCE);
                let provenance_entry =
                    provenance
                        .entry(measure.to_string())
                        .or_insert_with(|| RiskProvenance {
                            method: if *measure == RiskMeasure::Pv {
                                "base_valuation".into()
                            } else if used_bump {
                                "bump_and_revalue".into()
                            } else {
                                "aad".into()
                            },
                            bump_scheme: request.bump_scheme,
                            bump_size: request.bump_size.unwrap_or(DEFAULT_BUMP),
                            tolerance_abs: tolerance,
                            tolerance_rel: 1.0e-8,
                            context_hash: request.context.context_hash.clone(),
                            market_version: market.version(),
                            model_version: model.version(),
                            base_reused: true,
                            bump_reused: true,
                            aad_passes: graph.aad_passes,
                            valuation_calls: graph.valuation_calls,
                        });
                if used_bump && provenance_entry.method == "aad" {
                    provenance_entry.method = "mixed".into();
                }
            }
        }
        let shape = [measures.len(), factor_index.len()];
        for item in provenance.values_mut() {
            item.aad_passes = graph.aad_passes;
            item.valuation_calls = graph.valuation_calls;
        }
        Ok(RiskResult {
            operation_id: request.operation_id.clone(),
            context_hash: request.context.context_hash.clone(),
            base_value,
            base_values,
            trade_ids,
            measure_index: measures.iter().map(ToString::to_string).collect(),
            factor_index,
            shape,
            values: dense,
            provenance,
            artifacts: graph.artifacts.into_values().collect(),
        })
    }

    /// Cancellation-aware adapter used by the REST layer.  Risk is deterministic and bounded
    /// by portfolio size in this phase; the graph is admitted as one bounded chunk, preserving
    /// the cooperative cancellation contract used by portfolio/scenario pricing.
    pub fn calculate_risk_with_control(
        &self,
        request: &RiskRequest,
        control: &super::ExecutionControl,
    ) -> Result<RiskResult, QuantError> {
        control.check()?;
        let result = self.calculate_risk(request)?;
        control.mark_chunk();
        Ok(result)
    }
}

#[allow(clippy::too_many_arguments)]
fn aad_value_for(
    graph: &mut ArtifactGraph,
    cache_key: &str,
    slot: &mut Option<HullWhite1FGreeks>,
    a: f64,
    b: f64,
    sigma: f64,
    r0: f64,
    notional: f64,
    fixed_rate: f64,
    _use_par_rate: bool,
    start: f64,
    times: &[f64],
    accruals: &[f64],
    factor: &RiskFactor,
) -> Result<Option<f64>, QuantError> {
    // AAD exposes model scalars and a parallel state shock.  A curve pillar is a
    // piecewise market input, so it deliberately uses bump-and-revalue even though
    // its underlying scalar target is the current short rate.
    if target(factor) == Target::Unsupported
        || matches!(
            factor,
            RiskFactor::CurveBucket { .. } | RiskFactor::CurvePillar { .. }
        )
    {
        return Ok(None);
    }
    if slot.is_none() {
        if let Some(cached) = graph.aad.get(cache_key).copied() {
            *slot = Some(cached);
        } else {
            let greeks = engine_core::api::irs_hull_white_npv_all_greeks(
                a,
                b,
                sigma,
                r0,
                notional,
                fixed_rate,
                false,
                start,
                times.to_vec(),
                accruals.to_vec(),
            );
            graph.aad.insert(cache_key.to_owned(), greeks);
            *slot = Some(greeks);
            graph.aad_passes += 1;
            graph.record(
                graph.key(cache_key, "aad_tape"),
                "aad_tape",
                vec![graph.key(cache_key, "base_valuation")],
                false,
            );
        }
    }
    Ok(slot.as_ref().and_then(|g| aad_value(*g, factor)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use quant_domain::{ContextCommand, Portfolio, ResourceRef, RiskRequest, Trade};
    use serde_json::json;

    fn request(
        measures: Vec<RiskMeasure>,
        factors: Vec<RiskFactor>,
        scheme: BumpScheme,
    ) -> RiskRequest {
        let context = quant_domain::QuantContext::new("risk-test").apply(&[
            ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0": 0.02}) },
            ContextCommand::AddModel { id: "hw".into(), spec: json!({"a":0.1,"b":0.03,"sigma":0.01}) },
            ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional":1_000_000.0,"fixed_rate":0.02,"payment_times":[1.0,2.0,3.0],"accruals":[1.0,1.0,1.0]}) },
        ]).unwrap().0;
        let mut trade = Trade::new("t-1", "irs");
        trade.product = Some(context.product_handle("irs").unwrap().entry().spec.clone());
        let context = context
            .add_portfolio(Portfolio::new("book", vec![trade]))
            .unwrap();
        RiskRequest {
            context,
            operation_id: "risk-1".into(),
            portfolio_id: "book".into(),
            market: ResourceRef {
                id: "m".into(),
                hash: String::new(),
                kind: "market".into(),
                version: 1,
            },
            model: ResourceRef {
                id: "hw".into(),
                hash: String::new(),
                kind: "model".into(),
                version: 1,
            },
            measures,
            factors,
            bump_scheme: scheme,
            bump_size: Some(1e-5),
            tolerances: BTreeMap::new(),
            pricing: Default::default(),
        }
    }

    #[test]
    fn aad_delta_matches_central_bump_and_base_is_shared() {
        let engine = Engine::new(Default::default()).unwrap();
        let mut aad = request(vec![RiskMeasure::Delta], vec![], BumpScheme::Central);
        let result = engine.calculate_risk(&aad).unwrap();
        assert_eq!(result.shape, [1, 1]);
        assert_eq!(result.provenance["delta"].method, "aad");
        let aad_delta = result.values[0];
        aad.bump_scheme = BumpScheme::OneSided;
        let bumped = engine.calculate_risk(&aad).unwrap();
        assert!((aad_delta - bumped.values[0]).abs() < 1.0e3);
        assert_eq!(bumped.provenance["delta"].method, "bump_and_revalue");
        assert!(result.artifacts.iter().any(|a| a.kind == "base_valuation"));
    }

    #[test]
    fn curve_buckets_use_bump_scheme_and_aggregate_to_parallel() {
        let engine = Engine::new(Default::default()).unwrap();
        let factors = vec![
            RiskFactor::curve_bucket("discount", 0),
            RiskFactor::curve_bucket("discount", 1),
            RiskFactor::curve_bucket("discount", 2),
        ];
        let central = engine
            .calculate_risk(&request(
                vec![RiskMeasure::Rho],
                factors.clone(),
                BumpScheme::Central,
            ))
            .unwrap();
        let one_sided = engine
            .calculate_risk(&request(
                vec![RiskMeasure::Rho],
                factors,
                BumpScheme::OneSided,
            ))
            .unwrap();
        assert_eq!(central.shape, [1, 3]);
        assert_eq!(central.provenance["rho"].method, "bump_and_revalue");
        // The two first-order formulas differ by O(h) as expected; both remain finite and
        // bucket allocation is stable (the exact tolerance belongs to the factor policy).
        assert!(
            (central.values.iter().sum::<f64>() - one_sided.values.iter().sum::<f64>()).abs()
                < 1.0e3
        );
    }

    #[test]
    fn incompatible_reference_is_rejected_before_graph_reuse() {
        let engine = Engine::new(Default::default()).unwrap();
        let mut request = request(vec![RiskMeasure::Vega], vec![], BumpScheme::Central);
        request.market.hash = "sha256:wrong".into();
        assert!(
            matches!(engine.calculate_risk(&request), Err(QuantError::InvalidRequest(message)) if message.contains("incompatible"))
        );
    }
}
