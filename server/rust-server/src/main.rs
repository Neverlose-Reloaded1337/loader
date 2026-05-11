mod client_msg;
mod config;
mod data;
mod db;
mod error;
mod http;
mod models;
mod module_builder;
mod tls;
mod ws;

use sqlx::SqlitePool;
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;
use tokio::net::TcpListener;
use tokio::sync::{RwLock, mpsc};
use tracing_subscriber::EnvFilter;
use uuid::Uuid;

use crate::config::{DEFAULT_SERIAL, DEFAULT_USER_TOKEN, SINGLE_USER_NAME};
use crate::data::SEED_MODULE_BIN;
use crate::models::UserRow;

#[derive(Clone)]
pub struct AppState {
    pub db: SqlitePool,
    pub raw_module: Option<Vec<u8>>,
    pub single_user: Arc<RwLock<UserRow>>,
    pub live_replies: Arc<RwLock<HashMap<Uuid, mpsc::UnboundedSender<Vec<u8>>>>>,
}

#[tokio::main]
async fn main() {
    rustls::crypto::ring::default_provider()
        .install_default()
        .expect("failed to install rustls crypto provider");

    tracing_subscriber::fmt()
        .with_target(true)
        .with_level(true)
        .with_env_filter(EnvFilter::new("debug"))
        .init();

    let db = SqlitePool::connect("sqlite:data.db?mode=rwc")
        .await
        .expect("failed to connect to SQLite");

    sqlx::query("PRAGMA foreign_keys = ON")
        .execute(&db)
        .await
        .expect("failed to enable SQLite foreign keys");

    sqlx::migrate!("./migrations")
        .run(&db)
        .await
        .expect("failed to run database migrations");

    let single_user = ensure_single_user(&db)
        .await
        .expect("failed to initialize singleton user");

    let state = AppState {
        db,
        raw_module: None,
        single_user: Arc::new(RwLock::new(single_user)),
        live_replies: Arc::new(RwLock::new(HashMap::new())),
    };

    let http_addr = SocketAddr::from(([0, 0, 0, 0], config::HTTP_PORT));
    let https_addr = SocketAddr::from(([0, 0, 0, 0], config::HTTPS_PORT));
    let ws_addr = SocketAddr::from(([0, 0, 0, 0], config::WS_PORT));

    let http_listener = TcpListener::bind(http_addr)
        .await
        .expect("failed to bind HTTP port");

    tracing::info!("HTTP server on http://{http_addr}");

    let http_server = axum::serve(
        http_listener,
        http::router(state.clone()).into_make_service_with_connect_info::<SocketAddr>(),
    );

    let rustls_config = tls::load_rustls_config()
        .await
        .expect("failed to load TLS config");

    tracing::info!("HTTPS server on https://{https_addr}");
    tracing::info!("WS server on wss://{ws_addr}");

    let https_server = axum_server::bind_rustls(https_addr, rustls_config.clone())
        .serve(http::router(state.clone()).into_make_service_with_connect_info::<SocketAddr>());

    let ws_server = axum_server::bind_rustls(ws_addr, rustls_config)
        .serve(ws::router(state).into_make_service_with_connect_info::<SocketAddr>());

    tokio::select! {
        r = http_server => {
            if let Err(e) = r {
                tracing::error!("HTTP server error: {e}");
            }
        }
        r = https_server => {
            if let Err(e) = r {
                tracing::error!("HTTPS server error: {e}");
            }
        }
        r = ws_server => {
            if let Err(e) = r {
                tracing::error!("WSS server error: {e}");
            }
        }
        _ = tokio::signal::ctrl_c() => {
            tracing::info!("Shutting down");
        }
    }
}

async fn ensure_single_user(db: &SqlitePool) -> anyhow::Result<UserRow> {
    if let Some(user) = db::get_user_by_username(db, SINGLE_USER_NAME).await? {
        return Ok(user);
    }

    use nl_parser::module::Module;
    use nl_parser::pipeline;

    let flat = pipeline::load_module(SEED_MODULE_BIN)?;
    let module = Module::base_from_flatbuffer(&flat)?;
    let skin_data_msgpack = Module::extract_raw_skin_data(&flat)?;
    let languages_json = serde_json::to_value(&module.languages)?;

    let base_module = db::upsert_base_module(
        db,
        "default",
        module.version as i32,
        &module.author,
        module.checksum as i64,
        module.buffer_capacity as i64,
        module.enabled as i32,
        &skin_data_msgpack,
        &languages_json,
    )
    .await?;

    db::create_user(
        db,
        SINGLE_USER_NAME,
        DEFAULT_USER_TOKEN,
        base_module.id,
        DEFAULT_SERIAL,
    )
    .await
    .map_err(Into::into)
}
