//! Shared exposure graph and bounded XVA reductions.
//!
//! This implementation intentionally uses simple, auditable formulas.  It is a contract
//! baseline, not a regulatory capital model: hazard rates are piecewise constant, CVA/DVA
//! use discounted expected positive/negative exposure, and FVA/MVA/KVA are spread/capital
//! integrations over the same exposure rows.

use quant_domain::{
    hash_value, CollateralAgreement, ExposureCubeInput, ExposureCubeRef, FundingSpec,
    ProbabilityMeasure, XvaMeasure, XvaProvenance, XvaRequest, XvaResult,
};
use std::collections::BTreeMap;

fn clamp_recovery(value: f64) -> f64 {
    value.clamp(0.0, 1.0)
}
fn dt(times: &[f64], i: usize) -> f64 {
    if i == 0 {
        times[i].max(0.0)
    } else {
        (times[i] - times[i - 1]).max(0.0)
    }
}
fn df(cube: &ExposureCubeInput, i: usize) -> f64 {
    cube.discount_factors
        .get(i)
        .copied()
        .unwrap_or_else(|| (-0.0_f64 * cube.times[i]).exp())
}

fn default_increment(spec: &quant_domain::DefaultSpec, i: usize, times: &[f64]) -> f64 {
    if let Some(survival) = spec.survival_probabilities.get(i) {
        let previous = if i == 0 {
            1.0
        } else {
            spec.survival_probabilities
                .get(i - 1)
                .copied()
                .unwrap_or(1.0)
        };
        return (previous - survival).max(0.0);
    }
    let h = spec.hazard_rate.max(0.0);
    let start = if i == 0 { 0.0 } else { times[i - 1] };
    (-(h * start)).exp() - (-(h * times[i].max(start))).exp()
}

fn collateral_adjust(value: f64, csa: Option<&CollateralAgreement>) -> f64 {
    let Some(csa) = csa else {
        return value;
    };
    if !csa.variation_margin {
        return value;
    }
    let threshold = csa.threshold.max(0.0);
    let collateral = (value.abs() - threshold).max(0.0);
    if collateral < csa.minimum_transfer_amount.max(0.0) {
        value
    } else if value >= 0.0 {
        value - collateral - csa.independent_amount.max(0.0)
    } else {
        value + collateral + csa.independent_amount.max(0.0)
    }
}

fn quantile(values: &[f64], confidence: f64) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let p = confidence.clamp(0.0, 1.0);
    let index = ((sorted.len() - 1) as f64 * p).round() as usize;
    sorted[index]
}

fn artifact(cube: &ExposureCubeInput, materialized: bool, lineage: Vec<String>) -> ExposureCubeRef {
    let hash = hash_value(&serde_json::to_value(cube).expect("exposure cube serializable"));
    let bytes = cube
        .paths
        .iter()
        .map(|row| row.len() * std::mem::size_of::<f64>())
        .sum::<usize>() as u64;
    ExposureCubeRef {
        artifact_id: format!("exposure-{}", hash.as_str().trim_start_matches("sha256:")),
        hash,
        measure: cube.measure,
        shape: [cube.times.len(), cube.paths.first().map_or(0, Vec::len)],
        materialized,
        bytes,
        lineage,
    }
}

/// Calculate all requested XVAs from one shared exposure graph.  `q` drives EE/PFE/CVA
/// and funding measures; `p` drives DVA.  Rows are reduced one time bucket at a time.
pub fn calculate(
    request: &XvaRequest,
    q: &ExposureCubeInput,
    p: Option<&ExposureCubeInput>,
) -> Result<XvaResult, String> {
    q.validate()?;
    if q.measure != ProbabilityMeasure::Q {
        return Err("q_exposure must be tagged Q".into());
    }
    if let (Some(source), Some(context)) = (&q.source_hash, &request.context.context_hash) {
        if source != context {
            return Err("Q exposure source hash does not match context hash".into());
        }
    }
    if let Some(p_cube) = p {
        p_cube.validate()?;
        if p_cube.measure != ProbabilityMeasure::P {
            return Err("p_exposure must be tagged P".into());
        }
        if p_cube.times != q.times {
            return Err("Q and P exposure grids must match".into());
        }
        if let (Some(source), Some(context)) = (&p_cube.source_hash, &request.context.context_hash)
        {
            if source != context {
                return Err("P exposure source hash does not match context hash".into());
            }
        }
    }
    if !(0.0..=1.0).contains(&request.confidence_level) {
        return Err("confidence_level must be in [0,1]".into());
    }
    let csa = request.collateral_agreements.first();
    let mut ee = Vec::with_capacity(q.times.len());
    let mut pfe = Vec::with_capacity(q.times.len());
    let mut cva = 0.0;
    let mut dva = 0.0;
    let mut fva = 0.0;
    let mut mva = 0.0;
    let mut kva = 0.0;
    for (i, row) in q.paths.iter().enumerate() {
        let adjusted: Vec<f64> = row.iter().map(|v| collateral_adjust(*v, csa)).collect();
        let positive: Vec<f64> = adjusted.iter().map(|v| v.max(0.0)).collect();
        let negative: Vec<f64> = adjusted.iter().map(|v| (-*v).max(0.0)).collect();
        let expected_positive = positive.iter().sum::<f64>() / positive.len() as f64;
        let expected_negative = negative.iter().sum::<f64>() / negative.len() as f64;
        let discount = df(q, i);
        ee.push(expected_positive);
        pfe.push(quantile(&positive, request.confidence_level));
        let year_fraction = dt(&q.times, i);
        cva += discount
            * expected_positive
            * default_increment(&request.counterparty_default, i, &q.times)
            * (1.0 - clamp_recovery(request.counterparty_default.recovery_rate));
        fva += discount * expected_positive * request.funding.funding_spread * year_fraction;
        mva += discount
            * request
                .collateral_agreements
                .iter()
                .map(|c| c.initial_margin.max(0.0))
                .sum::<f64>()
            * request.funding.borrowing_spread
            * year_fraction;
        kva +=
            discount * expected_positive * request.funding.capital_factor.max(0.0) * year_fraction;
        if let Some(p_cube) = p {
            let p_adjusted: Vec<f64> = p_cube.paths[i]
                .iter()
                .map(|v| collateral_adjust(*v, csa))
                .collect();
            let expected_negative_p =
                p_adjusted.iter().map(|v| (-*v).max(0.0)).sum::<f64>() / p_adjusted.len() as f64;
            dva += discount
                * expected_negative_p
                * default_increment(&request.own_default, i, &q.times)
                * (1.0 - clamp_recovery(request.own_default.recovery_rate));
        } else {
            dva += discount
                * expected_negative
                * default_increment(&request.own_default, i, &q.times)
                * (1.0 - clamp_recovery(request.own_default.recovery_rate));
        }
    }
    let mut measures = BTreeMap::new();
    measures.insert(XvaMeasure::Cva.as_str().into(), cva);
    measures.insert(XvaMeasure::Dva.as_str().into(), dva);
    measures.insert(XvaMeasure::Fva.as_str().into(), fva);
    measures.insert(XvaMeasure::Mva.as_str().into(), mva);
    measures.insert(XvaMeasure::Kva.as_str().into(), kva);
    let mut approximations = vec![
        "piecewise-constant hazard and simple discounted exposure integration".into(),
        "all requested measures reuse one Q exposure reduction graph".into(),
    ];
    if p.is_none() {
        approximations.push("DVA falls back to Q exposure because no P cube was supplied".into());
    }
    if csa.is_some() {
        approximations.push(
            "collateral is represented as thresholded variation margin from the first CSA".into(),
        );
    }
    let mut lineage = vec![
        request.context.context_hash.as_ref().map_or_else(
            || "context:unhashed".into(),
            |hash| hash.as_str().to_owned(),
        ),
        format!("operation:{}", request.operation_id),
    ];
    lineage.extend(
        request
            .netting_sets
            .iter()
            .map(|set| format!("netting-set:{}", set.id)),
    );
    lineage.extend(
        request
            .collateral_agreements
            .iter()
            .map(|csa| format!("csa:{}", csa.id)),
    );
    let cube_ref = artifact(q, request.materialize_exposure, lineage.clone());
    let mut artifact_lineage = lineage;
    artifact_lineage.push(cube_ref.artifact_id.clone());
    artifact_lineage.push(cube_ref.hash.as_str().to_owned());
    let provenance = XvaProvenance {
        context_hash: request.context.context_hash.clone(),
        operation_id: request.operation_id.clone(),
        q_measure: true,
        p_measure: p.is_some(),
        shared_exposure_graph: true,
        approximations,
        artifact_lineage,
    };
    Ok(XvaResult {
        operation_id: request.operation_id.clone(),
        context_hash: request.context.context_hash.clone(),
        times: q.times.clone(),
        ee,
        pfe,
        cva,
        dva,
        fva,
        mva,
        kva,
        measures,
        exposure_cube: Some(cube_ref.clone()),
        artifacts: vec![cube_ref],
        provenance,
    })
}

pub fn estimate_memory_bytes(cube: &ExposureCubeInput) -> u64 {
    cube.paths
        .iter()
        .map(|row| row.len() as u64 * 8)
        .sum::<u64>()
}

#[allow(dead_code)]
fn _funding_spread(spec: &FundingSpec) -> f64 {
    if spec.funding_spread != 0.0 {
        spec.funding_spread
    } else {
        spec.borrowing_spread - spec.lending_spread
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use quant_domain::{
        DefaultSpec, FundingSpec, PricingContext, QuantContext, ResourceRef, XvaRequest,
    };

    fn request() -> XvaRequest {
        XvaRequest {
            context: QuantContext::new("xva-test"),
            operation_id: "xva-1".into(),
            portfolio_id: "book".into(),
            market: ResourceRef {
                id: "m".into(),
                hash: "h".into(),
                kind: "market".into(),
                version: 1,
            },
            model: ResourceRef {
                id: "model".into(),
                hash: "h".into(),
                kind: "model".into(),
                version: 1,
            },
            netting_sets: Vec::new(),
            collateral_agreements: Vec::new(),
            counterparty_default: DefaultSpec {
                hazard_rate: 0.02,
                recovery_rate: 0.4,
                ..Default::default()
            },
            own_default: DefaultSpec {
                hazard_rate: 0.02,
                recovery_rate: 0.4,
                measure: ProbabilityMeasure::P,
                ..Default::default()
            },
            funding: FundingSpec::default(),
            measures: vec![
                XvaMeasure::Ee,
                XvaMeasure::Pfe,
                XvaMeasure::Cva,
                XvaMeasure::Dva,
            ],
            confidence_level: 0.95,
            q_exposure: None,
            p_exposure: None,
            materialize_exposure: false,
            memory_budget_bytes: None,
            pricing: PricingContext::default(),
        }
    }

    fn cube(measure: ProbabilityMeasure, values: Vec<Vec<f64>>) -> ExposureCubeInput {
        ExposureCubeInput {
            measure,
            times: vec![1.0, 2.0],
            paths: values,
            discount_factors: vec![1.0, 1.0],
            source_hash: None,
        }
    }

    #[test]
    fn hazard_zero_and_full_recovery_remove_default_adjustments() {
        let mut req = request();
        req.counterparty_default.hazard_rate = 0.0;
        req.counterparty_default.recovery_rate = 1.0;
        let result = calculate(
            &req,
            &cube(ProbabilityMeasure::Q, vec![vec![10.0], vec![10.0]]),
            None,
        )
        .unwrap();
        assert_eq!(result.cva, 0.0);
    }

    #[test]
    fn q_p_symmetry_gives_equal_cva_and_dva() {
        let req = request();
        let q = cube(
            ProbabilityMeasure::Q,
            vec![vec![10.0, -10.0], vec![10.0, -10.0]],
        );
        let p = cube(
            ProbabilityMeasure::P,
            vec![vec![-10.0, 10.0], vec![-10.0, 10.0]],
        );
        let result = calculate(&req, &q, Some(&p)).unwrap();
        assert!((result.cva - result.dva).abs() < 1e-12);
    }

    #[test]
    fn repeated_reduction_is_deterministic_and_exposes_artifact_bytes() {
        let req = request();
        let q = cube(ProbabilityMeasure::Q, vec![vec![1.0, 3.0], vec![2.0, 4.0]]);
        let first = calculate(&req, &q, None).unwrap();
        let second = calculate(&req, &q, None).unwrap();
        assert_eq!(first, second);
        assert_eq!(first.exposure_cube.as_ref().unwrap().bytes, 32);
        assert!(!first.provenance.shared_exposure_graph || first.provenance.q_measure);
    }

    #[test]
    fn invalid_measure_is_rejected_instead_of_silently_mixing_q_and_p() {
        let req = request();
        let error = calculate(
            &req,
            &cube(ProbabilityMeasure::P, vec![vec![1.0], vec![1.0]]),
            None,
        )
        .unwrap_err();
        assert!(error.contains("tagged Q"));
    }

    #[test]
    fn exposure_context_mismatch_is_rejected() {
        let req = request();
        let mut q = cube(ProbabilityMeasure::Q, vec![vec![1.0], vec![1.0]]);
        q.source_hash = Some(quant_domain::ContextHash("sha256:other".into()));
        let error = calculate(&req, &q, None).unwrap_err();
        assert!(error.contains("source hash"));
    }
}
