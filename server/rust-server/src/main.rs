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
use std::net::{IpAddr, SocketAddr};
use std::sync::Arc;
use tokio::net::TcpListener;
use tokio::sync::{RwLock, mpsc};
use tracing_subscriber::EnvFilter;
use uuid::Uuid;

#[derive(Clone)]
pub struct AppState {
    pub db: SqlitePool,
    pub raw_module: Option<Vec<u8>>,
    pub ip_tokens: Arc<RwLock<HashMap<IpAddr, String>>>,
    pub live_replies: Arc<RwLock<HashMap<String, HashMap<Uuid, mpsc::UnboundedSender<Vec<u8>>>>>>,
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

    let state = AppState {
        db,
        raw_module: None,
        ip_tokens: Arc::new(RwLock::new(HashMap::new())),
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
