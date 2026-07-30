use std::os::windows::process::CommandExt;
use std::process::Command;
use std::sync::Mutex;
use std::time::{Duration, Instant};

const CREATE_NO_WINDOW: u32 = 0x08000000;

static ROUTE_CACHE: Mutex<Option<(Instant, String)>> = Mutex::new(None);

pub fn decode_bytes(bytes: &[u8]) -> String {
    if let Ok(s) = std::str::from_utf8(bytes) {
        return s.to_string();
    }
    let (cow, _, _) = encoding_rs::GBK.decode(bytes);
    cow.into_owned()
}

pub fn run_cmd(prog: &str, args: &[&str]) -> String {
    match Command::new(prog)
        .args(args)
        .creation_flags(CREATE_NO_WINDOW)
        .output()
    {
        Ok(o) => decode_bytes(&o.stdout),
        Err(_) => String::new(),
    }
}

pub fn run_route(args: &str) -> String {
    run_cmd("route.exe", &[args])
}

#[derive(Clone, Debug)]
pub struct NicInfo {
    pub name: String,
    pub description: String,
    pub if_index: u32,
    pub mac: String,
    pub ip: String,
    pub gateway: String,
    pub is_up: bool,
}

const VIRTUAL_KEYWORDS: &[&str] = &["TAP", "VPN", "WireGuard", "Tunnel", "Virtual", "Hyper-V", "EricVPN"];
const TAP_KEYWORDS: &[&str] = &["TAP", "VPN", "Virtual", "Hyper-V", "EricVPN"];
const IGNORE_KEYWORDS: &[&str] = &["WireGuard"];

pub fn is_virtual(desc: &str) -> bool {
    VIRTUAL_KEYWORDS.iter().any(|k| desc.to_lowercase().contains(&k.to_lowercase()))
}

pub fn is_tap(desc: &str) -> bool {
    let lower = desc.to_lowercase();
    TAP_KEYWORDS.iter().any(|k| lower.contains(&k.to_lowercase()))
        && !IGNORE_KEYWORDS.iter().any(|k| lower.contains(&k.to_lowercase()))
}

pub fn get_adapters() -> Vec<NicInfo> {
    // Get NIC info from wmic
    let nic_output = run_wmic_list("nic", &["NetConnectionID", "Description", "InterfaceIndex", "MACAddress", "NetEnabled", "NetConnectionStatus"]);
    let cfg_output = run_wmic_list("nicconfig", &["InterfaceIndex", "IPAddress", "DefaultIPGateway", "IPEnabled"]);

    // Build a map: ifIndex -> (ip, gateway) from nicconfig
    let mut ip_map: std::collections::HashMap<u32, (String, String)> = std::collections::HashMap::new();
    for record in cfg_output {
        let if_idx = record.get("InterfaceIndex").and_then(|s| s.parse::<u32>().ok());
        if let Some(idx) = if_idx {
            let ip = record.get("IPAddress").map(|s| extract_ip(s)).unwrap_or_default();
            let gw = record.get("DefaultIPGateway").map(|s| extract_ip(s)).unwrap_or_default();
            ip_map.insert(idx, (ip, gw));
        }
    }

    // Build NicInfo list from nic records
    let mut result = Vec::new();
    for record in nic_output {
        let if_idx = match record.get("InterfaceIndex").and_then(|s| s.parse::<u32>().ok()) {
            Some(v) => v,
            None => continue,
        };
        let net_enabled = record.get("NetEnabled").map(|s| s.eq_ignore_ascii_case("TRUE")).unwrap_or(false);
        let status = record.get("NetConnectionStatus").and_then(|s| s.trim().parse::<u32>().ok()).unwrap_or(0);
        // Status 2 = Connected
        let is_up = net_enabled && status == 2;

        let name = record.get("NetConnectionID").cloned().unwrap_or_default();
        let description = record.get("Description").cloned().unwrap_or_default();
        let mac = record.get("MACAddress").cloned().unwrap_or_default().replace(":", "").to_uppercase();

        let (ip, gateway) = ip_map.get(&if_idx).cloned().unwrap_or_default();

        // Skip loopback (ifIndex 1) and adapters with no name
        if if_idx == 1 || name.is_empty() {
            continue;
        }

        result.push(NicInfo {
            name,
            description,
            if_index: if_idx,
            mac,
            ip,
            gateway,
            is_up,
        });
    }

    result
}

pub fn get_primary_mac() -> Option<String> {
    let adapters = get_adapters();
    for nic in &adapters {
        if nic.is_up && !is_virtual(&nic.description) && !nic.mac.is_empty() {
            return Some(nic.mac.replace(":", "").replace("-", "").to_uppercase());
        }
    }
    None
}

pub fn get_main_nic(adapters: &[NicInfo]) -> Option<&NicInfo> {
    adapters.iter().find(|n| n.is_up && !is_virtual(&n.description) && !n.gateway.is_empty())
}

pub fn get_managed_adapters(adapters: &[NicInfo]) -> Vec<&NicInfo> {
    adapters.iter().filter(|n| n.is_up && is_tap(&n.description)).collect()
}

/// Get cached route table (cached for 2 seconds)
pub fn get_route_table() -> String {
    if let Ok(mut cache) = ROUTE_CACHE.lock() {
        if let Some((time, table)) = cache.as_ref() {
            if time.elapsed() < Duration::from_secs(2) {
                return table.clone();
            }
        }
        let table = run_route("print -4");
        *cache = Some((Instant::now(), table.clone()));
        return table;
    }
    run_route("print -4")
}

/// Convert CIDR (e.g. "0.0.0.0/1") to (destination, netmask) for route print matching
pub fn cidr_to_route_print(cidr: &str) -> (String, String) {
    let parts: Vec<&str> = cidr.split('/').collect();
    if parts.len() != 2 {
        return (cidr.to_string(), "0.0.0.0".to_string());
    }
    let prefix_len: u32 = parts[1].parse().unwrap_or(0);
    let ip_bytes: Vec<u8> = parts[0].split('.').filter_map(|s| s.parse().ok()).collect();
    if ip_bytes.len() != 4 {
        return (parts[0].to_string(), "0.0.0.0".to_string());
    }

    let mask_val: u32 = if prefix_len == 0 { 0 } else { 0xFFFFFFFFu32 << (32 - prefix_len) };
    let mask_bytes = mask_val.to_be_bytes();
    let dest_bytes: Vec<u8> = ip_bytes.iter().zip(mask_bytes.iter()).map(|(ip, m)| ip & m).collect();

    let dest = dest_bytes.iter().map(|b| b.to_string()).collect::<Vec<_>>().join(".");
    let mask = mask_bytes.iter().map(|b| b.to_string()).collect::<Vec<_>>().join(".");
    (dest, mask)
}

/// Check if a CIDR route exists in the route table (any interface)
pub fn route_exists(cidr: &str) -> bool {
    let table = get_route_table();
    let (dest, mask) = cidr_to_route_print(cidr);
    for line in table.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() >= 5 && parts[0] == dest && parts[1] == mask {
            return true;
        }
    }
    false
}

/// Check if a CIDR route exists on a specific interface (by interface IP)
pub fn route_exists_on_interface(cidr: &str, if_ip: &str) -> bool {
    let table = get_route_table();
    let (dest, mask) = cidr_to_route_print(cidr);
    for line in table.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() >= 5 && parts[0] == dest && parts[1] == mask {
            if if_ip.is_empty() || parts[3] == if_ip {
                return true;
            }
        }
    }
    false
}

/// Get the gateway for a TAP interface (by interface IP)
pub fn get_tap_gateway(if_ip: &str) -> Option<String> {
    let table = get_route_table();
    for line in table.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 5 { continue; }
        if !if_ip.is_empty() && parts[3] != if_ip { continue; }
        let gw = parts[2];
        if gw != "On-link" && gw.parse::<std::net::Ipv4Addr>().is_ok() && gw != "0.0.0.0" {
            return Some(gw.to_string());
        }
    }
    None
}

/// Get the metric of the default route on a specific interface (by IP)
pub fn get_adapter_metric(if_ip: &str) -> i32 {
    let table = get_route_table();
    for line in table.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 5 { continue; }
        if parts[0] == "0.0.0.0" && parts[1] == "0.0.0.0" {
            if !if_ip.is_empty() && parts[3] != if_ip { continue; }
            if let Ok(m) = parts[4].parse::<i32>() {
                return m;
            }
        }
    }
    -1
}

// === wmic parsing helpers ===

fn run_wmic_list(class: &str, fields: &[&str]) -> Vec<std::collections::HashMap<String, String>> {
    let field_str = fields.join(",");
    let output = run_cmd("wmic", &[class, "get", field_str.as_str(), "/format:list"]);

    let mut records = Vec::new();
    let mut current: std::collections::HashMap<String, String> = std::collections::HashMap::new();

    for line in output.lines() {
        let line = line.trim();
        if line.is_empty() {
            if !current.is_empty() {
                records.push(std::mem::take(&mut current));
            }
            continue;
        }
        if let Some(eq_pos) = line.find('=') {
            let key = line[..eq_pos].trim().to_string();
            let value = line[eq_pos + 1..].trim().to_string();
            current.insert(key, value);
        }
    }
    if !current.is_empty() {
        records.push(current);
    }
    records
}

/// Extract IP address from wmic array format: {"192.168.1.100"} or {192.168.1.100}
fn extract_ip(s: &str) -> String {
    let s = s.trim();
    // Remove braces and quotes
    let s = s.trim_start_matches('{').trim_end_matches('}');
    let s = s.trim_matches('"');
    // wmic returns multiple IPs as {"ip1","ip2"} — take the first IPv4
    for part in s.split(',') {
        let part = part.trim().trim_matches('"');
        if part.parse::<std::net::Ipv4Addr>().is_ok() {
            return part.to_string();
        }
    }
    // If no IPv4 found, return first non-empty part
    let first = s.split(',').next().unwrap_or("").trim().trim_matches('"');
    first.to_string()
}
