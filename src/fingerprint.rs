use sha2::{Sha256, Digest};
use crate::network;

pub fn generate() -> String {
    let mut parts: Vec<String> = Vec::new();

    // CPU ProcessorId via wmic
    if let Some(id) = wmic_get("cpu", "ProcessorId") {
        parts.push(id);
    }

    // Disk serial number (Index=0)
    if let Some(sn) = wmic_get_where("diskdrive", "SerialNumber", "Index=0") {
        parts.push(sn);
    }

    // Primary NIC MAC address
    if let Some(mac) = network::get_primary_mac() {
        parts.push(mac);
    }

    if parts.is_empty() {
        return "unknown".to_string();
    }

    let raw = parts.join("|");
    let mut hasher = Sha256::new();
    hasher.update(raw.as_bytes());
    let hash = hasher.finalize();
    hash.iter().map(|b| format!("{:02x}", b)).take(16).collect()
}

fn run_wmic(args: &[&str]) -> Option<String> {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x08000000;
    let output = std::process::Command::new("wmic")
        .args(args)
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .ok()?;
    if !output.status.success() { return None; }
    Some(crate::network::decode_bytes(&output.stdout))
}

fn wmic_get(class: &str, field: &str) -> Option<String> {
    let out = run_wmic(&[class, "get", field])?;
    // Skip header line, find first non-empty value
    for line in out.lines().skip(1) {
        let trimmed = line.trim();
        if !trimmed.is_empty() {
            return Some(trimmed.to_string());
        }
    }
    None
}

fn wmic_get_where(class: &str, field: &str, condition: &str) -> Option<String> {
    let out = run_wmic(&[class, "where", condition, "get", field])?;
    for line in out.lines().skip(1) {
        let trimmed = line.trim();
        if !trimmed.is_empty() {
            return Some(trimmed.to_string());
        }
    }
    None
}
