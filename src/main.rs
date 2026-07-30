slint::include_modules!();

mod auth;
mod config;
mod fingerprint;
mod logger;
mod network;
mod route_guard;
mod update;

use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use slint::{ComponentHandle, SharedString, Timer, TimerMode};

use config::AppConfig;
use logger::Logger;
use route_guard::{MonitorState, RouteGuard};

const APP_VERSION: &str = "4.0.0";

fn main() {
    // ==================== Initialize ====================
    let logger = Arc::new(Logger::new());
    let mut config = AppConfig::load();

    logger.info(&format!("GatewayPolicy v{} starting (Rust)", APP_VERSION));

    // ==================== Device Fingerprint ====================
    let device_id = fingerprint::generate();
    logger.info(&format!("Device ID: {}", device_id));

    // ==================== Auth Check ====================
    let server_url = config.effective_auth_server().to_string();
    let is_authorized = {
        let mut cfg = config.clone();
        let ok = auth::verify_token_silent(&device_id, &server_url, &mut cfg);
        config = cfg;
        if ok {
            logger.info("Device authorized");
        } else {
            logger.warn("Device not authorized — showing auth UI");
        }
        ok
    };

    let config = Arc::new(Mutex::new(config));
    let route_guard = Arc::new(RouteGuard::new(config.clone(), logger.clone()));

    // ==================== Create UI ====================
    let app = AppWindow::new().unwrap();
    app.set_device_id(SharedString::from(device_id.as_str()));

    if is_authorized {
        populate_settings(&app, &config);
        app.set_version(SharedString::from(APP_VERSION));
        let id_short = &device_id[..device_id.len().min(8)];
        app.set_auth_info(SharedString::from(
            format!("Device: {}...\nStatus: Authorized", id_short),
        ));
        app.set_mode(SharedString::from("main"));
        route_guard.start();
        app.set_is_running(true);
    } else {
        app.set_mode(SharedString::from("auth"));
        app.set_auth_message(SharedString::from(
            "Send the device code to admin, then click Verify after authorized.",
        ));
    }

    // ==================== Callback: verify-auth ====================
    {
        let weak = app.as_weak();
        let config = config.clone();
        let logger = logger.clone();
        let route_guard = route_guard.clone();
        let device_id = device_id.clone();

        app.on_verify_auth(move || {
            let app = match weak.upgrade() {
                Some(a) => a,
                None => return,
            };
            app.set_auth_busy(true);
            app.set_auth_message(SharedString::from("Verifying..."));

            let weak = weak.clone();
            let config = config.clone();
            let logger = logger.clone();
            let route_guard = route_guard.clone();
            let device_id = device_id.clone();

            thread::spawn(move || {
                let server_url = {
                    let cfg = config.lock().unwrap();
                    cfg.effective_auth_server().to_string()
                };
                match auth::check_auth(&device_id, &server_url) {
                    Ok(result) => {
                        if result.ok {
                            {
                                let mut cfg = config.lock().unwrap();
                                cfg.auth_token = result.token;
                                cfg.refresh_token_expiry();
                                cfg.save();
                            }
                            logger.info("Device authorized successfully");

                            let weak = weak.clone();
                            let config = config.clone();
                            let route_guard = route_guard.clone();
                            let device_id = device_id.clone();

                            let _ = slint::invoke_from_event_loop(move || {
                                let app = match weak.upgrade() {
                                    Some(a) => a,
                                    None => return,
                                };
                                app.set_auth_busy(false);
                                populate_settings(&app, &config);
                                app.set_version(SharedString::from(APP_VERSION));
                                let id_short = &device_id[..device_id.len().min(8)];
                                app.set_auth_info(SharedString::from(
                                    format!("Device: {}...\nStatus: Authorized", id_short),
                                ));
                                app.set_mode(SharedString::from("main"));
                                route_guard.start();
                                app.set_is_running(true);
                            });
                        } else {
                            let msg = format!("Denied: {}", result.message);
                            logger.warn(&msg);
                            let _ = slint::invoke_from_event_loop(move || {
                                if let Some(app) = weak.upgrade() {
                                    app.set_auth_busy(false);
                                    app.set_auth_message(SharedString::from(msg));
                                }
                            });
                        }
                    }
                    Err(e) => {
                        let msg = format!("Error: {}", e);
                        logger.error(&msg);
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(app) = weak.upgrade() {
                                app.set_auth_busy(false);
                                app.set_auth_message(SharedString::from(msg));
                            }
                        });
                    }
                }
            });
        });
    }

    // ==================== Callback: copy-device-id ====================
    {
        let device_id = device_id.clone();
        app.on_copy_device_id(move || {
            set_clipboard(&device_id);
        });
    }

    // ==================== Callback: toggle-monitor ====================
    {
        let weak = app.as_weak();
        let route_guard = route_guard.clone();
        app.on_toggle_monitor(move || {
            let app = match weak.upgrade() {
                Some(a) => a,
                None => return,
            };
            if app.get_is_running() {
                route_guard.stop();
                app.set_is_running(false);
            } else {
                route_guard.start();
                app.set_is_running(true);
            }
        });
    }

    // ==================== Callback: fix-now ====================
    {
        let route_guard = route_guard.clone();
        app.on_fix_now(move || {
            route_guard.do_check_now();
        });
    }

    // ==================== Callback: restore-network ====================
    {
        let weak = app.as_weak();
        let route_guard = route_guard.clone();
        app.on_restore_network(move || {
            route_guard.restore_network();
            if let Some(app) = weak.upgrade() {
                app.set_is_running(false);
            }
        });
    }

    // ==================== Callback: save-settings ====================
    {
        let weak = app.as_weak();
        let config = config.clone();
        let logger = logger.clone();
        app.on_save_settings(move || {
            let app = match weak.upgrade() {
                Some(a) => a,
                None => return,
            };
            {
                let mut cfg = config.lock().unwrap();
                cfg.check_interval_seconds = app.get_check_interval() as u32;
                cfg.main_metric = app.get_main_metric() as u32;
                cfg.auto_start = app.get_auto_start();
                cfg.minimize_to_tray = app.get_minimize_tray();
                cfg.show_notification = app.get_show_notify();
                cfg.bark_enabled = app.get_bark_enabled();
                cfg.bark_server = app.get_bark_server().to_string();
                cfg.bark_device_key = app.get_bark_key().to_string();
                cfg.save();
            }
            set_auto_start(app.get_auto_start());
            logger.info("Settings saved");
        });
    }

    // ==================== Callback: check-update ====================
    {
        let weak = app.as_weak();
        let config = config.clone();
        app.on_check_update(move || {
            if let Some(app) = weak.upgrade() {
                app.set_update_message(SharedString::from("Checking..."));
            }
            let weak = weak.clone();
            let config = config.clone();
            thread::spawn(move || {
                let repo = config.lock().unwrap().update_repo.clone();
                let ver = APP_VERSION.to_string();
                match update::check_latest(&repo, &ver) {
                    Ok(Some(info)) => {
                        let msg = format!("Found {}. Downloading...", info.tag_name);
                        let w = weak.clone();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(app) = w.upgrade() {
                                app.set_update_message(SharedString::from(msg));
                            }
                        });
                        match update::download_and_install(&info.download_url, &info.asset_name) {
                            Ok(()) => {
                                let w = weak.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(app) = w.upgrade() {
                                        app.set_update_message(SharedString::from(
                                            "Update downloaded. Restarting...",
                                        ));
                                    }
                                });
                                thread::sleep(Duration::from_secs(1));
                                std::process::exit(0);
                            }
                            Err(e) => {
                                let msg = format!("Update failed: {}", e);
                                let w = weak.clone();
                                let _ = slint::invoke_from_event_loop(move || {
                                    if let Some(app) = w.upgrade() {
                                        app.set_update_message(SharedString::from(msg));
                                    }
                                });
                            }
                        }
                    }
                    Ok(None) => {
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(app) = weak.upgrade() {
                                app.set_update_message(SharedString::from("Already up to date."));
                            }
                        });
                    }
                    Err(e) => {
                        let msg = format!("Check failed: {}", e);
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(app) = weak.upgrade() {
                                app.set_update_message(SharedString::from(msg));
                            }
                        });
                    }
                }
            });
        });
    }

    // ==================== Callback: clear-logs ====================
    {
        let logger = logger.clone();
        app.on_clear_logs(move || {
            logger.clear();
        });
    }

    // ==================== Callback: export-logs ====================
    {
        let logger = logger.clone();
        app.on_export_logs(move || {
            let content = logger.export_all();
            let path = std::env::current_exe()
                .map(|p| p.with_file_name("gp_export.txt"))
                .unwrap_or_else(|_| std::path::PathBuf::from("gp_export.txt"));
            let _ = std::fs::write(&path, &content);
        });
    }

    // ==================== UI Update Timer (500ms) ====================
    let ui_timer = Timer::default();
    {
        let weak = app.as_weak();
        let rg = route_guard.clone();
        let lg = logger.clone();
        ui_timer.start(TimerMode::Repeated, Duration::from_millis(500), move || {
            let app = match weak.upgrade() {
                Some(a) => a,
                None => return,
            };
            if app.get_mode().as_str() != "main" {
                return;
            }

            // Update status fields
            let status = rg.get_status();
            let state_str = match status.state {
                MonitorState::Running => "running",
                MonitorState::Fixing => "fixing",
                MonitorState::Stopped => "stopped",
            };
            app.set_state_text(SharedString::from(state_str));
            app.set_hijack_text(SharedString::from(if status.is_hijacked {
                "Hijacked"
            } else {
                "Clear"
            }));
            app.set_main_nic(SharedString::from(status.main_nic.as_str()));
            app.set_tap_nic(SharedString::from(status.tap_nic.as_str()));
            app.set_last_fix(SharedString::from(status.last_fix.as_str()));
            app.set_total_fixes(status.total_fixes as i32);

            // Drain pending log lines and append to UI log
            let pending = lg.take_pending();
            if !pending.is_empty() {
                let current = app.get_log_text().to_string();
                let mut combined = current;
                for line in &pending {
                    if !combined.is_empty() {
                        combined.push('\n');
                    }
                    combined.push_str(line);
                }
                // Keep last 500 lines to avoid unbounded growth
                let lines: Vec<&str> = combined.lines().collect();
                if lines.len() > 500 {
                    combined = lines[lines.len() - 500..].join("\n");
                }
                app.set_log_text(SharedString::from(combined));
            }
        });
    }

    // ==================== Token Re-validation Thread (30 min) ====================
    {
        let config = config.clone();
        let route_guard = route_guard.clone();
        let logger = logger.clone();
        let device_id = device_id.clone();
        let weak = app.as_weak();
        thread::spawn(move || loop {
            thread::sleep(Duration::from_secs(30 * 60));
            let server_url = {
                let cfg = config.lock().unwrap();
                cfg.effective_auth_server().to_string()
            };
            let ok = {
                let mut cfg = config.lock().unwrap();
                auth::verify_token_silent(&device_id, &server_url, &mut cfg)
            };
            if !ok {
                logger.warn("Token expired — switching to auth view");
                let weak = weak.clone();
                let route_guard = route_guard.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(app) = weak.upgrade() {
                        route_guard.stop();
                        app.set_is_running(false);
                        app.set_mode(SharedString::from("auth"));
                        app.set_auth_message(SharedString::from(
                            "Authorization expired. Please re-verify.",
                        ));
                    }
                });
            } else {
                logger.info("Token re-validated OK");
            }
        });
    }

    // ==================== Run Event Loop ====================
    app.run().unwrap();

    // ==================== Cleanup ====================
    route_guard.stop();
    logger.info("GatewayPolicy shutting down");
    // ui_timer is dropped here — stops the timer
}

// ==================== Helper Functions ====================

fn populate_settings(app: &AppWindow, config: &Arc<Mutex<AppConfig>>) {
    let cfg = config.lock().unwrap();
    app.set_check_interval(cfg.check_interval_seconds as i32);
    app.set_main_metric(cfg.main_metric as i32);
    app.set_auto_start(cfg.auto_start);
    app.set_minimize_tray(cfg.minimize_to_tray);
    app.set_show_notify(cfg.show_notification);
    app.set_bark_enabled(cfg.bark_enabled);
    app.set_bark_server(SharedString::from(cfg.bark_server.as_str()));
    app.set_bark_key(SharedString::from(cfg.bark_device_key.as_str()));
}

fn set_auto_start(enabled: bool) {
    use winreg::enums::*;
    use winreg::RegKey;

    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let key_path = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    if enabled {
        if let Ok(key) = hkcu.open_subkey_with_flags(key_path, KEY_SET_VALUE) {
            if let Ok(exe) = std::env::current_exe() {
                let _ = key.set_value("GatewayPolicy", &exe.to_string_lossy().to_string());
            }
        }
    } else if let Ok(key) = hkcu.open_subkey_with_flags(key_path, KEY_SET_VALUE) {
        let _ = key.delete_value("GatewayPolicy");
    }
}

fn set_clipboard(text: &str) {
    use std::ffi::OsStr;
    use std::os::windows::ffi::OsStrExt;

    const CF_UNICODETEXT: u32 = 13;
    const GMEM_MOVEABLE: u32 = 0x0002;

    #[link(name = "user32")]
    extern "system" {
        fn OpenClipboard(hwnd: *mut std::ffi::c_void) -> i32;
        fn CloseClipboard() -> i32;
        fn EmptyClipboard() -> i32;
        fn SetClipboardData(format: u32, hmem: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn GlobalAlloc(flags: u32, size: usize) -> *mut std::ffi::c_void;
        fn GlobalLock(hmem: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
        fn GlobalUnlock(hmem: *mut std::ffi::c_void) -> i32;
    }

    let mut utf16: Vec<u16> = OsStr::new(text).encode_wide().collect();
    utf16.push(0); // null terminator
    let size = utf16.len() * 2;

    unsafe {
        let hmem = GlobalAlloc(GMEM_MOVEABLE, size);
        if hmem.is_null() {
            return;
        }
        let ptr = GlobalLock(hmem);
        if ptr.is_null() {
            return;
        }
        std::ptr::copy_nonoverlapping(utf16.as_ptr() as *const u8, ptr as *mut u8, size);
        GlobalUnlock(hmem);

        if OpenClipboard(std::ptr::null_mut()) != 0 {
            EmptyClipboard();
            SetClipboardData(CF_UNICODETEXT, hmem);
            CloseClipboard();
        }
    }
}
