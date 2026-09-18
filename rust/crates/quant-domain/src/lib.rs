//! Tipos de dominio de la primera vertical REST.
//!
//! Este crate no conoce HTTP, Tokio ni el proveedor de cálculo. `QuantContext` es un
//! documento completo que el cliente conserva y vuelve a enviar; el servidor no tiene
//! que buscar una sesión por `context_id`.

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use thiserror::Error;

mod phase2;
mod phase4;
mod risk;
pub mod xva;
pub use phase2::{
    ContextMutation, CpuVectorPolicy, DevicePreference, ExecutionPolicy, MarketHandle, MarketId,
    MarketSnapshot, MarketSpec, MeasureKind, ModelEntry, ModelHandle, ModelId, ModelKind,
    ModelSpec, PricingContext, ProductEntry, ProductHandle, ProductId, ProductKind, ProductSpec,
    Snapshot,
};
pub use phase4::{
    CollateralAgreementId, NettingSetId, PartialFailure, Portfolio, PortfolioId, PortfolioSpec,
    RunRecord, Scenario, ScenarioId, ScenarioSet, ScenarioSetId, Trade, TradeId,
};
pub use risk::{
    BumpScheme, RiskArtifact, RiskFactor, RiskMeasure, RiskProvenance, RiskRequest, RiskResult,
};
pub use xva::{
    CollateralAgreement, DefaultSpec, ExposureCubeInput, ExposureCubeRef, FundingSpec, NettingSet,
    ProbabilityMeasure, XvaMeasure, XvaProvenance, XvaRequest, XvaResult,
};

pub const CONTEXT_SCHEMA: &str = "quant.context/v1";

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ContextHash(pub String);

impl ContextHash {
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResourceRef {
    pub id: String,
    pub hash: String,
    pub kind: String,
    pub version: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum ContextCommand {
    AddMarket {
        id: String,
        #[serde(default)]
        spec: Value,
    },
    AddMarketTyped {
        id: String,
        spec: MarketSpec,
    },
    AddModel {
        id: String,
        #[serde(default)]
        spec: Value,
    },
    AddModelTyped {
        id: String,
        spec: ModelSpec,
    },
    AddProduct {
        id: String,
        #[serde(default)]
        spec: Value,
    },
    AddProductTyped {
        id: String,
        spec: ProductSpec,
    },
    AddPortfolio {
        id: String,
        #[serde(default)]
        spec: Value,
    },
    AddPortfolioTyped {
        id: PortfolioId,
        spec: PortfolioSpec,
    },
    RecordRun {
        operation_id: String,
        #[serde(default)]
        record: RunRecord,
    },
    Remove {
        resource_kind: String,
        id: String,
    },
}

impl ContextCommand {
    pub fn resource_ref(&self, context: &QuantContext) -> Option<ResourceRef> {
        let (kind, id, spec) = match self {
            Self::AddMarket { id, spec } => ("market", id, spec.clone()),
            Self::AddModel { id, spec } => ("model", id, spec.clone()),
            Self::AddProduct { id, spec } => ("product", id, spec.clone()),
            Self::AddMarketTyped { id, spec } => ("market", id, spec.to_value()),
            Self::AddModelTyped { id, spec } => ("model", id, spec.to_value()),
            Self::AddProductTyped { id, spec } => ("product", id, spec.to_value()),
            Self::AddPortfolio { id, spec } => ("portfolio", id, spec.clone()),
            Self::AddPortfolioTyped { id, spec } => ("portfolio", &id.0, spec.to_value()),
            Self::RecordRun { .. } => return None,
            Self::Remove { .. } => return None,
        };
        Some(ResourceRef {
            id: id.clone(),
            hash: hash_value(&spec).0,
            kind: kind.to_string(),
            version: context.revision.saturating_add(1),
        })
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct QuantContext {
    #[serde(rename = "schema")]
    pub schema: String,
    pub context_id: String,
    pub revision: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent_hash: Option<ContextHash>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub context_hash: Option<ContextHash>,
    #[serde(default)]
    pub engine: BTreeMap<String, Value>,
    #[serde(default)]
    pub markets: BTreeMap<String, Value>,
    #[serde(default)]
    pub models: BTreeMap<String, Value>,
    #[serde(default)]
    pub products: BTreeMap<String, Value>,
    #[serde(default)]
    pub portfolios: BTreeMap<String, Value>,
    #[serde(default)]
    pub pricing: BTreeMap<String, Value>,
    #[serde(default)]
    pub runs: Vec<Value>,
    /// Logical branch name. It is part of the canonical hash, so two branches can never
    /// be silently mixed by a stateless replica.
    #[serde(default = "default_branch_id")]
    pub branch_id: String,
    /// Hashes of the snapshots from which this context was derived.
    #[serde(default)]
    pub lineage: Vec<ContextHash>,
}

fn default_branch_id() -> String {
    "main".to_string()
}

// The public OpenAPI name is an envelope containing the complete snapshot. Keeping the
// alias avoids two subtly different context representations in the transport and engine.
pub type ContextEnvelope = QuantContext;

impl Default for QuantContext {
    fn default() -> Self {
        Self {
            schema: CONTEXT_SCHEMA.to_string(),
            context_id: "default".to_string(),
            revision: 0,
            parent_hash: None,
            context_hash: None,
            engine: BTreeMap::new(),
            markets: BTreeMap::new(),
            models: BTreeMap::new(),
            products: BTreeMap::new(),
            portfolios: BTreeMap::new(),
            pricing: BTreeMap::new(),
            runs: Vec::new(),
            branch_id: default_branch_id(),
            lineage: Vec::new(),
        }
    }
}

impl QuantContext {
    pub fn new(context_id: impl Into<String>) -> Self {
        Self {
            context_id: context_id.into(),
            ..Self::default()
        }
        .with_hash()
    }

    /// Hash the complete snapshot without including the hash field itself.
    pub fn computed_hash(&self) -> ContextHash {
        let mut copy = self.clone();
        copy.context_hash = None;
        hash_value(&serde_json::to_value(copy).expect("QuantContext is serializable"))
    }

    pub fn with_hash(mut self) -> Self {
        self.context_hash = Some(self.computed_hash());
        self
    }

    pub fn verify_hash(&self) -> bool {
        self.context_hash.as_ref() == Some(&self.computed_hash())
    }

    pub fn apply(
        &self,
        commands: &[ContextCommand],
    ) -> Result<(Self, Option<ResourceRef>), DomainError> {
        if self.schema != CONTEXT_SCHEMA {
            return Err(DomainError::InvalidSchema(self.schema.clone()));
        }
        if self.context_hash.is_some() && !self.verify_hash() {
            return Err(DomainError::ContextHashMismatch);
        }
        let mut next = self.clone();
        let mut last_ref = None;
        for command in commands {
            match command {
                ContextCommand::AddMarket { id, spec } => {
                    next.markets.insert(id.clone(), spec.clone());
                }
                ContextCommand::AddMarketTyped { id, spec } => {
                    next.markets.insert(id.clone(), spec.to_value());
                }
                ContextCommand::AddModel { id, spec } => {
                    next.models.insert(id.clone(), spec.clone());
                }
                ContextCommand::AddModelTyped { id, spec } => {
                    next.models.insert(id.clone(), spec.to_value());
                }
                ContextCommand::AddProduct { id, spec } => {
                    next.products.insert(id.clone(), spec.clone());
                }
                ContextCommand::AddProductTyped { id, spec } => {
                    next.products.insert(id.clone(), spec.to_value());
                }
                ContextCommand::AddPortfolio { id, spec } => {
                    next.portfolios.insert(id.clone(), spec.clone());
                }
                ContextCommand::AddPortfolioTyped { id, spec } => {
                    next.portfolios.insert(id.0.clone(), spec.to_value());
                }
                ContextCommand::RecordRun {
                    operation_id,
                    record,
                } => {
                    if operation_id.trim().is_empty() {
                        return Err(DomainError::InvalidCommand(
                            "record_run operation_id is required".into(),
                        ));
                    }
                    let mut value = serde_json::to_value(record)
                        .map_err(|error| DomainError::InvalidCommand(error.to_string()))?;
                    if let Value::Object(ref mut object) = value {
                        object.insert("operation_id".into(), Value::String(operation_id.clone()));
                    }
                    next.runs.push(value);
                }
                ContextCommand::Remove { resource_kind, id } => {
                    let removed = match resource_kind.as_str() {
                        "market" => next.markets.remove(id),
                        "model" => next.models.remove(id),
                        "product" => next.products.remove(id),
                        "portfolio" => next.portfolios.remove(id),
                        _ => {
                            return Err(DomainError::InvalidCommand(format!(
                                "unknown resource kind: {resource_kind}"
                            )))
                        }
                    };
                    if removed.is_none() {
                        return Err(DomainError::ResourceNotFound {
                            kind: resource_kind.clone(),
                            id: id.clone(),
                        });
                    }
                }
            }
            last_ref = command.resource_ref(&next);
        }
        next.parent_hash = self.context_hash.clone();
        if let Some(parent) = &self.context_hash {
            next.lineage.push(parent.clone());
        }
        next.revision = self
            .revision
            .checked_add(1)
            .ok_or(DomainError::RevisionOverflow)?;
        next.context_hash = None;
        next.context_hash = Some(next.computed_hash());
        if let Some(resource) = &mut last_ref {
            resource.version = next.revision;
        }
        Ok((next, last_ref))
    }
}

pub fn hash_value(value: &Value) -> ContextHash {
    let bytes = serde_json::to_vec(value).expect("JSON values are serializable");
    let mut digest = Sha256::new();
    digest.update(bytes);
    ContextHash(format!("sha256:{:x}", digest.finalize()))
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum DomainError {
    #[error("invalid context schema: {0}")]
    InvalidSchema(String),
    #[error("context hash does not match its snapshot")]
    ContextHashMismatch,
    #[error("invalid context command: {0}")]
    InvalidCommand(String),
    #[error("resource not found: {kind}/{id}")]
    ResourceNotFound { kind: String, id: String },
    #[error("context revision overflow")]
    RevisionOverflow,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MeasureSpec {
    pub name: String,
    #[serde(default)]
    pub params: Map<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PricingInput {
    pub context: QuantContext,
    pub operation_id: String,
    #[serde(default)]
    pub market: Option<ResourceRef>,
    #[serde(default)]
    pub model: Option<ResourceRef>,
    #[serde(default)]
    pub products: Vec<ResourceRef>,
    pub measures: Vec<MeasureSpec>,
    #[serde(default)]
    pub pricing: BTreeMap<String, Value>,
    #[serde(default)]
    pub execution: BTreeMap<String, Value>,
    #[serde(default)]
    pub output: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PricingOutput {
    pub context: QuantContext,
    pub result: BTreeMap<String, Value>,
    pub provenance: BTreeMap<String, Value>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn context_hash_is_replayable_and_revisioned() {
        let c = QuantContext::new("client-a");
        let (next, _) = c
            .apply(&[ContextCommand::AddMarket {
                id: "m".into(),
                spec: serde_json::json!({"r0":0.02}),
            }])
            .unwrap();
        assert!(next.verify_hash());
        let (replay, _) = c
            .apply(&[ContextCommand::AddMarket {
                id: "m".into(),
                spec: serde_json::json!({"r0":0.02}),
            }])
            .unwrap();
        assert_eq!(next, replay);
        assert_eq!(next.revision, 1);
    }

    #[test]
    fn portfolio_and_run_record_round_trip_without_server_state() {
        let product = ProductSpec::from_value(serde_json::json!({
            "payment_times": [1.0], "accruals": [1.0]
        }))
        .unwrap();
        let mut trade = Trade::new("t-1", "p-1");
        trade.netting_set_id = Some(NettingSetId::from("net-1"));
        trade.collateral_agreement_id = Some(CollateralAgreementId::from("csa-1"));
        let context = QuantContext::new("client")
            .add_product("p-1", product)
            .unwrap()
            .add_portfolio(Portfolio::new("book", vec![trade]))
            .unwrap()
            .record_run(
                "op-1",
                RunRecord {
                    operation: "portfolio_price".into(),
                    ..Default::default()
                },
            )
            .unwrap();
        let portfolio = context.portfolio("book").unwrap();
        assert_eq!(
            portfolio.trades[0].netting_set_id.as_ref().unwrap().0,
            "net-1"
        );
        assert_eq!(context.runs[0]["operation_id"], "op-1");
        assert!(context.verify_hash());
    }
}
