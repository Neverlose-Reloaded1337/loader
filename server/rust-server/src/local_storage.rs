use std::path::PathBuf;

use crate::config::CONFIGS_LOCAL_DIR;

fn config_file_path(name: &str) -> Option<PathBuf> {
    if name.trim().is_empty() {
        tracing::warn!("skipping local config sync because config name is empty");
        return None;
    }

    Some(PathBuf::from(CONFIGS_LOCAL_DIR).join(format!("{name}.cfg")))
}

pub fn save_config_file(name: &str, content: &str) {
    let Some(path) = config_file_path(name) else {
        return;
    };

    if let Err(err) = std::fs::create_dir_all(CONFIGS_LOCAL_DIR) {
        tracing::warn!(
            "failed to create local config directory {}: {}",
            CONFIGS_LOCAL_DIR,
            err
        );
        return;
    }

    if let Err(err) = std::fs::write(&path, content) {
        tracing::warn!("failed to write local config file {}: {}", path.display(), err);
    }
}

pub fn delete_config_file(name: &str) {
    let Some(path) = config_file_path(name) else {
        return;
    };

    if let Err(err) = std::fs::remove_file(&path) {
        if err.kind() != std::io::ErrorKind::NotFound {
            tracing::warn!("failed to delete local config file {}: {}", path.display(), err);
        }
    }
}
