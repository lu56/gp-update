use std::os::windows::process::CommandExt;
use std::time::Duration;

pub struct UpdateInfo {
    pub tag_name: String,
    pub asset_name: String,
    pub download_url: String,
}

/// Check GitHub for the latest release. Returns UpdateInfo if a new version is available.
pub fn check_latest(repo: &str, current_version: &str) -> Result<Option<UpdateInfo>, String> {
    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(|e| e.to_string())?;

    let url = format!("https://api.github.com/repos/{}/releases/latest", repo);
    let resp = client
        .get(&url)
        .header("User-Agent", "GatewayPolicy")
        .send()
        .map_err(|e| format!("Request failed: {}", e))?;

    let json: serde_json::Value = resp.json().map_err(|e| format!("Parse error: {}", e))?;

    let tag_name = json.get("tag_name").and_then(|v| v.as_str()).unwrap_or("").to_string();
    if tag_name.is_empty() {
        return Ok(None);
    }

    let release_ver = tag_name.trim_start_matches('v').trim_start_matches('V');
    if !is_newer_version(release_ver, current_version) {
        return Ok(None);
    }

    // Find .exe asset
    let mut download_url = String::new();
    let mut asset_name = String::new();
    if let Some(assets) = json.get("assets").and_then(|v| v.as_array()) {
        for asset in assets {
            let name = asset.get("name").and_then(|v| v.as_str()).unwrap_or("");
            if name.to_lowercase().ends_with(".exe") {
                download_url = asset.get("browser_download_url").and_then(|v| v.as_str()).unwrap_or("").to_string();
                asset_name = name.to_string();
                break;
            }
        }
    }

    if download_url.is_empty() {
        return Err(format!("Found new version {} but no .exe asset found", tag_name));
    }

    Ok(Some(UpdateInfo { tag_name, asset_name, download_url }))
}

/// Download and install the update. Returns Ok(()) on success.
/// The caller should exit the app after this returns.
pub fn download_and_install(url: &str, file_name: &str) -> Result<(), String> {
    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(300))
        .build()
        .map_err(|e| e.to_string())?;

    let temp_dir = std::env::temp_dir().join("GatewayPolicy_Update");
    std::fs::create_dir_all(&temp_dir).map_err(|e| e.to_string())?;
    let temp_file = temp_dir.join(file_name);

    let data = client
        .get(url)
        .header("User-Agent", "GatewayPolicy")
        .send()
        .map_err(|e| format!("Download failed: {}", e))?
        .bytes()
        .map_err(|e| format!("Download read error: {}", e))?;

    std::fs::write(&temp_file, &data).map_err(|e| format!("Write failed: {}", e))?;

    let current_exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let current_path = current_exe.to_string_lossy().replace('\\', "/");
    let temp_path = temp_file.to_string_lossy().replace('\\', "/");
    let updater_bat = temp_dir.join("updater.bat");

    // BAT script must use pure English (GBK encoding issues)
    let script = format!(
        r#"@echo off
echo GatewayPolicy Updater
timeout /t 2 /nobreak >nul
:retry
del /f /q "{current}" 2>nul
if exist "{current}" (
    timeout /t 1 /nobreak >nul
    goto retry
)
copy /y "{temp}" "{current}" >nul
start "" "{current}"
del /f /q "%~f0"
"#,
        current = current_path,
        temp = temp_path,
    );

    std::fs::write(&updater_bat, script).map_err(|e| e.to_string())?;

    std::process::Command::new("cmd")
        .arg("/c")
        .arg(&updater_bat)
        .creation_flags(0x08000000) // CREATE_NO_WINDOW
        .spawn()
        .map_err(|e| format!("Failed to start updater: {}", e))?;

    Ok(())
}

fn is_newer_version(remote: &str, local: &str) -> bool {
    let r = parse_version(remote);
    let l = parse_version(local);
    if r[0] != l[0] { return r[0] > l[0]; }
    if r[1] != l[1] { return r[1] > l[1]; }
    r[2] > l[2]
}

fn parse_version(ver: &str) -> [u32; 3] {
    let parts: Vec<&str> = ver.trim_start_matches('v').trim_start_matches('V').split('.').collect();
    let mut result = [0u32; 3];
    for i in 0..3 {
        if i < parts.len() {
            result[i] = parts[i].parse().unwrap_or(0);
        }
    }
    result
}
