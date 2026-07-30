use serde::{Deserialize, Serialize};
use std::collections::HashMap;

const DEFAULT_AUTH_SERVER: &str = "https://pve.lu56.top:12233";

#[derive(Serialize, Deserialize, Clone)]
pub struct AppConfig {
    #[serde(default = "default_version", alias = "AppVersion")]
    pub app_version: String,

    #[serde(default = "default_check_interval", alias = "CheckIntervalSeconds")]
    pub check_interval_seconds: u32,
    #[serde(default = "default_tap_metric", alias = "TapMetric")]
    pub tap_metric: u32,
    #[serde(default = "default_main_metric", alias = "MainMetric")]
    pub main_metric: u32,

    #[serde(default = "default_private_nets", alias = "PrivateNets")]
    pub private_nets: Vec<String>,

    #[serde(default, alias = "CustomRoutes")]
    pub custom_routes: HashMap<String, String>,

    #[serde(default, alias = "AutoStart")]
    pub auto_start: bool,
    #[serde(default, alias = "MinimizeToTray")]
    pub minimize_to_tray: bool,
    #[serde(default, alias = "ShowNotification")]
    pub show_notification: bool,

    #[serde(default, alias = "BarkEnabled")]
    pub bark_enabled: bool,
    #[serde(default, alias = "BarkServer")]
    pub bark_server: String,
    #[serde(default, alias = "BarkDeviceKey")]
    pub bark_device_key: String,
    #[serde(default = "default_bark_title", alias = "BarkTitle")]
    pub bark_title: String,
    #[serde(default, alias = "BarkSound")]
    pub bark_sound: String,

    #[serde(default = "default_true", alias = "AutoUpdateCheck")]
    pub auto_update_check: bool,
    #[serde(default = "default_update_repo", alias = "UpdateRepo")]
    pub update_repo: String,

    #[serde(default, alias = "AuthServer")]
    pub auth_server: String,
    #[serde(default, alias = "AuthToken")]
    pub auth_token: String,
    #[serde(default, alias = "AuthExpire")]
    pub auth_expire: String,

    #[serde(default, alias = "TotalFixes")]
    pub total_fixes: u64,
    #[serde(default, with = "optional_datetime", alias = "LastFixTime")]
    pub last_fix_time: Option<String>,
}

fn default_version() -> String { "4.0.0".to_string() }
fn default_check_interval() -> u32 { 4 }
fn default_tap_metric() -> u32 { 50 }
fn default_main_metric() -> u32 { 16 }
fn default_true() -> bool { true }
fn default_bark_title() -> String { "GatewayPolicy".to_string() }
fn default_update_repo() -> String { "lu56/gp-update".to_string() }

fn default_private_nets() -> Vec<String> {
    vec![
        "10.0.0.0/8".into(),
        "172.16.0.0/12".into(),
        "192.168.0.0/16".into(),
        "100.64.0.0/10".into(),
    ]
}

mod optional_datetime {
    use serde::{Deserialize, Deserializer, Serializer};
    pub fn serialize<S: Serializer>(v: &Option<String>, s: S) -> Result<S::Ok, S::Error> {
        match v {
            Some(t) => s.serialize_str(t),
            None => s.serialize_none(),
        }
    }
    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
        let opt: Option<serde_json::Value> = Option::deserialize(d)?;
        match opt {
            None => Ok(None),
            Some(serde_json::Value::String(s)) => {
                if s.is_empty() { Ok(None) } else { Ok(Some(s)) }
            }
            Some(serde_json::Value::Null) => Ok(None),
            Some(serde_json::Value::Bool(false)) => Ok(None),
            Some(other) => Ok(Some(other.to_string())),
        }
    }
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            app_version: default_version(),
            check_interval_seconds: default_check_interval(),
            tap_metric: default_tap_metric(),
            main_metric: default_main_metric(),
            private_nets: default_private_nets(),
            custom_routes: HashMap::new(),
            auto_start: false,
            minimize_to_tray: true,
            show_notification: true,
            bark_enabled: false,
            bark_server: String::new(),
            bark_device_key: String::new(),
            bark_title: default_bark_title(),
            bark_sound: String::new(),
            auto_update_check: true,
            update_repo: default_update_repo(),
            auth_server: String::new(),
            auth_token: String::new(),
            auth_expire: String::new(),
            total_fixes: 0,
            last_fix_time: None,
        }
    }
}

impl AppConfig {
    pub fn config_path() -> std::path::PathBuf {
        let exe = std::env::current_exe().unwrap_or_else(|_| std::path::PathBuf::from("GatewayPolicy.exe"));
        exe.with_file_name("config.json")
    }

    pub fn load() -> AppConfig {
        let path = Self::config_path();
        if !path.exists() {
            return AppConfig::default();
        }
        match std::fs::read_to_string(&path) {
            Ok(json) => {
                let cfg: AppConfig = serde_json::from_str(&json).unwrap_or_else(|e| {
                    eprintln!("Config parse error: {e}");
                    partial_load(&json).unwrap_or_default()
                });
                // Safety net: if auth token wasn't loaded (e.g. unknown key format),
                // supplement from partial_load
                if cfg.auth_token.is_empty() {
                    if let Some(p) = partial_load(&json) {
                        let mut cfg = cfg;
                        cfg.auth_token = p.auth_token;
                        cfg.auth_expire = p.auth_expire;
                        return cfg;
                    }
                }
                cfg
            }
            Err(_) => AppConfig::default(),
        }
    }

    pub fn save(&self) {
        let path = Self::config_path();
        if let Ok(json) = serde_json::to_string_pretty(self) {
            let _ = std::fs::write(&path, json);
        }
    }

    pub fn effective_auth_server(&self) -> &str {
        if !self.auth_server.is_empty() {
            &self.auth_server
        } else {
            DEFAULT_AUTH_SERVER
        }
    }

    pub fn has_valid_token_cache(&self) -> bool {
        if self.auth_token.is_empty() || self.auth_expire.is_empty() {
            return false;
        }
        if let Ok(exp) = chrono::NaiveDateTime::parse_from_str(&self.auth_expire, "%Y-%m-%d %H:%M:%S") {
            let now = chrono::Local::now().naive_local();
            return now < exp;
        }
        false
    }

    pub fn refresh_token_expiry(&mut self) {
        let exp = chrono::Local::now() + chrono::Duration::days(2);
        self.auth_expire = exp.format("%Y-%m-%d %H:%M:%S").to_string();
    }
}

fn partial_load(json: &str) -> Option<AppConfig> {
    let v: serde_json::Value = serde_json::from_str(json).ok()?;
    let mut cfg = AppConfig::default();
    if let Some(t) = v.get("AuthToken").and_then(|v| v.as_str()) {
        cfg.auth_token = t.to_string();
    }
    if let Some(e) = v.get("AuthExpire").and_then(|v| v.as_str()) {
        cfg.auth_expire = e.to_string();
    }
    if let Some(s) = v.get("AuthServer").and_then(|v| v.as_str()) {
        if !s.is_empty() {
            cfg.auth_server = s.to_string();
        }
    }
    Some(cfg)
}
