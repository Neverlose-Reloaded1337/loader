use std::{
    env,
    path::Path,
};

use axum::{
    Router,
    body::Body,
    extract::{Path as AxumPath, Query, Request, State},
    http::{HeaderMap, HeaderValue, StatusCode, header},
    response::{Html, IntoResponse, Response},
    routing::{get, post},
};
use serde::Deserialize;
use serde_json::json;
use std::collections::HashMap;
use tower::ServiceBuilder;
use tower_http::set_header::SetResponseHeaderLayer;
use tower_http::trace::TraceLayer;
use uuid::Uuid;

use crate::config;
use crate::data::DEFAULT_AVATAR_PNG;
use crate::error::AppError;
use crate::{AppState, db, local_storage, ws};

pub fn router(state: AppState) -> Router {
    let middleware = ServiceBuilder::new()
        .layer(TraceLayer::new_for_http())
        .layer(SetResponseHeaderLayer::overriding(
            header::HeaderName::from_static("x-powered-by"),
            HeaderValue::from_static("Express"),
        ))
        .layer(SetResponseHeaderLayer::if_not_present(
            header::CONNECTION,
            HeaderValue::from_static("keep-alive"),
        ))
        .layer(SetResponseHeaderLayer::if_not_present(
            header::HeaderName::from_static("keep-alive"),
            HeaderValue::from_static("timeout=5"),
        ));

    Router::new()
        .route("/", get(index_handler))
        .route("/api/me", get(me_handler))
        .route("/api/avatar", post(update_avatar_handler))
        .route("/api/config", get(config_handler))
        .route("/api/getavatar", get(avatar_handler))
        .route("/getavatar", get(avatar_handler))
        .route("/api/sendlog", get(sendlog_handler))
        .route("/sendlog", get(sendlog_handler))
        .route("/lua/{*name}", get(lua_handler))
        .route("/api/reqitem", get(reqitem_handler))
        .route("/api/items", get(items_handler))
        .route("/api/share", post(share_handler))
        .route("/api/myshares", get(myshares_handler))
        .route("/api/unshare", post(unshare_handler))
        .route("/api/shared/{code}", get(shared_handler))
        .route("/api/import_to_account", post(import_to_account_handler))
        .fallback(fallback_handler)
        .layer(middleware)
        .with_state(state)
}

async fn index_handler() -> Html<&'static str> {
    Html(include_str!("../web/index.html"))
}

async fn config_handler(Query(params): Query<HashMap<String, String>>) -> impl IntoResponse {
    tracing::info!("[HTTP] GET /api/config params={:?}", params);
    let resp = json!({
        "status": "ok",
        "version": "2.0",
        "update": false,
        "config": {
            "glow": true,
            "esp": true,
            "aimbot": true,
            "misc": true,
        }
    });
    tracing::debug!("[HTTP] -> config response: {}", resp);
    axum::Json(resp)
}

async fn me_handler(State(state): State<AppState>) -> Result<Response, AppError> {
    let user = current_user(&state).await;

    Ok(response_json(
        StatusCode::OK,
        json!({
            "status": "ok",
            "username": user.username,
            "has_avatar": false,
            "avatar_url": "/api/getavatar",
        }),
    ))
}

async fn update_avatar_handler(
    State(state): State<AppState>,
    _payload: Option<axum::Json<serde_json::Value>>,
) -> Result<Response, AppError> {
    let user = current_user(&state).await;
    replace_current_user(&state, user).await;

    Ok(response_json(
        StatusCode::OK,
        json!({
            "status": "ok",
            "has_avatar": false,
            "avatar_url": "/api/getavatar",
        }),
    ))
}

async fn avatar_handler(
    State(state): State<AppState>,
    Query(params): Query<HashMap<String, String>>,
) -> impl IntoResponse {
    let size = params.get("size").cloned().unwrap_or_default();
    tracing::info!("[HTTP] GET /api/getavatar size={}", size);
    let _ = state;

    default_avatar()
}

async fn sendlog_handler(Query(params): Query<HashMap<String, String>>) -> impl IntoResponse {
    tracing::info!("[HTTP] GET /api/sendlog params={:?}", params);
    tracing::debug!("[HTTP] -> sendlog OK");
    axum::Json(json!({"status": "ok"}))
}

async fn lua_handler(
    State(state): State<AppState>,
    AxumPath(name): AxumPath<String>,
) -> impl IntoResponse {
    tracing::info!("[HTTP] GET /lua/{}", name);
    let user = current_user(&state).await;
    let body = db::get_user_script_by_name(&state.db, user.id, &name)
        .await
        .ok()
        .flatten()
        .map(|script| script.content)
        .filter(|content| !content.is_empty())
        .unwrap_or_else(|| format!("-- lua library: {name}\n"))
        .into_bytes();
    tracing::debug!("[HTTP] -> lua response ({} bytes)", body.len());
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
        .body(Body::from(body))
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

async fn reqitem_handler(Query(params): Query<HashMap<String, String>>) -> Response {
    let name_preview = params
        .get("name")
        .map(|name| preview_text(name, 120))
        .unwrap_or_else(|| "<missing>".to_string());
    tracing::info!(
        "[HTTP] GET /api/reqitem name_preview={} cheat={:?} token_present={}",
        name_preview,
        params.get("cheat"),
        params.get("token").is_some()
    );

    let requested = params
        .get("name")
        .and_then(|name| requested_lua_library_name(name));

    let Some(name) = requested else {
        tracing::debug!("[HTTP] /api/reqitem non-library payload acknowledged");
        return empty_ok();
    };

    let Some(library) = lua_library_response(&name).await else {
        tracing::warn!("[HTTP] /api/reqitem missing library {}", name);
        return empty_ok();
    };

    tracing::debug!(
        "[HTTP] -> reqitem library={} source={} bytes={}",
        name,
        library.source,
        library.body.len()
    );
    let body = reqitem_library_json(&name, &library.body);
    let content_len = body.len().to_string();
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "application/json; charset=utf-8")
        .header(header::CONTENT_LENGTH, content_len)
        .body(Body::from(body))
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

#[derive(Clone)]
struct LuaLibrary {
    source: String,
    body: Vec<u8>,
}

fn requested_lua_library_name(name: &str) -> Option<String> {
    let trimmed = name.trim();
    let library_name = trimmed
        .strip_prefix("-- lua library:")
        .map(str::trim)
        .unwrap_or(trimmed);

    sanitize_lua_library_name(library_name)
}

fn sanitize_lua_library_name(name: &str) -> Option<String> {
    let normalized = name.trim().trim_matches('/').replace('\\', "/");
    if normalized.is_empty()
        || normalized.len() > 160
        || normalized
            .chars()
            .any(|ch| ch.is_control() || matches!(ch, '<' | '>' | ':' | '"' | '|' | '?' | '*'))
        || normalized
            .split('/')
            .any(|part| part.is_empty() || part == "." || part == "..")
        || normalized.contains(':')
    {
        return None;
    }
    Some(normalized)
}

async fn lua_library_response(name: &str) -> Option<LuaLibrary> {
    let name = sanitize_lua_library_name(name)?;
    for candidate in lua_library_urls(&name) {
        match fetch_lua_library(&candidate).await {
            Ok(body) => {
                tracing::info!(
                    "[HTTP] lua library {} -> {} ({} bytes)",
                    name,
                    candidate,
                    body.len()
                );
                return Some(LuaLibrary {
                    source: candidate,
                    body,
                });
            }
            Err(err) => {
                tracing::warn!("[HTTP] failed fetching lua library {}: {}", candidate, err);
            }
        }
    }
    None
}

fn lua_library_urls(name: &str) -> Vec<String> {
    let mut candidates = Vec::new();
    let base_url = lua_library_base_url();
    let requested = Path::new(name);
    let has_extension = requested.extension().is_some();

    candidates.push(format!("{base_url}/{name}"));
    if !has_extension {
        candidates.push(format!("{base_url}/{name}.bin"));
        candidates.push(format!("{base_url}/{name}.lua"));
    }

    candidates
}

fn lua_library_base_url() -> String {
    env::var("LUA_LIBRARY_BASE_URL")
        .ok()
        .map(|value| value.trim().trim_end_matches('/').to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| {
            "https://raw.githubusercontent.com/Neverlose-Reloaded1337/Neverlose-Reloaded/main/libraries/open_source".to_string()
        })
}

async fn fetch_lua_library(url: &str) -> Result<Vec<u8>, reqwest::Error> {
    let response = reqwest::Client::new()
        .get(url)
        .header(reqwest::header::USER_AGENT, "neverlose-server/0.1")
        .send()
        .await?
        .error_for_status()?;

    let body = response.bytes().await?;
    Ok(body.to_vec())
}

fn reqitem_library_json(name: &str, body: &[u8]) -> Vec<u8> {
    let body_text = String::from_utf8_lossy(body);
    let item = json!({
        "succ": true,
        "closure": 0,
        "name": name,
        "type": "library",
        "content": body_text,
        "source": body_text,
        "data": body_text,
        "body": body_text,
        "code": body_text,
    });
    let payload = json!({
        "succ": true,
        "closure": 0,
        "name": name,
        "type": "library",
        "content": body_text,
        "source": body_text,
        "data": body_text,
        "item": item,
        "library": item,
        "body": body_text,
        "script": item,
        "code": body_text,
    });

    serde_json::to_vec(&payload).unwrap_or_else(|_| b"{\"succ\":false}".to_vec())
}

fn preview_text(text: &str, max_chars: usize) -> String {
    let mut preview: String = text.chars().take(max_chars).collect();
    if text.chars().count() > max_chars {
        preview.push_str("...");
    }
    preview.escape_debug().to_string()
}

fn empty_ok() -> Response {
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
        .body(Body::empty())
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

async fn items_handler(State(state): State<AppState>) -> Result<Response, AppError> {
    let user = current_user(&state).await;

    let scripts = db::get_user_scripts(&state.db, user.id).await?;
    let configs = db::get_user_configs(&state.db, user.id).await?;
    let styles = db::get_user_styles(&state.db, user.id).await?;

    Ok(response_json(
        StatusCode::OK,
        json!({
            "status": "ok",
            "scripts": scripts,
            "configs": configs,
            "styles": styles,
        }),
    ))
}

async fn share_handler(
    State(state): State<AppState>,
    headers: HeaderMap,
    axum::Json(req): axum::Json<ShareReq>,
) -> Result<Response, AppError> {
    let user = current_user(&state).await;

    let item_id =
        Uuid::parse_str(&req.item_id).map_err(|_| AppError(anyhow::anyhow!("invalid item_id")))?;

    let (item_name, item_exists) = match req.item_type.as_str() {
        "Script" => {
            let scripts = db::get_user_scripts(&state.db, user.id).await?;
            let found = scripts.iter().find(|s| s.id == item_id);
            (
                found.map(|s| s.name.clone()).unwrap_or_default(),
                found.is_some(),
            )
        }
        "Config" => {
            let configs = db::get_user_configs(&state.db, user.id).await?;
            let found = configs.iter().find(|c| c.id == item_id);
            (
                found.map(|c| c.name.clone()).unwrap_or_default(),
                found.is_some(),
            )
        }
        "Style" => {
            let styles = db::get_user_styles(&state.db, user.id).await?;
            let found = styles.iter().find(|s| s.id == item_id);
            (
                found.map(|s| s.name.clone()).unwrap_or_default(),
                found.is_some(),
            )
        }
        _ => {
            return Ok(response_json(
                StatusCode::BAD_REQUEST,
                json!({"error": "invalid item type"}),
            ));
        }
    };

    if !item_exists {
        return Ok(response_json(
            StatusCode::NOT_FOUND,
            json!({"error": "item not found"}),
        ));
    }

    let share =
        db::create_share_code(&state.db, user.id, &req.item_type, item_id, &item_name).await?;
    let share_url = share_url_from_headers(&headers, &share.share_code);
    Ok(response_json(
        StatusCode::CREATED,
        json!({
            "status": "ok",
            "share_code": share.share_code,
            "share_url": share_url,
            "item_type": share.item_type,
            "item_name": share.item_name,
        }),
    ))
}

async fn myshares_handler(State(state): State<AppState>) -> Result<Response, AppError> {
    let user = current_user(&state).await;
    let shares = db::list_user_share_codes(&state.db, user.id).await?;
    Ok(response_json(
        StatusCode::OK,
        json!({"status": "ok", "shares": shares}),
    ))
}

async fn unshare_handler(
    State(state): State<AppState>,
    axum::Json(req): axum::Json<UnshareReq>,
) -> Result<Response, AppError> {
    let user = current_user(&state).await;

    let Some(share) = db::get_share_code(&state.db, &req.share_code).await? else {
        return Ok(response_json(
            StatusCode::NOT_FOUND,
            json!({"error": "share code not found"}),
        ));
    };
    if share.user_id != user.id {
        return Ok(response_json(
            StatusCode::FORBIDDEN,
            json!({"error": "share code does not belong to you"}),
        ));
    }

    db::delete_share_code(&state.db, &req.share_code).await?;
    Ok(response_json(StatusCode::OK, json!({"status": "ok"})))
}

async fn shared_handler(
    State(state): State<AppState>,
    AxumPath(code): AxumPath<String>,
) -> Response {
    match db::get_shared_item(&state.db, &code).await {
        Ok(Some((share, script, style, config))) => match share.item_type.as_str() {
            "Script" => match script {
                Some(s) => response_json(
                    StatusCode::OK,
                    json!({"type": "Script", "name": s.name, "content": s.content, "shared_by": share.user_id}),
                ),
                None => response_json(
                    StatusCode::NOT_FOUND,
                    json!({"error": "script content not found"}),
                ),
            },
            "Config" => match config {
                Some(c) => response_json(
                    StatusCode::OK,
                    json!({"type": "Config", "name": c.name, "content": c.content, "shared_by": share.user_id}),
                ),
                None => response_json(
                    StatusCode::NOT_FOUND,
                    json!({"error": "config content not found"}),
                ),
            },
            "Style" => match style {
                Some(s) => response_json(
                    StatusCode::OK,
                    json!({"type": "Style", "name": s.name, "content": s.content, "shared_by": share.user_id}),
                ),
                None => response_json(
                    StatusCode::NOT_FOUND,
                    json!({"error": "style content not found"}),
                ),
            },
            _ => response_json(
                StatusCode::BAD_REQUEST,
                json!({"error": "unknown item type"}),
            ),
        },
        Ok(None) => response_json(
            StatusCode::NOT_FOUND,
            json!({"error": "share code not found"}),
        ),
        Err(_) => response_json(
            StatusCode::INTERNAL_SERVER_ERROR,
            json!({"error": "database error"}),
        ),
    }
}

async fn import_to_account_handler(
    State(state): State<AppState>,
    axum::Json(req): axum::Json<ImportToAccountReq>,
) -> Result<Response, AppError> {
    let user = current_user(&state).await;

    let (share, script, style, config) = db::get_shared_item(&state.db, &req.share_code)
        .await?
        .ok_or_else(|| AppError(anyhow::anyhow!("share code not found")))?;

    let author_name = current_user(&state).await.username;
    let new_entry_id = db::next_entry_id(&state.db, user.id).await?;
    let now_ts = chrono::Utc::now().timestamp() as i32;

    let imported_name = match share.item_type.as_str() {
        "Script" => {
            let s = script.ok_or_else(|| AppError(anyhow::anyhow!("content missing")))?;
            let name = s.name;
            let content = s.content;
            db::create_script(&state.db, user.id, new_entry_id, &name).await?;
            db::update_script_content(&state.db, user.id, new_entry_id, &content).await?;
            name
        }
        "Config" => {
            let c = config.ok_or_else(|| AppError(anyhow::anyhow!("content missing")))?;
            let name = c.name;
            let content = c.content;
            db::create_config(&state.db, user.id, new_entry_id, &name).await?;
            db::update_config_content(&state.db, user.id, new_entry_id, &content).await?;
            local_storage::save_config_file(&name, &content);
            name
        }
        "Style" => {
            let s = style.ok_or_else(|| AppError(anyhow::anyhow!("content missing")))?;
            let name = s.name;
            let content = s.content;
            db::create_style(&state.db, user.id, new_entry_id, &name).await?;
            db::update_style_content(&state.db, user.id, new_entry_id, &content).await?;
            name
        }
        _ => {
            return Ok(response_json(
                StatusCode::BAD_REQUEST,
                json!({"error": "invalid type"}),
            ));
        }
    };

    db::create_log_entry(
        &state.db,
        user.id,
        new_entry_id,
        now_ts,
        &share.item_type,
        &author_name,
    )
    .await?;

    let live_insert_sent = if matches!(share.item_type.as_str(), "Script" | "Config" | "Style") {
        let sent = ws::push_live_insert(
            &state,
            new_entry_id as u32,
            now_ts as u32,
            &share.item_type,
            &imported_name,
            &author_name,
        )
        .await?;
        tracing::info!(
            "[HTTP] import_to_account live insert entry_id={} type={} sent_to={} sockets",
            new_entry_id,
            share.item_type,
            sent
        );
        sent
    } else {
        tracing::info!(
            "[HTTP] import_to_account skipped live insert for unsupported type={}",
            share.item_type
        );
        0
    };

    Ok(response_json(
        StatusCode::OK,
        json!({"status": "ok", "live_insert_sent": live_insert_sent}),
    ))
}

async fn fallback_handler(State(state): State<AppState>, req: Request) -> Response {
    let method = req.method().clone();
    let path = req.uri().path().to_owned();
    let query = req.uri().query().unwrap_or("").to_owned();
    let headers: Vec<(String, String)> = req
        .headers()
        .iter()
        .map(|(k, v)| (k.to_string(), v.to_str().unwrap_or("<binary>").to_owned()))
        .collect();

    tracing::info!(
        "[HTTP] {} {} query={} headers={:?}",
        method,
        path,
        query,
        headers
    );

    // Some clients ask for the current serial with a type=4 POST payload.
    if method == axum::http::Method::POST {
        let body_bytes = match axum::body::to_bytes(req.into_body(), 1024 * 1024).await {
            Ok(b) => b,
            Err(e) => {
                tracing::error!("[HTTP] Failed to read POST body: {e}");
                return express_404(method.as_str(), &path);
            }
        };

        // Try as JSON first
        if let Ok(val) = serde_json::from_slice::<serde_json::Value>(&body_bytes) {
            tracing::info!("[HTTP] POST JSON ({} bytes): {}", body_bytes.len(), val);
            if val.get("type").and_then(|t| t.as_i64()) == Some(4) {
                let serial = current_user(&state).await.serial;

                tracing::info!(
                    "[HTTP] -> GetSerial (type=4), returning serial ({} bytes)",
                    serial.len()
                );
                return Response::builder()
                    .status(StatusCode::OK)
                    .header(header::CONTENT_TYPE, "text/plain; charset=utf-8")
                    .body(Body::from(serial))
                    .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response());
            }
        } else {
            // Not JSON — log raw and try decrypt+decompress
            tracing::info!(
                "[HTTP] POST binary ({} bytes): {}",
                body_bytes.len(),
                hex_preview(&body_bytes, 64)
            );
            try_decrypt_and_log("[HTTP] POST", &body_bytes);
        }
    }

    tracing::warn!("[HTTP] -> 404 (unknown route)");
    express_404(method.as_str(), &path)
}

fn express_404(method: &str, path: &str) -> Response {
    let html = format!(
        "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n\
         <meta charset=\"utf-8\">\n<title>Error</title>\n\
         </head>\n<body>\n\
         <pre>Cannot {method} {path}</pre>\n\
         </body>\n</html>\n"
    );
    Response::builder()
        .status(StatusCode::NOT_FOUND)
        .header(header::CONTENT_SECURITY_POLICY, "default-src 'none'")
        .header("x-content-type-options", "nosniff")
        .header(header::CONTENT_TYPE, "text/html; charset=utf-8")
        .body(Body::from(html))
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

fn default_avatar() -> Response {
    avatar_response(DEFAULT_AVATAR_PNG.to_vec())
}

fn avatar_response(bytes: Vec<u8>) -> Response {
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "image/png")
        .header(
            header::CACHE_CONTROL,
            "no-store, no-cache, must-revalidate, max-age=0",
        )
        .header(header::PRAGMA, "no-cache")
        .header(header::EXPIRES, "0")
        .body(Body::from(bytes))
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

fn response_json(status: StatusCode, value: serde_json::Value) -> Response {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json")
        .body(Body::from(value.to_string()))
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

#[derive(Deserialize)]
struct ShareReq {
    item_type: String,
    item_id: String,
}

#[derive(Deserialize)]
struct UnshareReq {
    share_code: String,
}

#[derive(Deserialize)]
struct ImportToAccountReq {
    share_code: String,
}

pub(crate) fn share_url_from_headers(headers: &HeaderMap, share_code: &str) -> String {
    if let Ok(base_url) =
        std::env::var("PUBLIC_SHARE_BASE_URL").or_else(|_| std::env::var("PUBLIC_BASE_URL"))
    {
        let base_url = base_url.trim().trim_end_matches('/');
        if !base_url.is_empty() {
            return format!("{}/?share={}", base_url, url_encode_component(share_code));
        }
    }

    let host = header_string(headers, "x-forwarded-host")
        .or_else(|| header_string(headers, "host"))
        .unwrap_or_else(|| format!("localhost:{}", config::HTTPS_PORT));
    let proto = header_string(headers, "x-forwarded-proto").unwrap_or_else(|| {
        if host.ends_with(&format!(":{}", config::HTTP_PORT)) {
            "http".to_string()
        } else {
            "https".to_string()
        }
    });

    format!(
        "{}://{}/?share={}",
        proto,
        website_host(&host),
        url_encode_component(share_code)
    )
}

fn header_string(headers: &HeaderMap, name: &'static str) -> Option<String> {
    headers
        .get(name)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.split(',').next())
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(ToOwned::to_owned)
}

fn website_host(host: &str) -> String {
    let ws_port = format!(":{}", config::WS_PORT);
    if let Some(base) = host.strip_suffix(&ws_port) {
        format!("{}:{}", base, config::HTTPS_PORT)
    } else {
        host.to_string()
    }
}

fn url_encode_component(value: &str) -> String {
    value
        .bytes()
        .flat_map(|byte| match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                vec![byte as char]
            }
            _ => format!("%{byte:02X}").chars().collect(),
        })
        .collect()
}

async fn current_user(state: &AppState) -> crate::models::UserRow {
    state.single_user.read().await.clone()
}

async fn replace_current_user(state: &AppState, user: crate::models::UserRow) {
    *state.single_user.write().await = user;
}

// ── Decryption helpers ──

fn hex_preview(data: &[u8], max_bytes: usize) -> String {
    let preview: String = data
        .iter()
        .take(max_bytes)
        .map(|b| format!("{:02x}", b))
        .collect::<Vec<_>>()
        .join(" ");
    if data.len() > max_bytes {
        format!("{}... ({} bytes total)", preview, data.len())
    } else {
        preview
    }
}

fn try_decrypt_and_log(prefix: &str, data: &[u8]) {
    use nl_parser::module::Module;
    use nl_parser::pipeline;

    // Try AES decrypt only
    match pipeline::decrypt(data) {
        Ok(decrypted) => {
            tracing::info!(
                "{} decrypted ({} bytes): {}",
                prefix,
                decrypted.len(),
                hex_preview(&decrypted, 64)
            );
            // Try LZ4 decompress
            match pipeline::decompress(&decrypted) {
                Ok(decompressed) => {
                    tracing::info!("{} decompressed ({} bytes)", prefix, decompressed.len());
                    // Try as UTF-8 text
                    if let Ok(text) = std::str::from_utf8(&decompressed) {
                        let preview = if text.len() > 1000 {
                            format!("{}...", &text[..1000])
                        } else {
                            text.to_string()
                        };
                        tracing::info!("{} plaintext: {}", prefix, preview);
                    }
                    // Try as FlatBuffer module
                    match Module::from_flatbuffer(&decompressed) {
                        Ok(module) => {
                            tracing::info!(
                                "{} parsed as Module: version={} author={} token={} checksum={} enabled={} config_log={} script_log={} languages={}",
                                prefix,
                                module.version,
                                module.author,
                                module.auth_token,
                                module.checksum,
                                module.enabled,
                                module.config_log.len(),
                                module.script_log.len(),
                                module.languages.len(),
                            );
                        }
                        Err(_) => {
                            tracing::debug!(
                                "{} not a valid Module FlatBuffer, raw hex: {}",
                                prefix,
                                hex_preview(&decompressed, 128)
                            );
                        }
                    }
                }
                Err(_) => {
                    // Not LZ4 — just show decrypted as text or hex
                    if let Ok(text) = std::str::from_utf8(&decrypted) {
                        let preview = if text.len() > 1000 {
                            format!("{}...", &text[..1000])
                        } else {
                            text.to_string()
                        };
                        tracing::info!("{} decrypted text: {}", prefix, preview);
                    }
                }
            }
        }
        Err(_) => {
            tracing::debug!("{} not AES-encrypted (decrypt failed)", prefix);
        }
    }
}
