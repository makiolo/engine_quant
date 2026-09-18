//! Typed, immutable building blocks used by the phase-2 planner.
//!
//! The REST representation remains JSON (`Value`) for backwards compatibility.  These
//! types are the in-process representation: snapshots are immutable and handles only
//! carry an `Arc`, so a plan never copies a large market surface.

use crate::{hash_value, ContextHash, DomainError, QuantContext};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::{collections::BTreeMap, fmt, sync::Arc};

macro_rules! id_type {
    ($name:ident) => {
        #[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
        #[serde(transparent)]
        pub struct $name(pub String);
        impl From<&str> for $name {
            fn from(v: &str) -> Self {
                Self(v.to_owned())
            }
        }
        impl From<String> for $name {
            fn from(v: String) -> Self {
                Self(v)
            }
        }
        impl AsRef<str> for $name {
            fn as_ref(&self) -> &str {
                &self.0
            }
        }
        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(&self.0)
            }
        }
    };
}

id_type!(MarketId);
id_type!(ModelId);
id_type!(ProductId);

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ModelKind(pub String);
impl From<&str> for ModelKind {
    fn from(v: &str) -> Self {
        Self(v.to_owned())
    }
}
impl From<String> for ModelKind {
    fn from(v: String) -> Self {
        Self(v)
    }
}
impl fmt::Display for ModelKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ProductKind(pub String);
impl From<&str> for ProductKind {
    fn from(v: &str) -> Self {
        Self(v.to_owned())
    }
}
impl From<String> for ProductKind {
    fn from(v: String) -> Self {
        Self(v)
    }
}
impl fmt::Display for ProductKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(transparent)]
pub struct MeasureKind(pub String);
impl From<&str> for MeasureKind {
    fn from(v: &str) -> Self {
        Self(v.to_owned())
    }
}

/// A market definition is deliberately an open map. Providers may add curves and
/// surfaces without forcing a domain-crate release.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct MarketSpec {
    #[serde(default)]
    pub kind: String,
    #[serde(flatten)]
    pub params: BTreeMap<String, Value>,
}
impl MarketSpec {
    pub fn new(value: Value) -> Result<Self, String> {
        let mut object = value
            .as_object()
            .cloned()
            .ok_or("market spec must be an object")?;
        let kind = object
            .remove("kind")
            .and_then(|v| v.as_str().map(str::to_owned))
            .unwrap_or_default();
        Ok(Self {
            kind,
            params: object.into_iter().collect(),
        })
    }
    pub fn hash(&self) -> ContextHash {
        hash_value(&serde_json::to_value(self).expect("serializable"))
    }
    pub fn to_value(&self) -> Value {
        serde_json::to_value(self).expect("serializable")
    }
}
impl From<MarketSpec> for Value {
    fn from(v: MarketSpec) -> Self {
        v.to_value()
    }
}
impl TryFrom<Value> for MarketSpec {
    type Error = String;
    fn try_from(v: Value) -> Result<Self, Self::Error> {
        Self::new(v)
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MarketSnapshot {
    pub id: MarketId,
    pub spec: MarketSpec,
    pub version: u64,
    pub hash: ContextHash,
    #[serde(default)]
    pub parent_hash: Option<ContextHash>,
}
pub type Snapshot = MarketSnapshot;
impl MarketSnapshot {
    pub fn new(
        id: impl Into<MarketId>,
        spec: MarketSpec,
        version: u64,
        parent_hash: Option<ContextHash>,
    ) -> Self {
        let id = id.into();
        let hash = hash_value(
            &serde_json::json!({"id": id, "spec": spec, "version": version, "parent_hash": parent_hash}),
        );
        Self {
            id,
            spec,
            version,
            hash,
            parent_hash,
        }
    }
}
#[derive(Debug, Clone)]
pub struct MarketHandle(pub Arc<MarketSnapshot>);
impl MarketHandle {
    pub fn new(snapshot: MarketSnapshot) -> Self {
        Self(Arc::new(snapshot))
    }
    pub fn snapshot(&self) -> &MarketSnapshot {
        &self.0
    }
    pub fn id(&self) -> &MarketId {
        &self.0.id
    }
    pub fn hash(&self) -> &ContextHash {
        &self.0.hash
    }
    pub fn version(&self) -> u64 {
        self.0.version
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct HullWhite1FSpec {
    #[serde(flatten)]
    pub params: BTreeMap<String, Value>,
}
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct HestonSpec {
    #[serde(flatten)]
    pub params: BTreeMap<String, Value>,
}
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SabrSpec {
    #[serde(flatten)]
    pub params: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", content = "params", rename_all = "snake_case")]
pub enum ModelSpec {
    #[serde(rename = "hull_white_1f")]
    HullWhite1F(HullWhite1FSpec),
    Heston(HestonSpec),
    Sabr(SabrSpec),
    Json {
        name: String,
        params: BTreeMap<String, Value>,
    },
    External {
        provider: String,
        name: String,
        payload: Vec<u8>,
    },
}
impl ModelSpec {
    pub fn hull_white(params: BTreeMap<String, Value>) -> Self {
        Self::HullWhite1F(HullWhite1FSpec { params })
    }
    pub fn from_value(value: Value) -> Result<Self, String> {
        let object = value
            .as_object()
            .cloned()
            .ok_or("model spec must be an object")?;
        let inferred = if object.contains_key("a") && object.contains_key("sigma") {
            "hull_white_1f"
        } else {
            "json"
        };
        let name = object
            .get("kind")
            .or_else(|| object.get("model"))
            .and_then(Value::as_str)
            .unwrap_or(inferred)
            .to_owned();
        let params = object
            .into_iter()
            .filter(|(k, _)| k != "kind" && k != "model")
            .collect();
        Ok(match name.to_ascii_lowercase().as_str() {
            "hull_white_1f" | "hull_white" => Self::hull_white(params),
            _ => Self::Json { name, params },
        })
    }
    pub fn kind(&self) -> ModelKind {
        ModelKind(
            match self {
                Self::HullWhite1F(_) => "hull_white_1f",
                Self::Heston(_) => "heston",
                Self::Sabr(_) => "sabr",
                Self::Json { name, .. } => name,
                Self::External { name, .. } => name,
            }
            .to_owned(),
        )
    }
    pub fn to_value(&self) -> Value {
        serde_json::to_value(self).expect("serializable")
    }
    pub fn hash(&self) -> ContextHash {
        hash_value(&self.to_value())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct IrSwapSpec {
    #[serde(flatten)]
    pub params: BTreeMap<String, Value>,
}
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EuropeanOptionSpec {
    #[serde(flatten)]
    pub params: BTreeMap<String, Value>,
}
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", content = "params", rename_all = "snake_case")]
pub enum ProductSpec {
    #[serde(rename = "ir_swap")]
    IrSwap(IrSwapSpec),
    EuropeanOption(EuropeanOptionSpec),
    Json {
        name: String,
        params: BTreeMap<String, Value>,
    },
    External {
        provider: String,
        name: String,
        payload: Vec<u8>,
    },
}
impl ProductSpec {
    pub fn from_value(value: Value) -> Result<Self, String> {
        let object = value
            .as_object()
            .cloned()
            .ok_or("product spec must be an object")?;
        let inferred = if object.contains_key("payment_times") {
            "ir_swap"
        } else {
            "json"
        };
        let name = object
            .get("kind")
            .or_else(|| object.get("product"))
            .and_then(Value::as_str)
            .unwrap_or(inferred)
            .to_owned();
        let params = object
            .into_iter()
            .filter(|(k, _)| k != "kind" && k != "product")
            .collect();
        Ok(match name.to_ascii_lowercase().as_str() {
            "ir_swap" | "irs" => Self::IrSwap(IrSwapSpec { params }),
            _ => Self::Json { name, params },
        })
    }
    pub fn kind(&self) -> ProductKind {
        ProductKind(
            match self {
                Self::IrSwap(_) => "ir_swap",
                Self::EuropeanOption(_) => "european_option",
                Self::Json { name, .. } => name,
                Self::External { name, .. } => name,
            }
            .to_owned(),
        )
    }
    pub fn to_value(&self) -> Value {
        serde_json::to_value(self).expect("serializable")
    }
    pub fn hash(&self) -> ContextHash {
        hash_value(&self.to_value())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelEntry {
    pub id: ModelId,
    pub spec: ModelSpec,
    pub version: u64,
    pub hash: ContextHash,
    #[serde(default)]
    pub parent_hash: Option<ContextHash>,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProductEntry {
    pub id: ProductId,
    pub spec: ProductSpec,
    pub version: u64,
    pub hash: ContextHash,
    #[serde(default)]
    pub parent_hash: Option<ContextHash>,
}
#[derive(Debug, Clone)]
pub struct ModelHandle(pub Arc<ModelEntry>);
#[derive(Debug, Clone)]
pub struct ProductHandle(pub Arc<ProductEntry>);
impl ModelHandle {
    pub fn new(entry: ModelEntry) -> Self {
        Self(Arc::new(entry))
    }
    pub fn entry(&self) -> &ModelEntry {
        &self.0
    }
    pub fn id(&self) -> &ModelId {
        &self.0.id
    }
    pub fn hash(&self) -> &ContextHash {
        &self.0.hash
    }
}
impl ModelHandle {
    pub fn version(&self) -> u64 {
        self.0.version
    }
}
impl ProductHandle {
    pub fn new(entry: ProductEntry) -> Self {
        Self(Arc::new(entry))
    }
    pub fn entry(&self) -> &ProductEntry {
        &self.0
    }
    pub fn id(&self) -> &ProductId {
        &self.0.id
    }
    pub fn hash(&self) -> &ContextHash {
        &self.0.hash
    }
    pub fn version(&self) -> u64 {
        self.0.version
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum DevicePreference {
    #[default]
    Auto,
    Cpu,
    Gpu {
        device: Option<u32>,
    },
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum CpuVectorPolicy {
    #[default]
    Auto,
    /// Bit-for-bit left-to-right reference reduction.
    Scalar,
    /// Reserved until stable portable SIMD is available on the supported toolchains.
    PortableSimd,
    /// Explicit x86 AVX2 strategy; the application must fall back when unavailable.
    Avx2,
    /// Reserved for a future AVX-512 kernel; never assume it is available.
    Avx512,
}
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionPolicy {
    #[serde(default)]
    pub device: DevicePreference,
    #[serde(default)]
    pub cpu_vector: CpuVectorPolicy,
    #[serde(default)]
    pub max_threads: Option<usize>,
    #[serde(default = "default_true")]
    pub deterministic: bool,
    #[serde(default)]
    pub deadline_ms: Option<u64>,
    #[serde(default)]
    pub memory_budget_bytes: u64,
}
fn default_true() -> bool {
    true
}
impl Default for ExecutionPolicy {
    fn default() -> Self {
        Self {
            device: DevicePreference::Auto,
            cpu_vector: CpuVectorPolicy::Auto,
            max_threads: None,
            deterministic: true,
            deadline_ms: None,
            memory_budget_bytes: 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct PricingContext {
    #[serde(default)]
    pub pricing_date: Option<String>,
    #[serde(default)]
    pub n_paths: u64,
    #[serde(default)]
    pub n_steps: u64,
    #[serde(default)]
    pub seed: u64,
    #[serde(default)]
    pub measure: Option<String>,
    #[serde(default)]
    pub execution: ExecutionPolicy,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum ContextMutation {
    AddMarket { id: MarketId, spec: MarketSpec },
    AddModel { id: ModelId, spec: ModelSpec },
    AddProduct { id: ProductId, spec: ProductSpec },
    Fork { branch_id: String },
}
impl QuantContext {
    pub fn fork(&self, branch_id: impl Into<String>) -> Result<Self, DomainError> {
        let mut next = self.clone();
        next.branch_id = branch_id.into();
        next.parent_hash = self.context_hash.clone();
        next.revision = 0;
        next.context_hash = None;
        Ok(next.with_hash())
    }
    pub fn apply_typed(&self, mutations: &[ContextMutation]) -> Result<Self, DomainError> {
        let mut commands = Vec::with_capacity(mutations.len());
        for mutation in mutations {
            match mutation {
                ContextMutation::AddMarket { id, spec } => {
                    commands.push(crate::ContextCommand::AddMarket {
                        id: id.0.clone(),
                        spec: spec.to_value(),
                    })
                }
                ContextMutation::AddModel { id, spec } => {
                    commands.push(crate::ContextCommand::AddModel {
                        id: id.0.clone(),
                        spec: spec.to_value(),
                    })
                }
                ContextMutation::AddProduct { id, spec } => {
                    commands.push(crate::ContextCommand::AddProduct {
                        id: id.0.clone(),
                        spec: spec.to_value(),
                    })
                }
                ContextMutation::Fork { branch_id } => {
                    let mut fork = self.fork(branch_id.clone())?;
                    fork.context_hash = Some(fork.computed_hash());
                    return Ok(fork);
                }
            }
        }
        self.apply(&commands).map(|(next, _)| next)
    }
    pub fn add_market(
        &self,
        id: impl Into<MarketId>,
        spec: MarketSpec,
    ) -> Result<Self, DomainError> {
        self.apply_typed(&[ContextMutation::AddMarket {
            id: id.into(),
            spec,
        }])
    }
    pub fn add_model(&self, id: impl Into<ModelId>, spec: ModelSpec) -> Result<Self, DomainError> {
        self.apply_typed(&[ContextMutation::AddModel {
            id: id.into(),
            spec,
        }])
    }
    pub fn add_product(
        &self,
        id: impl Into<ProductId>,
        spec: ProductSpec,
    ) -> Result<Self, DomainError> {
        self.apply_typed(&[ContextMutation::AddProduct {
            id: id.into(),
            spec,
        }])
    }

    pub fn market_handle(&self, id: impl AsRef<str>) -> Result<MarketHandle, DomainError> {
        let id = id.as_ref();
        let value = self
            .markets
            .get(id)
            .ok_or_else(|| DomainError::ResourceNotFound {
                kind: "market".into(),
                id: id.into(),
            })?;
        let spec = MarketSpec::try_from(value.clone()).map_err(DomainError::InvalidCommand)?;
        Ok(MarketHandle::new(MarketSnapshot::new(
            id,
            spec,
            self.revision,
            self.parent_hash.clone(),
        )))
    }
    pub fn model_handle(&self, id: impl AsRef<str>) -> Result<ModelHandle, DomainError> {
        let id = id.as_ref();
        let value = self
            .models
            .get(id)
            .ok_or_else(|| DomainError::ResourceNotFound {
                kind: "model".into(),
                id: id.into(),
            })?;
        let spec = ModelSpec::from_value(value.clone()).map_err(DomainError::InvalidCommand)?;
        Ok(ModelHandle::new(ModelEntry {
            id: id.into(),
            hash: spec.hash(),
            spec,
            version: self.revision,
            parent_hash: self.parent_hash.clone(),
        }))
    }
    pub fn product_handle(&self, id: impl AsRef<str>) -> Result<ProductHandle, DomainError> {
        let id = id.as_ref();
        let value = self
            .products
            .get(id)
            .ok_or_else(|| DomainError::ResourceNotFound {
                kind: "product".into(),
                id: id.into(),
            })?;
        let spec = ProductSpec::from_value(value.clone()).map_err(DomainError::InvalidCommand)?;
        Ok(ProductHandle::new(ProductEntry {
            id: id.into(),
            hash: spec.hash(),
            spec,
            version: self.revision,
            parent_hash: self.parent_hash.clone(),
        }))
    }
}

impl From<&Map<String, Value>> for MarketSpec {
    fn from(v: &Map<String, Value>) -> Self {
        Self {
            kind: String::new(),
            params: v.clone().into_iter().collect(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn typed_context_round_trip_preserves_hash_and_handles() {
        let market = MarketSpec::new(serde_json::json!({"r0": 0.02})).unwrap();
        let model = ModelSpec::from_value(serde_json::json!({"a": 0.1, "sigma": 0.01})).unwrap();
        let product =
            ProductSpec::from_value(serde_json::json!({"payment_times": [1.0], "accruals": [1.0]}))
                .unwrap();
        let context = QuantContext::new("client")
            .add_market("m", market)
            .unwrap()
            .add_model("hw", model)
            .unwrap()
            .add_product("irs", product)
            .unwrap();
        let encoded = serde_json::to_vec(&context).unwrap();
        let decoded: QuantContext = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(context.context_hash, decoded.context_hash);
        assert_eq!(
            decoded.market_handle("m").unwrap().version(),
            context.revision
        );
        assert_eq!(
            decoded.model_handle("hw").unwrap().entry().spec.kind().0,
            "hull_white_1f"
        );
        assert_eq!(
            decoded.product_handle("irs").unwrap().entry().spec.kind().0,
            "ir_swap"
        );
    }

    #[test]
    fn branches_have_distinct_lineage_and_hashes() {
        let root = QuantContext::new("client");
        let left = root.fork("left").unwrap();
        let right = root.fork("right").unwrap();
        assert_ne!(left.context_hash, right.context_hash);
        assert_eq!(left.parent_hash, root.context_hash);
        assert_eq!(right.parent_hash, root.context_hash);
    }
}
