//! Adaptador HTTP de la vertical v1. JSON termina aquí: el engine recibe DTOs ya validados.

use axum::{
    extract::{rejection::JsonRejection, Json, State},
    http::{header, HeaderValue, StatusCode},
    middleware,
    response::{IntoResponse, Response},
    routing::{get, post},
    Router,
};
use quant_domain::{
    ContextCommand, PricingContext, PricingInput, PricingOutput, QuantContext, ResourceRef,
    RiskRequest, RiskResult, ScenarioSet, XvaRequest, XvaResult,
};
use quant_engine::{Engine, ExecutionControl, PortfolioPricingResult, QuantError, ScenarioResult};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::sync::Arc;
use tokio::sync::{oneshot, Semaphore};
use tower_http::{limit::RequestBodyLimitLayer, trace::TraceLayer};

pub const DEFAULT_MAX_BODY_BYTES: usize = 1_048_576;

#[derive(Clone)]
pub struct AppState {
    pub engine: Engine,
    pub max_body_bytes: usize,
    wait_permits: Arc<Semaphore>,
}

/// Dropping an Axum handler future (normally because the client disconnected) cooperatively
/// cancels the owned engine operation. A provider call already running may still finish.
struct RequestCancellationGuard(ExecutionControl);
impl Drop for RequestCancellationGuard {
    fn drop(&mut self) {
        self.0.cancel();
    }
}

impl AppState {
    pub fn new(engine: Engine) -> Self {
        let permits = engine.config().worker_count + engine.config().queue_capacity;
        Self {
            engine,
            max_body_bytes: DEFAULT_MAX_BODY_BYTES,
            wait_permits: Arc::new(Semaphore::new(permits)),
        }
    }
}

pub fn router(state: AppState) -> Router {
    let max_body_bytes = state.max_body_bytes;
    Router::new()
        .route("/v1/context:apply", post(apply_context))
        .route("/v1/prices", post(price))
        .route("/v1/portfolios:price", post(price_portfolio))
        .route("/v1/scenarios:run", post(run_scenarios))
        .route("/v1/risk:calculate", post(calculate_risk))
        .route("/v1/risk", post(calculate_risk))
        .route("/v1/xva:calculate", post(calculate_xva))
        .route("/v1/xva", post(calculate_xva))
        .route("/health/live", get(health_live))
        .route("/health/ready", get(health_ready))
        .with_state(Arc::new(state))
        .layer(RequestBodyLimitLayer::new(max_body_bytes))
        .layer(middleware::from_fn(reject_oversized_content_length))
        .layer(TraceLayer::new_for_http())
}

async fn reject_oversized_content_length(
    request: axum::http::Request<axum::body::Body>,
    next: middleware::Next,
) -> Response {
    let too_large = request
        .headers()
        .get(header::CONTENT_LENGTH)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.parse::<usize>().ok())
        .is_some_and(|length| length > DEFAULT_MAX_BODY_BYTES);
    if too_large {
        ProblemDetails::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "body_limit_exceeded",
            "request body exceeds the configured byte limit",
        )
        .into_response()
    } else {
        next.run(request).await
    }
}

#[derive(Debug, Deserialize)]
pub struct ContextOperation {
    pub context: QuantContext,
    pub operation_id: String,
    #[serde(default)]
    pub commands: Vec<ContextCommand>,
    #[serde(default)]
    pub resource: Option<Value>,
}

#[derive(Debug, Serialize)]
pub struct ContextOperationResponse {
    pub context: QuantContext,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub resource_ref: Option<ResourceRef>,
}

#[derive(Debug, Deserialize)]
pub struct PortfolioPriceRequest {
    pub context: QuantContext,
    pub operation_id: String,
    pub portfolio_id: String,
    pub market: ResourceRef,
    pub model: ResourceRef,
    #[serde(default)]
    pub pricing: PricingContext,
}

#[derive(Debug, Serialize)]
pub struct PortfolioPriceResponse {
    pub operation_id: String,
    pub context_hash: Option<quant_domain::ContextHash>,
    pub result: PortfolioPricingResult,
    pub continuation_token: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct ScenarioRunRequest {
    pub context: QuantContext,
    pub operation_id: String,
    pub portfolio_id: String,
    pub market: ResourceRef,
    pub model: ResourceRef,
    pub scenarios: ScenarioSet,
    #[serde(default)]
    pub pricing: PricingContext,
}

#[derive(Debug, Serialize)]
pub struct ScenarioRunResponse {
    pub operation_id: String,
    pub context_hash: Option<quant_domain::ContextHash>,
    pub result: ScenarioResult,
}

async fn apply_context(
    State(state): State<Arc<AppState>>,
    request: Result<Json<ContextOperation>, JsonRejection>,
) -> Result<Json<ContextOperationResponse>, ProblemDetails> {
    let Json(request) = request.map_err(|_| {
        ProblemDetails::bad_request(
            "invalid_json",
            "request body must be valid JSON matching the v1 contract",
        )
    })?;
    if request.operation_id.trim().is_empty() {
        return Err(ProblemDetails::bad_request(
            "invalid_request",
            "operation_id is required",
        ));
    }
    let (context, resource_ref) = state
        .engine
        .apply_context(&request.context, &request.commands)
        .map_err(ProblemDetails::from_quant)?;
    Ok(Json(ContextOperationResponse {
        context,
        resource_ref,
    }))
}

async fn price(
    State(state): State<Arc<AppState>>,
    request: Result<Json<PricingInput>, JsonRejection>,
) -> Result<Json<PricingOutput>, ProblemDetails> {
    let Json(request) = request.map_err(|_| {
        ProblemDetails::bad_request(
            "invalid_json",
            "request body must be valid JSON matching the v1 contract",
        )
    })?;
    let permit = state
        .wait_permits
        .clone()
        .try_acquire_owned()
        .map_err(|_| {
            ProblemDetails::new(
                StatusCode::TOO_MANY_REQUESTS,
                "queue_full",
                "pricing admission is full",
            )
        })?;
    let engine = state.engine.clone();
    let control = ExecutionControl::new();
    let _cancellation_guard = RequestCancellationGuard(control.clone());
    let (sender, receiver) = oneshot::channel();
    let task_control = control.cancellation_token();
    engine
        .submit_price_callback(request, task_control, move |result| {
            let _ = sender.send(result);
            drop(permit);
        })
        .map_err(ProblemDetails::from_quant)?;
    let output = receiver
        .await
        .map_err(|_| {
            ProblemDetails::service_unavailable("worker_stopped", "pricing worker stopped")
        })?
        .map_err(ProblemDetails::from_quant)?;
    Ok(Json(output))
}

async fn price_portfolio(
    State(state): State<Arc<AppState>>,
    request: Result<Json<PortfolioPriceRequest>, JsonRejection>,
) -> Result<Json<PortfolioPriceResponse>, ProblemDetails> {
    let Json(request) = request.map_err(|_| {
        ProblemDetails::bad_request("invalid_json", "request body must be valid JSON")
    })?;
    if let Some(error) = validate_operation_id(&request.operation_id) {
        return Err(error);
    }
    let portfolio = request
        .context
        .portfolio(&request.portfolio_id)
        .map_err(|error| ProblemDetails::bad_request("invalid_context", error.to_string()))?;
    let market = request
        .context
        .market_handle(&request.market.id)
        .map_err(|error| ProblemDetails::bad_request("invalid_context", error.to_string()))?;
    let model = request
        .context
        .model_handle(&request.model.id)
        .map_err(|error| ProblemDetails::bad_request("invalid_context", error.to_string()))?;
    let permit = state
        .wait_permits
        .clone()
        .try_acquire_owned()
        .map_err(|_| {
            ProblemDetails::new(
                StatusCode::TOO_MANY_REQUESTS,
                "queue_full",
                "pricing admission is full",
            )
        })?;
    let engine = state.engine.clone();
    let context = request.context.clone();
    let pricing = request.pricing.clone();
    let control = ExecutionControl::new();
    let _cancellation_guard = RequestCancellationGuard(control.clone());
    let (sender, receiver) = oneshot::channel();
    let task_control = control.cancellation_token();
    let control_for_task = control.clone();
    let task_engine = engine.clone();
    engine
        .submit_task(
            quant_engine::JobCost::new(1, 64 * 1024),
            task_control,
            move || {
                let result = if control_for_task.is_cancelled() {
                    Err(QuantError::Cancelled)
                } else {
                    task_engine.price_portfolio_with_control(
                        &portfolio,
                        &model,
                        &market,
                        &pricing,
                        &control_for_task,
                    )
                };
                let _ = sender.send(result);
                drop(permit);
            },
        )
        .map_err(ProblemDetails::from_quant)?;
    let result = receiver
        .await
        .map_err(|_| ProblemDetails::service_unavailable("worker_stopped", "worker stopped"))?
        .map_err(ProblemDetails::from_quant)?;
    Ok(Json(PortfolioPriceResponse {
        operation_id: request.operation_id,
        context_hash: context.context_hash,
        result,
        continuation_token: None,
    }))
}

async fn run_scenarios(
    State(state): State<Arc<AppState>>,
    request: Result<Json<ScenarioRunRequest>, JsonRejection>,
) -> Result<Json<ScenarioRunResponse>, ProblemDetails> {
    let Json(request) = request.map_err(|_| {
        ProblemDetails::bad_request("invalid_json", "request body must be valid JSON")
    })?;
    if let Some(error) = validate_operation_id(&request.operation_id) {
        return Err(error);
    }
    let portfolio = request
        .context
        .portfolio(&request.portfolio_id)
        .map_err(|error| ProblemDetails::bad_request("invalid_context", error.to_string()))?;
    let market = request
        .context
        .market_handle(&request.market.id)
        .map_err(|error| ProblemDetails::bad_request("invalid_context", error.to_string()))?;
    let model = request
        .context
        .model_handle(&request.model.id)
        .map_err(|error| ProblemDetails::bad_request("invalid_context", error.to_string()))?;
    let permit = state
        .wait_permits
        .clone()
        .try_acquire_owned()
        .map_err(|_| {
            ProblemDetails::new(
                StatusCode::TOO_MANY_REQUESTS,
                "queue_full",
                "pricing admission is full",
            )
        })?;
    let engine = state.engine.clone();
    let context = request.context.clone();
    let pricing = request.pricing.clone();
    let scenarios = request.scenarios.clone();
    let control = ExecutionControl::new();
    let _cancellation_guard = RequestCancellationGuard(control.clone());
    let (sender, receiver) = oneshot::channel();
    let task_control = control.cancellation_token();
    let control_for_task = control.clone();
    let task_engine = engine.clone();
    engine
        .submit_task(
            quant_engine::JobCost::new(1, 64 * 1024),
            task_control,
            move || {
                let result = if control_for_task.is_cancelled() {
                    Err(QuantError::Cancelled)
                } else {
                    task_engine.run_scenarios_with_control(
                        &portfolio,
                        &scenarios,
                        &model,
                        &market,
                        &pricing,
                        &control_for_task,
                    )
                };
                let _ = sender.send(result);
                drop(permit);
            },
        )
        .map_err(ProblemDetails::from_quant)?;
    let result = receiver
        .await
        .map_err(|_| ProblemDetails::service_unavailable("worker_stopped", "worker stopped"))?
        .map_err(ProblemDetails::from_quant)?;
    Ok(Json(ScenarioRunResponse {
        operation_id: request.operation_id,
        context_hash: context.context_hash,
        result,
    }))
}

/// Calculate dense portfolio Greeks using the engine's shared base/bump/AAD artifact graph.
/// The request remains client-owned and stateless, exactly like the pricing endpoints.
async fn calculate_risk(
    State(state): State<Arc<AppState>>,
    request: Result<Json<RiskRequest>, JsonRejection>,
) -> Result<Json<RiskResult>, ProblemDetails> {
    let Json(request) = request.map_err(|_| {
        ProblemDetails::bad_request("invalid_json", "request body must be valid JSON")
    })?;
    if let Some(error) = validate_operation_id(&request.operation_id) {
        return Err(error);
    }
    let permit = state
        .wait_permits
        .clone()
        .try_acquire_owned()
        .map_err(|_| {
            ProblemDetails::new(
                StatusCode::TOO_MANY_REQUESTS,
                "queue_full",
                "risk admission is full",
            )
        })?;
    let engine = state.engine.clone();
    let control = ExecutionControl::new();
    let _cancellation_guard = RequestCancellationGuard(control.clone());
    let (sender, receiver) = oneshot::channel();
    let task_control = control.cancellation_token();
    let control_for_task = control.clone();
    let task_engine = engine.clone();
    engine
        .submit_task(
            quant_engine::JobCost::new(1, 64 * 1024),
            task_control,
            move || {
                let result = if control_for_task.is_cancelled() {
                    Err(QuantError::Cancelled)
                } else {
                    task_engine.calculate_risk_with_control(&request, &control_for_task)
                };
                let _ = sender.send(result);
                drop(permit);
            },
        )
        .map_err(ProblemDetails::from_quant)?;
    let result = receiver
        .await
        .map_err(|_| ProblemDetails::service_unavailable("worker_stopped", "worker stopped"))?
        .map_err(ProblemDetails::from_quant)?;
    Ok(Json(result))
}

/// Calculate all requested XVA measures from a shared Q/P exposure graph.
async fn calculate_xva(
    State(state): State<Arc<AppState>>,
    request: Result<Json<XvaRequest>, JsonRejection>,
) -> Result<Json<XvaResult>, ProblemDetails> {
    let Json(request) = request.map_err(|_| {
        ProblemDetails::bad_request("invalid_json", "request body must be valid JSON")
    })?;
    if let Some(error) = validate_operation_id(&request.operation_id) {
        return Err(error);
    }
    let permit = state
        .wait_permits
        .clone()
        .try_acquire_owned()
        .map_err(|_| {
            ProblemDetails::new(
                StatusCode::TOO_MANY_REQUESTS,
                "queue_full",
                "XVA admission is full",
            )
        })?;
    let engine = state.engine.clone();
    let control = ExecutionControl::new();
    let _cancellation_guard = RequestCancellationGuard(control.clone());
    let (sender, receiver) = oneshot::channel();
    let task_control = control.cancellation_token();
    let control_for_task = control.clone();
    let task_engine = engine.clone();
    engine
        .submit_task(
            quant_engine::JobCost::new(1, request.memory_budget_bytes.unwrap_or(64 * 1024)),
            task_control,
            move || {
                let result = if control_for_task.is_cancelled() {
                    Err(QuantError::Cancelled)
                } else {
                    task_engine.calculate_xva_with_control(&request, &control_for_task)
                };
                let _ = sender.send(result);
                drop(permit);
            },
        )
        .map_err(ProblemDetails::from_quant)?;
    let result = receiver
        .await
        .map_err(|_| ProblemDetails::service_unavailable("worker_stopped", "worker stopped"))?
        .map_err(ProblemDetails::from_quant)?;
    Ok(Json(result))
}

fn validate_operation_id(operation_id: &str) -> Option<ProblemDetails> {
    operation_id
        .trim()
        .is_empty()
        .then(|| ProblemDetails::bad_request("invalid_request", "operation_id is required"))
}

#[derive(Debug, Serialize)]
struct Health {
    status: &'static str,
}

async fn health_live() -> Json<Health> {
    Json(Health { status: "ok" })
}

async fn health_ready(State(state): State<Arc<AppState>>) -> Response {
    if state.engine.is_ready() {
        (StatusCode::OK, Json(Health { status: "ready" })).into_response()
    } else {
        ProblemDetails::service_unavailable("queue_full", "pricing queue is full").into_response()
    }
}

#[derive(Debug, Serialize)]
pub struct ProblemDetails {
    #[serde(rename = "type")]
    pub type_uri: String,
    pub title: String,
    pub status: u16,
    pub detail: String,
    pub code: String,
    pub trace_id: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub field_errors: Vec<FieldError>,
}

#[derive(Debug, Serialize)]
pub struct FieldError {
    pub field: String,
    pub message: String,
}

impl ProblemDetails {
    pub fn bad_request(code: impl Into<String>, detail: impl Into<String>) -> Self {
        Self::new(StatusCode::BAD_REQUEST, code, detail)
    }
    pub fn service_unavailable(code: impl Into<String>, detail: impl Into<String>) -> Self {
        Self::new(StatusCode::SERVICE_UNAVAILABLE, code, detail)
    }
    fn new(status: StatusCode, code: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            type_uri: "https://quant.example/problems/quant-error".into(),
            title: status.canonical_reason().unwrap_or("Request error").into(),
            status: status.as_u16(),
            detail: detail.into(),
            code: code.into(),
            trace_id: format!("local-{}", std::process::id()),
            field_errors: Vec::new(),
        }
    }
    fn from_quant(error: QuantError) -> Self {
        match error {
            QuantError::QueueFull => Self::new(
                StatusCode::TOO_MANY_REQUESTS,
                "queue_full",
                error.to_string(),
            ),
            QuantError::Cancelled => {
                Self::new(StatusCode::CONFLICT, "cancelled", error.to_string())
            }
            QuantError::WorkerStopped => {
                Self::service_unavailable("worker_stopped", error.to_string())
            }
            QuantError::InvalidRequest(detail) => Self::bad_request("invalid_request", detail),
            QuantError::ResourceNotFound { .. } => Self::new(
                StatusCode::NOT_FOUND,
                "resource_not_found",
                error.to_string(),
            ),
            QuantError::UnsupportedMeasure(_) => {
                Self::bad_request("unsupported_measure", error.to_string())
            }
            QuantError::Domain(detail) => Self::bad_request("invalid_context", detail),
            QuantError::Legacy { .. } => {
                Self::new(StatusCode::BAD_GATEWAY, "legacy_backend", error.to_string())
            }
        }
    }
}

impl IntoResponse for ProblemDetails {
    fn into_response(self) -> Response {
        let body = Json(self);
        let mut response = (
            StatusCode::from_u16(body.0.status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR),
            body,
        )
            .into_response();
        response.headers_mut().insert(
            header::CONTENT_TYPE,
            HeaderValue::from_static("application/problem+json"),
        );
        response
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::{to_bytes, Body};
    use axum::http::{Request, StatusCode};
    use quant_engine::EngineConfig;
    use serde_json::json;
    use tower::ServiceExt;

    fn app() -> Router {
        router(AppState::new(Engine::new(EngineConfig::default()).unwrap()))
    }

    fn portfolio_context(include_bad_trade: bool) -> QuantContext {
        let trades = if include_bad_trade {
            json!([{"trade_id":"good","product_id":"irs"},{"trade_id":"bad","product_id":"missing"}])
        } else {
            json!([{"trade_id":"t-1","product_id":"irs"}])
        };
        QuantContext::new("client").apply(&[
            ContextCommand::AddMarket { id: "m".into(), spec: json!({"r0":0.02}) },
            ContextCommand::AddModel { id: "hw".into(), spec: json!({"a":0.1,"b":0.03,"sigma":0.01}) },
            ContextCommand::AddProduct { id: "irs".into(), spec: json!({"notional":1000000,"fixed_rate":0.02,"payment_times":[1.0,2.0],"accruals":[1.0,1.0]}) },
            ContextCommand::AddPortfolio { id: "book".into(), spec: json!({"trades":trades}) },
        ]).unwrap().0
    }

    fn ref_json(id: &str, kind: &str) -> serde_json::Value {
        json!({"id":id,"hash":"","kind":kind,"version":1})
    }

    #[tokio::test]
    async fn context_apply_and_price_round_trip() {
        let context = QuantContext::new("client");
        let payload = json!({"context": context, "operation_id":"ctx-1", "commands":[
            {"kind":"add_market","id":"m","spec":{"r0":0.02}},
            {"kind":"add_model","id":"hw","spec":{"a":0.1,"b":0.03,"sigma":0.01}},
            {"kind":"add_product","id":"irs","spec":{"notional":1000000,"fixed_rate":0.02,"payment_times":[1,2],"accruals":[1,1]}}
        ]});
        let response = app()
            .oneshot(
                Request::post("/v1/context:apply")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
    }

    #[tokio::test]
    async fn problem_details_are_stable() {
        let response = app()
            .oneshot(
                Request::post("/v1/prices")
                    .header("content-type", "application/json")
                    .body(Body::from("{}"))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::BAD_REQUEST);
        assert_eq!(
            response.headers()[header::CONTENT_TYPE],
            "application/problem+json"
        );
    }

    #[tokio::test]
    async fn price_endpoint_executes_on_fixed_pool_without_nested_submission() {
        let context = portfolio_context(false);
        let payload = json!({
            "context": context,
            "operation_id": "price-1",
            "market": ref_json("m", "market"),
            "model": ref_json("hw", "model"),
            "products": [ref_json("irs", "product")],
            "measures": [{"name": "PV", "params": {}}]
        });
        let response = app()
            .oneshot(
                Request::post("/v1/prices")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
    }

    #[tokio::test]
    async fn oversized_body_is_problem_details() {
        let body = vec![b'x'; DEFAULT_MAX_BODY_BYTES + 1];
        let response = app()
            .oneshot(
                Request::post("/v1/prices")
                    .header("content-type", "application/json")
                    .header("content-length", (DEFAULT_MAX_BODY_BYTES + 1).to_string())
                    .body(Body::from(body))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::PAYLOAD_TOO_LARGE);
        assert_eq!(
            response.headers()[header::CONTENT_TYPE],
            "application/problem+json"
        );
    }

    #[tokio::test]
    async fn portfolio_endpoint_prices_and_reports_partial_failures() {
        let context = portfolio_context(true);
        let payload = json!({"context":context,"operation_id":"portfolio-1","portfolio_id":"book",
            "market":ref_json("m","market"),"model":ref_json("hw","model")});
        let response = app()
            .oneshot(
                Request::post("/v1/portfolios:price")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = to_bytes(response.into_body(), DEFAULT_MAX_BODY_BYTES)
            .await
            .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(value["operation_id"], "portfolio-1");
        assert_eq!(value["result"]["trade_ids"], json!(["good"]));
        assert_eq!(value["result"]["failures"][0]["item_id"], "bad");
    }

    #[tokio::test]
    async fn scenarios_endpoint_returns_stable_rows_and_no_continuation() {
        let context = portfolio_context(false);
        let payload = json!({"context":context,"operation_id":"scenario-1","portfolio_id":"book",
            "market":ref_json("m","market"),"model":ref_json("hw","model"),
            "scenarios":{"id":"set-1","scenarios":[{"id":"base","shocks":{}},{"id":"up","shocks":{"r0":0.01}}]}});
        let response = app()
            .oneshot(
                Request::post("/v1/scenarios:run")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = to_bytes(response.into_body(), DEFAULT_MAX_BODY_BYTES)
            .await
            .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(value["result"]["scenarios"][0]["scenario_id"], "base");
        assert_eq!(value["result"]["scenarios"][1]["scenario_id"], "up");
        assert!(value["result"]["continuation_token"].is_null());
    }

    #[tokio::test]
    async fn risk_endpoint_returns_dense_indexed_greeks() {
        let context = portfolio_context(false);
        let payload = json!({"context":context,"operation_id":"risk-1","portfolio_id":"book",
            "market":ref_json("m","market"),"model":ref_json("hw","model"),
            "measures":["delta","vega"]});
        let response = app()
            .oneshot(
                Request::post("/v1/risk:calculate")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = to_bytes(response.into_body(), DEFAULT_MAX_BODY_BYTES)
            .await
            .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(value["operation_id"], "risk-1");
        assert_eq!(value["shape"], json!([2, 2]));
        assert_eq!(value["values"].as_array().unwrap().len(), 4);
        assert!(value["provenance"]["delta"]["method"].is_string());
    }

    #[tokio::test]
    async fn xva_endpoint_returns_result_and_rejects_exposure_context_mismatch() {
        let context = QuantContext::new("xva-client");
        let context_hash = context.context_hash.as_ref().unwrap().0.clone();
        let payload = json!({
            "context": context,
            "operation_id": "xva-1",
            "portfolio_id": "book",
            "market": ref_json("m", "market"),
            "model": ref_json("hw", "model"),
            "measures": ["ee", "pfe", "cva", "dva", "fva", "mva", "kva"],
            "q_exposure": {
                "measure": "Q",
                "times": [1.0, 2.0],
                "paths": [[10.0, -5.0], [12.0, -6.0]],
                "source_hash": context_hash
            }
        });
        let response = app()
            .oneshot(
                Request::post("/v1/xva:calculate")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = to_bytes(response.into_body(), DEFAULT_MAX_BODY_BYTES)
            .await
            .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(value["operation_id"], "xva-1");
        assert!(value["cva"].is_number());
        assert_eq!(value["provenance"]["shared_exposure_graph"], true);
        assert!(
            value["provenance"]["artifact_lineage"]
                .as_array()
                .unwrap()
                .len()
                >= 4
        );

        let mismatch = json!({
            "context": QuantContext::new("xva-mismatch"),
            "operation_id": "xva-mismatch",
            "portfolio_id": "book",
            "market": ref_json("m", "market"),
            "model": ref_json("hw", "model"),
            "q_exposure": {
                "measure": "Q",
                "times": [1.0],
                "paths": [[1.0]],
                "source_hash": "sha256:not-the-context"
            }
        });
        let response = app()
            .oneshot(
                Request::post("/v1/xva:calculate")
                    .header("content-type", "application/json")
                    .body(Body::from(mismatch.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::BAD_REQUEST);
        assert_eq!(
            response.headers()[header::CONTENT_TYPE],
            "application/problem+json"
        );
        let body = to_bytes(response.into_body(), DEFAULT_MAX_BODY_BYTES)
            .await
            .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(value["code"], "invalid_request");
        assert!(value["detail"].as_str().unwrap().contains("source hash"));
    }

    #[tokio::test]
    async fn portfolio_backpressure_is_explicit() {
        let context = portfolio_context(false);
        let payload = json!({"context":context,"operation_id":"busy-1","portfolio_id":"book",
            "market":ref_json("m","market"),"model":ref_json("hw","model")});
        let state = AppState::new(Engine::new(EngineConfig::default()).unwrap());
        state.wait_permits.close();
        let response = router(state)
            .oneshot(
                Request::post("/v1/portfolios:price")
                    .header("content-type", "application/json")
                    .body(Body::from(payload.to_string()))
                    .unwrap(),
            )
            .await
            .unwrap();
        assert_eq!(response.status(), StatusCode::TOO_MANY_REQUESTS);
    }

    #[tokio::test]
    async fn liveness_stays_responsive_while_cpu_pool_is_busy() {
        use std::sync::atomic::AtomicBool;
        let engine = Engine::new(EngineConfig {
            worker_count: 1,
            queue_capacity: 1,
            ..Default::default()
        })
        .unwrap();
        let ran = Arc::new(AtomicBool::new(false));
        let ran_in_task = Arc::clone(&ran);
        engine
            .submit_task(
                quant_engine::JobCost::new(1, 4096),
                Arc::new(AtomicBool::new(false)),
                move || {
                    std::thread::sleep(std::time::Duration::from_millis(100));
                    ran_in_task.store(true, std::sync::atomic::Ordering::Release);
                },
            )
            .unwrap();
        let app = router(AppState::new(engine));
        let live = app
            .clone()
            .oneshot(Request::get("/health/live").body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(live.status(), StatusCode::OK);
        let ready = app
            .oneshot(Request::get("/health/ready").body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(ready.status(), StatusCode::SERVICE_UNAVAILABLE);
        tokio::time::sleep(std::time::Duration::from_millis(120)).await;
        assert!(ran.load(std::sync::atomic::Ordering::Acquire));
    }

    #[test]
    fn request_guard_cancels_engine_control_when_handler_is_dropped() {
        let control = ExecutionControl::new();
        {
            let _guard = RequestCancellationGuard(control.clone());
        }
        assert!(control.is_cancelled());
    }
}
