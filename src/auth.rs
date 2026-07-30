use crate::config::AppConfig;
use std::time::Duration;

pub struct AuthResult {
    pub ok: bool,
    pub message: String,
    pub token: String,
}

fn build_client() -> reqwest::blocking::Client {
    reqwest::blocking::Client::builder()
        .danger_accept_invalid_certs(true)
        .timeout(Duration::from_secs(10))
        .build()
        .expect("Failed to build HTTP client")
}

/// Check device authorization with the server.
/// Returns AuthResult with token on success.
pub fn check_auth(device_id: &str, server_url: &str) -> Result<AuthResult, String> {
    let client = build_client();
    let url = format!("{}/api/auth/check", server_url);
    let body = serde_json::json!({ "device_id": device_id });
    let body_str = serde_json::to_string(&body).map_err(|e| e.to_string())?;

    let resp = client
        .post(&url)
        .header("Content-Type", "application/json")
        .body(body_str)
        .send()
        .map_err(|e| format!("Connection failed: {}", e))?;

    let json: serde_json::Value = resp.json().map_err(|e| format!("Parse error: {}", e))?;

    let ok = json.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
    let message = json.get("message").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let token = json
        .get("data")
        .and_then(|d| d.get("token"))
        .and_then(|t| t.as_str())
        .unwrap_or("")
        .to_string();

    Ok(AuthResult { ok, message, token })
}

/// Silently verify the cached token with the server.
/// Returns true if the device is still authorized.
pub fn verify_token_silent(device_id: &str, server_url: &str, config: &mut AppConfig) -> bool {
    match check_auth(device_id, server_url) {
        Ok(result) => {
            if result.ok {
                config.auth_token = result.token;
                config.refresh_token_expiry();
                config.save();
                true
            } else {
                config.auth_token.clear();
                config.auth_expire.clear();
                config.save();
                false
            }
        }
        Err(_) => {
            // Network error — trust cached token if still within expiry
            config.has_valid_token_cache()
        }
    }
}
