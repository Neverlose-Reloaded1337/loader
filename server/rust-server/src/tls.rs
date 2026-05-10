use axum_server::tls_rustls::RustlsConfig;

static CERT_PEM: &[u8] = include_bytes!("../certs/cert.pem");
static KEY_PEM: &[u8] = include_bytes!("../certs/key.pem");

pub async fn load_rustls_config() -> anyhow::Result<RustlsConfig> {
    RustlsConfig::from_pem(CERT_PEM.to_vec(), KEY_PEM.to_vec())
        .await
        .map_err(Into::into)
}
