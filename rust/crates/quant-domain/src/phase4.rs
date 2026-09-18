//! Portfolio and scenario domain primitives (REST API phase 4).
//!
//! These are declarative, serializable values.  They deliberately contain IDs instead of
//! handles or provider objects, so a complete context can be replayed by another replica.

use crate::{DomainError, ProductId, ProductSpec, QuantContext, Value};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

macro_rules! id_type {
    ($name:ident) => {
        #[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
        #[serde(transparent)]
        pub struct $name(pub String);
        impl From<&str> for $name {
            fn from(value: &str) -> Self {
                Self(value.to_owned())
            }
        }
        impl From<String> for $name {
            fn from(value: String) -> Self {
                Self(value)
            }
        }
        impl AsRef<str> for $name {
            fn as_ref(&self) -> &str {
                &self.0
            }
        }
        impl std::fmt::Display for $name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str(&self.0)
            }
        }
    };
}

id_type!(PortfolioId);
id_type!(TradeId);
id_type!(ScenarioSetId);
id_type!(ScenarioId);
id_type!(NettingSetId);
id_type!(CollateralAgreementId);

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Trade {
    pub trade_id: TradeId,
    pub product_id: ProductId,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub product: Option<ProductSpec>,
    #[serde(default = "one")]
    pub quantity: f64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub netting_set_id: Option<NettingSetId>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub collateral_agreement_id: Option<CollateralAgreementId>,
    #[serde(default)]
    pub metadata: BTreeMap<String, Value>,
}

fn one() -> f64 {
    1.0
}

impl Trade {
    pub fn new(trade_id: impl Into<TradeId>, product_id: impl Into<ProductId>) -> Self {
        Self {
            trade_id: trade_id.into(),
            product_id: product_id.into(),
            product: None,
            quantity: 1.0,
            netting_set_id: None,
            collateral_agreement_id: None,
            metadata: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Portfolio {
    pub id: PortfolioId,
    pub trades: Vec<Trade>,
    #[serde(default)]
    pub metadata: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PortfolioSpec {
    pub trades: Vec<Trade>,
    #[serde(default)]
    pub metadata: BTreeMap<String, Value>,
}
impl PortfolioSpec {
    pub fn new(trades: Vec<Trade>) -> Self {
        Self {
            trades,
            metadata: BTreeMap::new(),
        }
    }
    pub fn to_value(&self) -> Value {
        serde_json::to_value(self).expect("portfolio spec is serializable")
    }
}

impl Portfolio {
    pub fn new(id: impl Into<PortfolioId>, trades: Vec<Trade>) -> Self {
        Self {
            id: id.into(),
            trades,
            metadata: BTreeMap::new(),
        }
    }
    pub fn is_empty(&self) -> bool {
        self.trades.is_empty()
    }
    pub fn len(&self) -> usize {
        self.trades.len()
    }
    pub fn to_value(&self) -> Value {
        serde_json::to_value(PortfolioSpec {
            trades: self.trades.clone(),
            metadata: self.metadata.clone(),
        })
        .expect("portfolio is serializable")
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Scenario {
    pub id: ScenarioId,
    #[serde(default)]
    pub shocks: BTreeMap<String, f64>,
    #[serde(default)]
    pub metadata: BTreeMap<String, Value>,
}

impl Scenario {
    pub fn new(id: impl Into<ScenarioId>, shocks: BTreeMap<String, f64>) -> Self {
        Self {
            id: id.into(),
            shocks,
            metadata: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ScenarioSet {
    pub id: ScenarioSetId,
    pub scenarios: Vec<Scenario>,
    /// Maximum number of scenarios retained by a single planner chunk. Zero means engine default.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chunk_size: Option<usize>,
    #[serde(default)]
    pub metadata: BTreeMap<String, Value>,
}

impl ScenarioSet {
    pub fn new(id: impl Into<ScenarioSetId>, scenarios: Vec<Scenario>) -> Self {
        Self {
            id: id.into(),
            scenarios,
            chunk_size: None,
            metadata: BTreeMap::new(),
        }
    }
    pub fn len(&self) -> usize {
        self.scenarios.len()
    }
    pub fn is_empty(&self) -> bool {
        self.scenarios.is_empty()
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct RunRecord {
    #[serde(default)]
    pub context_hash: Option<String>,
    #[serde(default)]
    pub operation: String,
    #[serde(default)]
    pub result_hash: Option<String>,
    #[serde(default)]
    pub seed: Option<u64>,
    #[serde(default)]
    pub metrics: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PartialFailure {
    pub item_id: String,
    pub error: String,
}

impl QuantContext {
    pub fn add_portfolio(&self, portfolio: Portfolio) -> Result<Self, DomainError> {
        self.apply(&[crate::ContextCommand::AddPortfolioTyped {
            id: portfolio.id.clone(),
            spec: PortfolioSpec {
                trades: portfolio.trades,
                metadata: portfolio.metadata,
            },
        }])
        .map(|(context, _)| context)
    }

    pub fn portfolio(&self, id: impl AsRef<str>) -> Result<Portfolio, DomainError> {
        let id = id.as_ref();
        let value = self
            .portfolios
            .get(id)
            .ok_or_else(|| DomainError::ResourceNotFound {
                kind: "portfolio".into(),
                id: id.into(),
            })?;
        let spec: PortfolioSpec = serde_json::from_value(value.clone())
            .map_err(|error| DomainError::InvalidCommand(error.to_string()))?;
        let mut portfolio = Portfolio {
            id: id.into(),
            trades: spec.trades,
            metadata: spec.metadata,
        };
        for trade in &mut portfolio.trades {
            if trade.product.is_none() {
                if let Some(product) = self.products.get(&trade.product_id.0) {
                    trade.product = Some(
                        ProductSpec::from_value(product.clone())
                            .map_err(DomainError::InvalidCommand)?,
                    );
                }
            }
        }
        Ok(portfolio)
    }

    pub fn record_run(
        &self,
        operation_id: impl Into<String>,
        record: RunRecord,
    ) -> Result<Self, DomainError> {
        self.apply(&[crate::ContextCommand::RecordRun {
            operation_id: operation_id.into(),
            record,
        }])
        .map(|(context, _)| context)
    }
}
