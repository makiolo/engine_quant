use quant_api::{router, AppState};
use quant_engine::{Engine, EngineConfig};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let engine = Engine::new(EngineConfig::default())?;
    let listener = tokio::net::TcpListener::bind(("0.0.0.0", 8080)).await?;
    axum::serve(listener, router(AppState::new(engine))).await?;
    Ok(())
}
