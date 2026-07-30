use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;

pub enum LogLevel {
    Info,
    Warning,
    Error,
}

pub struct Logger {
    log_dir: PathBuf,
    current_file: Mutex<Option<String>>,
    max_files: usize,
    pending: Mutex<Vec<String>>,
}

impl Logger {
    pub fn new() -> Self {
        let exe = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("GatewayPolicy.exe"));
        let log_dir = exe.with_file_name("logs");
        let _ = fs::create_dir_all(&log_dir);
        Logger {
            log_dir,
            current_file: Mutex::new(None),
            max_files: 30,
            pending: Mutex::new(Vec::new()),
        }
    }

    pub fn take_pending(&self) -> Vec<String> {
        let mut p = self.pending.lock().unwrap();
        std::mem::take(&mut *p)
    }

    pub fn write(&self, message: &str, level: LogLevel) {
        let now = chrono::Local::now();
        let prefix = match level {
            LogLevel::Info => "INFO",
            LogLevel::Warning => "WARN",
            LogLevel::Error => "ERR ",
        };
        let line = format!("[{}] [{}] {}", now.format("%H:%M:%S"), prefix, message);
        self.write_to_file(&now, &line);
    }

    pub fn info(&self, msg: &str) { self.write(msg, LogLevel::Info); }
    pub fn warn(&self, msg: &str) { self.write(msg, LogLevel::Warning); }
    pub fn error(&self, msg: &str) { self.write(msg, LogLevel::Error); }

    fn write_to_file(&self, now: &chrono::DateTime<chrono::Local>, line: &str) {
        let log_file = self.log_dir.join(format!("gp_{}.log", now.format("%Y%m%d")));
        let log_file_str = log_file.to_string_lossy().to_string();

        {
            let mut current = self.current_file.lock().unwrap();
            if current.as_deref() != Some(&log_file_str) {
                *current = Some(log_file_str.clone());
                drop(current);
                self.clean_old_logs();
            }
        }

        if let Ok(mut f) = OpenOptions::new().append(true).create(true).open(&log_file) {
            let _ = writeln!(f, "{}", line);
        }

        // Push to pending buffer for UI consumption
        self.pending.lock().unwrap().push(line.to_string());
    }

    fn clean_old_logs(&self) {
        if let Ok(entries) = fs::read_dir(&self.log_dir) {
            let mut files: Vec<_> = entries
                .filter_map(|e| e.ok())
                .filter(|e| e.file_name().to_string_lossy().starts_with("gp_"))
                .collect();
            files.sort_by(|a, b| b.file_name().cmp(&a.file_name()));
            for f in files.into_iter().skip(self.max_files) {
                let _ = fs::remove_file(f.path());
            }
        }
    }

    pub fn get_recent_lines(&self, max_lines: usize) -> String {
        let mut files: Vec<_> = fs::read_dir(&self.log_dir)
            .map(|d| d.filter_map(|e| e.ok()).filter(|e| e.file_name().to_string_lossy().starts_with("gp_")).collect())
            .unwrap_or_default();
        files.sort_by(|a, b| b.file_name().cmp(&a.file_name()));

        let mut all_lines: Vec<String> = Vec::new();
        for f in &files {
            if let Ok(content) = fs::read_to_string(f.path()) {
                for line in content.lines().rev() {
                    if !line.is_empty() {
                        all_lines.push(line.to_string());
                        if all_lines.len() >= max_lines { break; }
                    }
                }
            }
            if all_lines.len() >= max_lines { break; }
        }
        all_lines.reverse();
        all_lines.join("\n")
    }

    pub fn export_all(&self) -> String {
        let mut files: Vec<_> = fs::read_dir(&self.log_dir)
            .map(|d| d.filter_map(|e| e.ok()).filter(|e| e.file_name().to_string_lossy().starts_with("gp_")).collect())
            .unwrap_or_default();
        files.sort_by(|a, b| a.file_name().cmp(&b.file_name()));

        let mut result = String::new();
        for f in &files {
            result.push_str(&format!("=== {} ===\n", f.file_name().to_string_lossy()));
            if let Ok(content) = fs::read_to_string(f.path()) {
                result.push_str(&content);
            }
            result.push('\n');
        }
        result
    }

    pub fn clear(&self) {
        if let Ok(entries) = fs::read_dir(&self.log_dir) {
            for e in entries.filter_map(|e| e.ok()) {
                if e.file_name().to_string_lossy().starts_with("gp_") {
                    let _ = fs::remove_file(e.path());
                }
            }
        }
    }
}
