//! Risk/Greeks contracts for the phase-5 API.
//!
//! The types in this module are deliberately declarative.  They carry IDs, factors and
//! numerical policy, but never a provider handle or an autodiff tape.

use crate::{ContextHash, PricingContext, QuantContext, ResourceRef};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RiskMeasure {
    #[serde(alias = "PV", alias = "present_value")]
    Pv,
    #[serde(alias = "PV01", alias = "dv01", alias = "DV01")]
    Pv01,
    #[serde(alias = "DELTA")]
    Delta,
    #[serde(alias = "RHO")]
    Rho,
    #[serde(alias = "VEGA")]
    Vega,
    #[serde(alias = "ALL_GREEKS")]
    AllGreeks,
}

impl RiskMeasure {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Pv => "pv",
            Self::Pv01 => "pv01",
            Self::Delta => "delta",
            Self::Rho => "rho",
            Self::Vega => "vega",
            Self::AllGreeks => "all_greeks",
        }
    }
}

impl std::fmt::Display for RiskMeasure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum RiskFactor {
    ModelParameter {
        name: String,
    },
    CurveParallel {
        curve: String,
    },
    CurveBucket {
        curve: String,
        bucket: usize,
    },
    /// Alias used by curve-oriented clients; it has the same bump semantics as `CurveBucket`.
    CurvePillar {
        curve: String,
        pillar: usize,
    },
    CreditParameter {
        name: String,
    },
    TimeShift {
        name: String,
    },
}

impl RiskFactor {
    pub fn model_parameter(name: impl Into<String>) -> Self {
        Self::ModelParameter { name: name.into() }
    }
    pub fn curve_parallel(curve: impl Into<String>) -> Self {
        Self::CurveParallel {
            curve: curve.into(),
        }
    }
    pub fn curve_bucket(curve: impl Into<String>, bucket: usize) -> Self {
        Self::CurveBucket {
            curve: curve.into(),
            bucket,
        }
    }
    pub fn curve_pillar(curve: impl Into<String>, pillar: usize) -> Self {
        Self::CurvePillar {
            curve: curve.into(),
            pillar,
        }
    }
    pub fn key(&self) -> String {
        match self {
            Self::ModelParameter { name } => format!("model.{name}"),
            Self::CurveParallel { curve } => format!("curve.{curve}.parallel"),
            Self::CurveBucket { curve, bucket } => format!("curve.{curve}.bucket[{bucket}]"),
            Self::CurvePillar { curve, pillar } => format!("curve.{curve}.bucket[{pillar}]"),
            Self::CreditParameter { name } => format!("credit.{name}"),
            Self::TimeShift { name } => format!("time.{name}"),
        }
    }
}

impl std::fmt::Display for RiskFactor {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.key())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum BumpScheme {
    #[default]
    Central,
    OneSided,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RiskRequest {
    pub context: QuantContext,
    pub operation_id: String,
    pub portfolio_id: String,
    pub market: ResourceRef,
    pub model: ResourceRef,
    #[serde(default)]
    pub measures: Vec<RiskMeasure>,
    #[serde(default)]
    pub factors: Vec<RiskFactor>,
    #[serde(default)]
    pub bump_scheme: BumpScheme,
    /// Absolute bump in model/curve units.  If absent, the engine chooses 1bp.
    #[serde(default)]
    pub bump_size: Option<f64>,
    /// Optional absolute tolerance by factor key, e.g. `model.r0`.
    #[serde(default)]
    pub tolerances: BTreeMap<String, f64>,
    #[serde(default)]
    pub pricing: PricingContext,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RiskProvenance {
    pub method: String,
    pub bump_scheme: BumpScheme,
    pub bump_size: f64,
    pub tolerance_abs: f64,
    pub tolerance_rel: f64,
    pub context_hash: Option<ContextHash>,
    pub market_version: u64,
    pub model_version: u64,
    pub base_reused: bool,
    pub bump_reused: bool,
    pub aad_passes: usize,
    pub valuation_calls: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RiskArtifact {
    pub key: String,
    pub kind: String,
    pub dependencies: Vec<String>,
    pub reused: bool,
}

/// Dense row-major values.  `values[row * factor_index.len() + column]` is the value for
/// one measure/factor pair; `trade_ids` identifies the portfolio aggregation source.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RiskResult {
    pub operation_id: String,
    pub context_hash: Option<ContextHash>,
    pub base_value: f64,
    pub base_values: Vec<f64>,
    pub trade_ids: Vec<String>,
    pub measure_index: Vec<String>,
    pub factor_index: Vec<String>,
    pub shape: [usize; 2],
    pub values: Vec<f64>,
    pub provenance: BTreeMap<String, RiskProvenance>,
    pub artifacts: Vec<RiskArtifact>,
}
