use anyhow::{Context, Result};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use nl_parser::flatcc_builder::{FlatccBuilder, Ref};
use nl_parser::module::{Language, LogEntry, Module, serialize_translations_json};
use nl_parser::pipeline;

use crate::config;
<<<<<<< HEAD
use crate::models::{BaseModuleRow, ConfigRow, LogEntryRow, ScriptRow, StyleRow};
=======
use crate::models::{BaseModuleRow, LogEntryRow, ScriptRow, StyleRow};
>>>>>>> 1ea974db663c59b4548e1e9ee4db9a452ebe92a2

pub fn build_module_bin(
    base: &BaseModuleRow,
    username: &str,
    type7_blob: Option<&str>,
    log_entries: &[LogEntryRow],
<<<<<<< HEAD
    configs: &[ConfigRow],
=======
>>>>>>> 1ea974db663c59b4548e1e9ee4db9a452ebe92a2
    scripts: &[ScriptRow],
    styles: &[StyleRow],
) -> Result<Vec<u8>> {
    let languages: Vec<Language> =
        serde_json::from_str(&base.languages_json).context("deserialize languages from DB")?;

<<<<<<< HEAD
    let config_rows: std::collections::HashMap<i32, &ConfigRow> =
        configs.iter().map(|config| (config.entry_id, config)).collect();
=======
>>>>>>> 1ea974db663c59b4548e1e9ee4db9a452ebe92a2
    let script_rows: std::collections::HashMap<i32, &ScriptRow> =
        scripts.iter().map(|script| (script.entry_id, script)).collect();
    let style_rows: std::collections::HashMap<i32, &StyleRow> =
        styles.iter().map(|style| (style.entry_id, style)).collect();
    let mut config_log = Vec::new();
    let mut script_log = Vec::new();
    let mut style_log = Vec::new();
    for entry in log_entries {
<<<<<<< HEAD
        let config = config_rows.get(&entry.entry_id).copied();
        let script = script_rows.get(&entry.entry_id).copied();
        let style = style_rows.get(&entry.entry_id).copied();
        let name = config
            .map(|config| config.name.as_str())
            .or_else(|| script.map(|script| script.name.as_str()))
=======
        let script = script_rows.get(&entry.entry_id).copied();
        let style = style_rows.get(&entry.entry_id).copied();
        let name = script
            .map(|script| script.name.as_str())
>>>>>>> 1ea974db663c59b4548e1e9ee4db9a452ebe92a2
            .or_else(|| style.map(|style| style.name.as_str()))
            .unwrap_or_else(|| entry.entry_type.as_str());
        let log_entry = LogEntry {
            entry_id: entry.entry_id as u32,
            timestamp: entry.timestamp as u32,
            name: name.to_string(),
            author: entry.author.clone(),
            lua_code: script.map(|script| script.content.clone()),
        };
        match entry.entry_type.as_str() {
            "Config" => config_log.push(log_entry),
            "Script" => script_log.push(log_entry),
            "Style" => {
                let Some(style) = style else {
                    continue;
                };

                style_log.push(LogEntry {
                    entry_id: style.entry_id as u32,
                    timestamp: entry.timestamp as u32,
                    name: style.name.clone(),
                    author: entry.author.clone(),
                    lua_code: Some(style.content.clone()),
                });
            }
            _ => {}
        }
    }

    let inner_bytes = build_inner_flatbuffer(
        base,
        username,
        type7_blob,
        &config_log,
        &script_log,
        &style_log,
        &languages,
    )?;

    let flatbuffer_bytes = build_outer_wrapper(base.version as u32, &inner_bytes);
    Module::from_flatbuffer(&flatbuffer_bytes).with_context(|| {
        format!(
            "built invalid module flatbuffer for user={username} version={} flatbuffer_len={} inner_len={} config_log={} script_log={} styles={} languages={} skin_data_msgpack={}",
            base.version,
            flatbuffer_bytes.len(),
            inner_bytes.len(),
            config_log.len(),
            script_log.len(),
            style_log.len(),
            languages.len(),
            base.skin_data_msgpack.len(),
        )
    })?;
    let encrypted = pipeline::save_module(&flatbuffer_bytes).context("encrypt module")?;
    Ok(encrypted)
}

fn build_outer_wrapper(version: u32, inner_bytes: &[u8]) -> Vec<u8> {
    let mut ob = FlatccBuilder::new();
    ob.force_defaults(true);
    let payload = ob.create_vector_u8(inner_bytes);
    ob.start_table(2);
    ob.table_add_u32(0, version, 0);
    ob.table_add_offset(1, payload);
    let wrapper = ob.end_table();
    ob.finish(wrapper)
}

struct LangStrings {
    code: Ref,
    english_name: Ref,
    native_name: Ref,
    translations: Option<Ref>,
}

fn build_inner_flatbuffer(
    base: &BaseModuleRow,
    username: &str,
    type7_blob: Option<&str>,
    config_log: &[LogEntry],
    script_log: &[LogEntry],
    styles: &[LogEntry],
    languages: &[Language],
) -> Result<Vec<u8>> {
    let mut b = FlatccBuilder::new();

    let config_offsets: Vec<_> = config_log
        .iter()
        .map(|e| build_log_entry_with_gap(&mut b, e, 8))
        .collect();
    let script_offsets: Vec<_> = script_log.iter().map(|e| build_log_entry(&mut b, e)).collect();
    let style_offsets: Vec<_> = styles.iter().map(|e| build_log_entry(&mut b, e)).collect();

    let mut lang_data: Vec<LangStrings> = Vec::new();
    for lang in languages {
        let translations = match &lang.translations {
            Some(value) => {
                let json_bytes = serialize_translations_json(value)?;
                let json_str = std::str::from_utf8(&json_bytes)
                    .map_err(|e| anyhow::anyhow!("translations JSON not UTF-8: {e}"))?;
                Some(b.create_string(json_str))
            }
            None => None,
        };
        let code = b.create_string(&lang.code);
        let english_name = b.create_string(&lang.english_name);
        let native_name = if lang.native_name == lang.english_name {
            english_name
        } else {
            b.create_string(&lang.native_name)
        };
        lang_data.push(LangStrings {
            code,
            english_name,
            native_name,
            translations,
        });
    }

    b.push_zeros(4);
    let extra_data = match type7_blob.map(decode_type7_blob).transpose()? {
        Some(json_bytes) => b.create_vector_u8(&json_bytes),
        None => b.create_vector_u8(&[]),
    };

    let _orphaned_admin = b.create_string("admin");
    let skin_data = b.create_vector_u8(&base.skin_data_msgpack);
    let auth_token = b.create_string(config::MODULE_AUTH_TOKEN);

    let lang_offsets = if lang_data.len() == 5 {
        let creation_order: &[usize] = &[0, 3, 2, 1, 4];
        let mut offsets = vec![Ref::dummy(); lang_data.len()];
        for &idx in creation_order {
            offsets[idx] = build_lang_table(&mut b, &lang_data[idx]);
        }
        offsets
    } else {
        lang_data
            .iter()
            .map(|ls| build_lang_table(&mut b, ls))
            .collect()
    };

    let lang_vec = b.create_vector_offsets(&lang_offsets);
    let script_vec = b.create_vector_offsets(&script_offsets);
    let style_vec = b.create_vector_offsets(&style_offsets);
    let config_vec = b.create_vector_offsets(&config_offsets);

    b.start_table(13);
    b.table_add_offset(4, config_vec);
    b.table_add_offset(5, script_vec);
    b.table_add_offset(6, style_vec);
    b.table_add_offset(7, lang_vec);
    b.table_add_offset(1, extra_data);
    b.push_zeros(4);
    let author = b.create_string(username);
    b.table_add_offset(2, author);
    b.table_add_offset(9, skin_data);
    b.table_add_u32(3, base.checksum as u32, 0);
    b.table_add_u32(8, base.enabled as u32, 0);
    b.table_add_u32(12, base.buffer_capacity as u32, 0);
    b.table_add_offset(11, auth_token);
    let root = b.end_table();
    Ok(b.finish_minimal(root))
}

fn decode_type7_blob(blob_b64: &str) -> Result<Vec<u8>> {
    let raw = STANDARD
        .decode(blob_b64.trim())
        .context("type7_blob base64 decode failed")?;
    let value: serde_json::Value =
        rmp_serde::from_slice(&raw).context("type7_blob MessagePack decode failed")?;
    serde_json::to_vec(&value).context("type7_blob JSON encode failed")
}

fn build_lang_table(b: &mut FlatccBuilder, ls: &LangStrings) -> Ref {
    b.start_table(7);
    b.table_add_offset(2, ls.code);
    b.table_add_offset(4, ls.english_name);
    b.table_add_offset(5, ls.native_name);
    if let Some(t) = ls.translations {
        b.table_add_offset(6, t);
    }
    b.end_table()
}

fn build_log_entry(b: &mut FlatccBuilder, entry: &LogEntry) -> Ref {
    let name = b.create_string(&entry.name);
    let author = b.create_string(&entry.author);
    let lua_code = entry.lua_code.as_ref().map(|code| b.create_string(code));
    b.start_table(if lua_code.is_some() { 6 } else { 5 });
    b.table_add_u32(0, entry.entry_id, 0);
    b.table_add_u32(1, entry.timestamp, 0);
    b.table_add_offset(3, name);
    b.table_add_offset(4, author);
    if let Some(lua_code) = lua_code {
        b.table_add_offset(5, lua_code);
    }
    b.end_table()
}

fn build_log_entry_with_gap(b: &mut FlatccBuilder, entry: &LogEntry, gap: usize) -> Ref {
    let name = b.create_string(&entry.name);
    let author = b.create_string(&entry.author);
    b.push_zeros(gap);
    b.start_table(5);
    b.table_add_u32(0, entry.entry_id, 0);
    b.table_add_u32(1, entry.timestamp, 0);
    b.table_add_offset(3, name);
    b.table_add_offset(4, author);
    b.end_table()
}
