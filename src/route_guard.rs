use crate::config::AppConfig;
use crate::logger::Logger;
use crate::network;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum MonitorState {
    Stopped,
    Running,
    Fixing,
}

const COUNTER_ROUTES: &[&str] = &["0.0.0.0/2", "64.0.0.0/2", "128.0.0.0/2", "192.0.0.0/2"];

struct GuardInner {
    main_if_index: u32,
    main_if_ip: String,
    main_gateway: String,
    main_nic_name: String,
    fixing: bool,
    was_hijacked: bool,
    steady_state: bool,
    last_nic_log: String,
    state: MonitorState,
    running: bool,
}

pub struct RouteGuard {
    inner: Arc<Mutex<GuardInner>>,
    config: Arc<Mutex<AppConfig>>,
    logger: Arc<Logger>,
}

#[derive(Clone)]
pub struct GuardStatus {
    pub state: MonitorState,
    pub is_hijacked: bool,
    pub main_nic: String,
    pub tap_nic: String,
    pub total_fixes: u64,
    pub last_fix: String,
}

impl RouteGuard {
    pub fn new(config: Arc<Mutex<AppConfig>>, logger: Arc<Logger>) -> Self {
        RouteGuard {
            inner: Arc::new(Mutex::new(GuardInner {
                main_if_index: 0,
                main_if_ip: String::new(),
                main_gateway: String::new(),
                main_nic_name: String::new(),
                fixing: false,
                was_hijacked: false,
                steady_state: false,
                last_nic_log: String::new(),
                state: MonitorState::Stopped,
                running: false,
            })),
            config,
            logger,
        }
    }

    pub fn get_status(&self) -> GuardStatus {
        let inner = self.inner.lock().unwrap();
        let config = self.config.lock().unwrap();
        GuardStatus {
            state: inner.state,
            is_hijacked: inner.was_hijacked,
            main_nic: inner.main_nic_name.clone(),
            tap_nic: {
                let adapters = network::get_adapters();
                let taps = network::get_managed_adapters(&adapters);
                taps.first().map(|t| t.name.clone()).unwrap_or_default()
            },
            total_fixes: config.total_fixes,
            last_fix: config.last_fix_time.clone().unwrap_or_default(),
        }
    }

    pub fn start(&self) {
        let mut inner = self.inner.lock().unwrap();
        if inner.running {
            return;
        }
        inner.running = true;
        inner.state = MonitorState::Running;
        drop(inner);

        self.cache_main_nic();
        self.logger.info("Monitor started (v4.0.0: /2 counter-route strategy, Rust)");

        // Spawn monitoring thread
        let inner = self.inner.clone();
        let config = self.config.clone();
        let logger = self.logger.clone();

        thread::spawn(move || {
            thread::sleep(Duration::from_secs(2)); // initial delay
            loop {
                let should_run = {
                    let g = inner.lock().unwrap();
                    g.running
                };
                if !should_run { break; }

                let check_interval = config.lock().unwrap().check_interval_seconds;
                do_check(&inner, &config, &logger);
                thread::sleep(Duration::from_secs(check_interval.max(1) as u64));
            }
        });
    }

    pub fn stop(&self) {
        let mut inner = self.inner.lock().unwrap();
        inner.running = false;
        inner.state = MonitorState::Stopped;
        drop(inner);
        self.logger.info("Monitor stopped");
    }

    pub fn do_check_now(&self) {
        do_check(&self.inner, &self.config, &self.logger);
    }

    pub fn restore_network(&self) {
        self.stop();

        let mut removed = 0;
        for prefix in COUNTER_ROUTES {
            network::run_route(&format!("delete {}", prefix));
            removed += 1;
        }

        let config = self.config.lock().unwrap();
        for net in &config.private_nets {
            network::run_route(&format!("delete {}", net));
            removed += 1;
        }
        for key in config.custom_routes.keys() {
            network::run_route(&format!("delete {}", key));
            removed += 1;
        }
        drop(config);

        let mut inner = self.inner.lock().unwrap();
        inner.was_hijacked = false;
        inner.steady_state = false;
        drop(inner);

        self.logger.info(&format!("Network restored — removed {} routes", removed));
    }

    fn cache_main_nic(&self) {
        let adapters = network::get_adapters();
        if let Some(nic) = network::get_main_nic(&adapters) {
            let nic_info = format!("{}|{}|{}", nic.name, nic.if_index, nic.gateway);
            let mut inner = self.inner.lock().unwrap();
            inner.main_if_index = nic.if_index;
            inner.main_if_ip = nic.ip.clone();
            inner.main_gateway = nic.gateway.clone();
            inner.main_nic_name = nic.name.clone();

            if inner.last_nic_log != nic_info {
                inner.last_nic_log = nic_info;
                drop(inner);
                self.logger.info(&format!("Main NIC: {} (if={}, gw={})", nic.name, nic.if_index, nic.gateway));
            }
        } else {
            self.logger.error("Could not detect main NIC");
        }
    }
}

fn do_check(inner: &Arc<Mutex<GuardInner>>, config: &Arc<Mutex<AppConfig>>, logger: &Arc<Logger>) {
    {
        let g = inner.lock().unwrap();
        if g.fixing || !g.running {
            return;
        }
    }

    // Refresh main NIC info
    let adapters = network::get_adapters();
    if let Some(nic) = network::get_main_nic(&adapters) {
        let nic_info = format!("{}|{}|{}", nic.name, nic.if_index, nic.gateway);
        let mut g = inner.lock().unwrap();
        g.main_if_index = nic.if_index;
        g.main_if_ip = nic.ip.clone();
        g.main_gateway = nic.gateway.clone();
        g.main_nic_name = nic.name.clone();
        let should_log = g.last_nic_log != nic_info;
        if should_log {
            g.last_nic_log = nic_info.clone();
        }
        drop(g);
        if should_log {
            logger.info(&format!("Main NIC: {} (if={}, gw={})", nic.name, nic.if_index, nic.gateway));
        }
    }

    // Check for def1 hijack
    let mut hijacked = false;
    if network::route_exists("0.0.0.0/1") || network::route_exists("128.0.0.0/1") {
        hijacked = true;
        let mut g = inner.lock().unwrap();
        if !g.was_hijacked {
            g.was_hijacked = true;
            drop(g);
            logger.info("Detected VPN def1 hijack route (0.0.0.0/1 or 128.0.0.0/1)");
        }
    } else {
        let mut g = inner.lock().unwrap();
        if g.was_hijacked {
            g.was_hijacked = false;
            g.steady_state = false;
            drop(g);
            logger.info("VPN hijack cleared — no longer detected");
        }
    }

    // Secondary check: TAP has low metric default route
    let taps = network::get_managed_adapters(&adapters);
    for tap in &taps {
        let metric = network::get_adapter_metric(&tap.ip);
        if metric > 0 && metric < 10 {
            hijacked = true;
            let mut g = inner.lock().unwrap();
            if !g.was_hijacked {
                g.was_hijacked = true;
                drop(g);
                logger.info(&format!("TAP adapter {} has low metric default route (metric={})", tap.name, metric));
            }
        }
    }

    if hijacked {
        apply_counter_routes(inner, config, logger);
    }
}

fn apply_counter_routes(inner: &Arc<Mutex<GuardInner>>, config: &Arc<Mutex<AppConfig>>, logger: &Arc<Logger>) {
    let g = inner.lock().unwrap();
    if g.fixing { return; }
    let main_gw = g.main_gateway.clone();
    let main_if = g.main_if_index;
    let main_if_ip = g.main_if_ip.clone();
    drop(g);

    let adapters = network::get_adapters();
    let taps = network::get_managed_adapters(&adapters);

    // Phase 1: Check what needs fixing
    let mut need_fix = false;
    let mut routes_to_add: Vec<(String, String, u32, u32, &str)> = Vec::new();

    let cfg = config.lock().unwrap();
    let main_metric = cfg.main_metric;
    let private_nets = cfg.private_nets.clone();
    let custom_routes = cfg.custom_routes.clone();
    drop(cfg);

    // Step 1: Check /2 counter-routes
    if !main_gw.is_empty() && main_if > 0 {
        for prefix in COUNTER_ROUTES {
            if !network::route_exists_on_interface(prefix, &main_if_ip) {
                routes_to_add.push((prefix.to_string(), main_gw.clone(), main_if, main_metric, "Counter-route"));
                need_fix = true;
            }
        }
    }

    // Step 2: Check private networks -> TAP
    if !taps.is_empty() {
        let tap = taps[0];
        let tap_gw = network::get_tap_gateway(&tap.ip);
        if let Some(ref gw) = tap_gw {
            for net in &private_nets {
                if !network::route_exists_on_interface(net, &tap.ip) {
                    routes_to_add.push((net.clone(), gw.clone(), tap.if_index, 20, "Private route"));
                    need_fix = true;
                }
            }
        }
    }

    // Step 3: Check custom routes
    if !main_gw.is_empty() && !taps.is_empty() {
        let tap = taps[0];
        let tap_gw = network::get_tap_gateway(&tap.ip);

        for (prefix, via) in &custom_routes {
            if via == "tap" {
                if let Some(ref gw) = tap_gw {
                    if !network::route_exists_on_interface(prefix, &tap.ip) {
                        routes_to_add.push((prefix.clone(), gw.clone(), tap.if_index, 20, "Custom route"));
                        need_fix = true;
                    }
                }
            } else if via == "main" {
                if !network::route_exists_on_interface(prefix, &main_if_ip) {
                    routes_to_add.push((prefix.clone(), main_gw.clone(), main_if, main_metric, "Custom route"));
                    need_fix = true;
                }
            }
        }
    }

    if !need_fix {
        let mut g = inner.lock().unwrap();
        if !g.steady_state {
            g.steady_state = true;
            drop(g);
            logger.info("All routes verified OK — monitoring (steady state)");
        }
        return;
    }

    // Phase 2: Enter Fixing state
    {
        let mut g = inner.lock().unwrap();
        g.fixing = true;
        g.state = MonitorState::Fixing;
        g.steady_state = false;
    }

    // Phase 3: Actually add routes
    for (cidr, gw, ifidx, metric, label) in &routes_to_add {
        network::run_route(&format!("add {} {} if {} metric {}", cidr, gw, ifidx, metric));
        logger.info(&format!("{} ADD: {} -> if={} gw={} metric={}", label, cidr, ifidx, gw, metric));
    }

    // Update config
    {
        let mut cfg = config.lock().unwrap();
        cfg.total_fixes += 1;
        cfg.last_fix_time = Some(chrono::Local::now().format("%Y-%m-%d %H:%M:%S").to_string());
        cfg.save();
        let total = cfg.total_fixes;
        let bark = cfg.bark_enabled;
        let bark_server = cfg.bark_server.clone();
        let bark_key = cfg.bark_device_key.clone();
        let bark_title = cfg.bark_title.clone();
        drop(cfg);

        logger.info(&format!("Counter-routes applied (total fixes: {})", total));

        if bark && !bark_server.is_empty() && !bark_key.is_empty() {
            send_bark(&bark_server, &bark_key, &bark_title, total, logger);
        }
    }

    // Restore to Running
    {
        let mut g = inner.lock().unwrap();
        g.fixing = false;
        if g.state == MonitorState::Fixing {
            g.state = MonitorState::Running;
        }
    }
}

fn send_bark(server: &str, key: &str, title: &str, total: u64, logger: &Arc<Logger>) {
    let server = server.trim_end_matches('/');
    let now = chrono::Local::now().format("%H:%M:%S");
    let title_enc = url_encode(title);
    let body_enc = url_encode(&format!("Route fix #{} at {}", total, now));
    let url = format!("{}/{}/{}/{}", server, key, title_enc, body_enc);

    match reqwest::blocking::get(&url) {
        Ok(r) => {
            if r.status().is_success() {
                logger.info("Bark notification sent");
            } else {
                logger.warn(&format!("Bark response: {}", r.status()));
            }
        }
        Err(e) => logger.warn(&format!("Bark send error: {}", e)),
    }
}

fn url_encode(s: &str) -> String {
    let mut result = String::new();
    for byte in s.bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) {
            result.push(byte as char);
        } else {
            result.push_str(&format!("%{:02X}", byte));
        }
    }
    result
}
