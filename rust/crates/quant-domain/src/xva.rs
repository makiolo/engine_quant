//! XVA domain contracts.
//!
//! The domain keeps the probability measure explicit.  Exposure generated under Q is
//! not silently reused as P (or the other way around); the engine records the measure
//! in the exposure artifact and returns it in provenance.

use crate::{
    CollateralAgreementId, ContextHash, ContextHash as Hash, NettingSetId, PricingContext,
    QuantContext, ResourceRef,
};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "UPPERCASE")]
pub enum ProbabilityMeasure {
    #[default]
    Q,
    P,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct NettingSet {
    pub id: NettingSetId,
    #[serde(default)]
    pub counterparty: String,
    #[serde(default)]
    pub legal_entity: String,
    #[serde(default = "default_true")]
    pub closeout_netting: bool,
    #[serde(default)]
    pub collateral_agreement_id: Option<CollateralAgreementId>,
    #[serde(default)]
    pub metadata: BTreeMap<String, serde_json::Value>,
}

fn default_true() -> bool {
    true
}

impl NettingSet {
    pub fn new(id: impl Into<String>) -> Self {
        Self {
            id: NettingSetId::from(id.into()),
            counterparty: String::new(),
            legal_entity: String::new(),
            closeout_netting: true,
            collateral_agreement_id: None,
            metadata: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CollateralAgreement {
    pub id: CollateralAgreementId,
    #[serde(default = "default_currency")]
    pub currency: String,
    /// Positive threshold and minimum transfer amount in valuation currency.
    #[serde(default)]
    pub threshold: f64,
    #[serde(default)]
    pub minimum_transfer_amount: f64,
    #[serde(default)]
    pub independent_amount: f64,
    #[serde(default)]
    pub initial_margin: f64,
    #[serde(default)]
    pub variation_margin: bool,
    #[serde(default)]
    pub collateral_rate: f64,
    #[serde(default)]
    pub metadata: BTreeMap<String, serde_json::Value>,
}

fn default_currency() -> String {
    "USD".to_owned()
}

impl CollateralAgreement {
    pub fn new(id: impl Into<String>) -> Self {
        Self {
            id: CollateralAgreementId::from(id.into()),
            currency: default_currency(),
            threshold: 0.0,
            minimum_transfer_amount: 0.0,
            independent_amount: 0.0,
            initial_margin: 0.0,
            variation_margin: true,
            collateral_rate: 0.0,
            metadata: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DefaultSpec {
    #[serde(default)]
    pub hazard_rate: f64,
    #[serde(default = "default_recovery")]
    pub recovery_rate: f64,
    #[serde(default)]
    pub survival_probabilities: Vec<f64>,
    #[serde(default)]
    pub measure: ProbabilityMeasure,
}

fn default_recovery() -> f64 {
    0.4
}

impl Default for DefaultSpec {
    fn default() -> Self {
        Self {
            hazard_rate: 0.0,
            recovery_rate: default_recovery(),
            survival_probabilities: Vec::new(),
            measure: ProbabilityMeasure::Q,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FundingSpec {
    #[serde(default)]
    pub funding_spread: f64,
    #[serde(default)]
    pub borrowing_spread: f64,
    #[serde(default)]
    pub lending_spread: f64,
    #[serde(default)]
    pub capital_factor: f64,
    #[serde(default)]
    pub margin_period_years: f64,
}

impl Default for FundingSpec {
    fn default() -> Self {
        Self {
            funding_spread: 0.0,
            borrowing_spread: 0.0,
            lending_spread: 0.0,
            capital_factor: 0.08,
            margin_period_years: 1.0 / 252.0,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum XvaMeasure {
    Ee,
    Pfe,
    Cva,
    Dva,
    Fva,
    Mva,
    Kva,
}

impl XvaMeasure {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ee => "ee",
            Self::Pfe => "pfe",
            Self::Cva => "cva",
            Self::Dva => "dva",
            Self::Fva => "fva",
            Self::Mva => "mva",
            Self::Kva => "kva",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ExposureCubeInput {
    pub measure: ProbabilityMeasure,
    pub times: Vec<f64>,
    /// Row-major [time][path].  Keeping paths as chunks allows streaming reductions.
    pub paths: Vec<Vec<f64>>,
    #[serde(default)]
    pub discount_factors: Vec<f64>,
    #[serde(default)]
    pub source_hash: Option<ContextHash>,
}

impl ExposureCubeInput {
    pub fn validate(&self) -> Result<(), String> {
        if self.times.is_empty() || self.paths.len() != self.times.len() {
            return Err("exposure times and rows must be non-empty and have equal length".into());
        }
        if self
            .times
            .windows(2)
            .any(|w| !w[1].is_finite() || w[1] <= w[0])
            || self.times[0] < 0.0
        {
            return Err("exposure times must be finite and strictly increasing".into());
        }
        let width = self.paths[0].len();
        if width == 0
            || self
                .paths
                .iter()
                .any(|row| row.len() != width || row.iter().any(|v| !v.is_finite()))
        {
            return Err("exposure paths must be rectangular finite rows".into());
        }
        if !self.discount_factors.is_empty()
            && (self.discount_factors.len() != self.times.len()
                || self
                    .discount_factors
                    .iter()
                    .any(|v| !v.is_finite() || *v < 0.0))
        {
            return Err("discount_factors must match times and be non-negative".into());
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ExposureCubeRef {
    pub artifact_id: String,
    pub hash: Hash,
    pub measure: ProbabilityMeasure,
    pub shape: [usize; 2],
    pub materialized: bool,
    pub bytes: u64,
    pub lineage: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct XvaRequest {
    pub context: QuantContext,
    pub operation_id: String,
    pub portfolio_id: String,
    pub market: ResourceRef,
    pub model: ResourceRef,
    #[serde(default)]
    pub netting_sets: Vec<NettingSet>,
    #[serde(default)]
    pub collateral_agreements: Vec<CollateralAgreement>,
    #[serde(default)]
    pub counterparty_default: DefaultSpec,
    #[serde(default)]
    pub own_default: DefaultSpec,
    #[serde(default)]
    pub funding: FundingSpec,
    #[serde(default)]
    pub measures: Vec<XvaMeasure>,
    #[serde(default = "default_confidence")]
    pub confidence_level: f64,
    #[serde(default)]
    pub q_exposure: Option<ExposureCubeInput>,
    #[serde(default)]
    pub p_exposure: Option<ExposureCubeInput>,
    #[serde(default)]
    pub materialize_exposure: bool,
    #[serde(default)]
    pub memory_budget_bytes: Option<u64>,
    #[serde(default)]
    pub pricing: PricingContext,
}

fn default_confidence() -> f64 {
    0.95
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct XvaProvenance {
    pub context_hash: Option<ContextHash>,
    pub operation_id: String,
    pub q_measure: bool,
    pub p_measure: bool,
    pub shared_exposure_graph: bool,
    pub approximations: Vec<String>,
    pub artifact_lineage: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct XvaResult {
    pub operation_id: String,
    pub context_hash: Option<ContextHash>,
    pub times: Vec<f64>,
    pub ee: Vec<f64>,
    pub pfe: Vec<f64>,
    pub cva: f64,
    pub dva: f64,
    pub fva: f64,
    pub mva: f64,
    pub kva: f64,
    #[serde(default)]
    pub measures: BTreeMap<String, f64>,
    #[serde(default)]
    pub exposure_cube: Option<ExposureCubeRef>,
    #[serde(default)]
    pub artifacts: Vec<ExposureCubeRef>,
    pub provenance: XvaProvenance,
}
