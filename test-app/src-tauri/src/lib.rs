use std::fs;
use std::path::PathBuf;
use std::collections::HashMap;
use serde::{Deserialize, Serialize};
use tauri::Manager;

// === 核心辅助函数 ===
// 作用：自动找到用户的【文档/MyNotes】文件夹路径
// 改用 Tauri 的 AppHandle 获取路径，以更好地支持跨平台（包括移动端）
fn get_notes_dir(app: &tauri::AppHandle) -> PathBuf {
    let mut path = app.path().document_dir().unwrap_or(PathBuf::from("."));
    path.push("MyNotes");
    
    // 如果文件夹不存在，自动创建它
    // 使用 create_dir_all 以防父目录不存在
    if !path.exists() {
        let _ = fs::create_dir_all(&path);
    }
    path
}

// === 命令 1: 获取笔记列表 ===
#[tauri::command]
fn get_notes(app: tauri::AppHandle) -> Vec<String> {
    let mut notes = Vec::new();
    let path = get_notes_dir(&app);

    if let Ok(entries) = fs::read_dir(path) {
        for entry in entries {
            if let Ok(entry) = entry {
                let p = entry.path();
                // 筛选 .md 和 .txt 文件
                if let Some(ext) = p.extension().and_then(|s| s.to_str()) {
                    if ext == "md" || ext == "txt" {
                        if let Some(name) = p.file_name().and_then(|s| s.to_str()) {
                            notes.push(name.to_string());
                        }
                    }
                }
            }
        }
    }
    notes
}

// === 命令 2: 读取笔记内容 ===
#[tauri::command]
fn read_note(app: tauri::AppHandle, filename: String) -> String {
    let mut path = get_notes_dir(&app);
    path.push(filename);
    // 如果读取失败，返回空字符串
    fs::read_to_string(path).unwrap_or_default()
}

// === 命令 3: 保存笔记 ===
#[tauri::command]
fn save_note(app: tauri::AppHandle, filename: String, content: String) {
    let mut path = get_notes_dir(&app);
    path.push(&filename);
    let _ = fs::write(path, content);
    println!("Saved: {}", filename);
}

// === 命令 4: 新建笔记 ===
#[tauri::command]
fn create_note(app: tauri::AppHandle, filename: String) -> Result<String, String> {
    let mut path = get_notes_dir(&app);
    path.push(&filename);

    if path.exists() {
        return Err("文件已存在".to_string());
    }

    match fs::write(&path, "") {
        Ok(_) => Ok("创建成功".to_string()),
        Err(e) => Err(format!("创建失败: {}", e)),
    }
}

// === 命令 5: 删除笔记 ===
#[tauri::command]
fn delete_note(app: tauri::AppHandle, filename: String) -> Result<String, String> {
    let mut path = get_notes_dir(&app);
    path.push(&filename);

    if !path.exists() {
        return Err("文件不存在".to_string());
    }

    match fs::remove_file(&path) {
        Ok(_) => Ok("删除成功".to_string()),
        Err(e) => Err(format!("删除失败: {}", e)),
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ZoteroLibraryItem {
    id: String,
    item_key: String,
    title: String,
    author: String,
    year: String,
    cite_key: String,
    item_uri: String,
    pdf_uri: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ZoteroCollectionNode {
    id: i64,
    parent_id: Option<i64>,
    key: String,
    name: String,
    item_count: usize,
    item_keys: Vec<String>,
    children: Vec<ZoteroCollectionNode>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ZoteroCollectionsPayload {
    db_path: String,
    collections: Vec<ZoteroCollectionNode>,
}

fn parse_authors(value: &serde_json::Value) -> String {
    let mut names: Vec<String> = Vec::new();
    if let Some(arr) = value.as_array() {
        for person in arr {
            if let Some(literal) = person.get("literal").and_then(|x| x.as_str()) {
                if !literal.trim().is_empty() {
                    names.push(literal.trim().to_string());
                    continue;
                }
            }

            let family = person.get("family").and_then(|x| x.as_str()).unwrap_or("").trim();
            let given = person.get("given").and_then(|x| x.as_str()).unwrap_or("").trim();
            let full = if family.is_empty() && given.is_empty() {
                String::new()
            } else if given.is_empty() {
                family.to_string()
            } else if family.is_empty() {
                given.to_string()
            } else {
                format!("{} {}", family, given)
            };

            if !full.is_empty() {
                names.push(full);
            }
        }
    }

    if names.is_empty() {
        "未知作者".to_string()
    } else {
        names.join("; ")
    }
}

fn parse_year(item: &serde_json::Value) -> String {
    if let Some(year) = item
        .get("issued")
        .and_then(|issued| issued.get("date-parts"))
        .and_then(|parts| parts.as_array())
        .and_then(|parts| parts.first())
        .and_then(|part| part.as_array())
        .and_then(|part| part.first())
        .and_then(|x| x.as_i64())
    {
        return year.to_string();
    }

    item
        .get("year")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .trim()
        .to_string()
}

fn parse_cite_key(item: &serde_json::Value) -> String {
    item
        .get("citationKey")
        .or_else(|| item.get("citekey"))
        .or_else(|| item.get("id"))
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .trim()
        .to_string()
}

fn fetch_zotero_library_from_sqlite(db_path: Option<String>) -> Result<Vec<ZoteroLibraryItem>, String> {
    let sqlite_path = db_path
        .map(PathBuf::from)
        .unwrap_or_else(default_zotero_sqlite_path);

    if !sqlite_path.exists() {
        return Err(format!(
            "未找到 Zotero 数据库: {}",
            sqlite_path.to_string_lossy()
        ));
    }

    let conn = rusqlite::Connection::open_with_flags(
        &sqlite_path,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
    )
    .map_err(|e| format!("打开 Zotero SQLite 失败: {}", e))?;

    let mut stmt = conn
        .prepare(
            "
            SELECT
              i.itemID,
              i.key,
              COALESCE((
                SELECT idv.value
                FROM itemData id
                JOIN fields f ON f.fieldID = id.fieldID
                JOIN itemDataValues idv ON idv.valueID = id.valueID
                WHERE id.itemID = i.itemID AND f.fieldName = 'title'
                LIMIT 1
              ), '未命名文献') AS title,
              COALESCE((
                SELECT idv.value
                FROM itemData id
                JOIN fields f ON f.fieldID = id.fieldID
                JOIN itemDataValues idv ON idv.valueID = id.valueID
                WHERE id.itemID = i.itemID AND f.fieldName IN ('date', 'year')
                LIMIT 1
              ), '') AS year,
              COALESCE((
                SELECT GROUP_CONCAT(
                  TRIM(COALESCE(cd.lastName, '') || ' ' || COALESCE(cd.firstName, '')),
                  '; '
                )
                FROM itemCreators ic
                JOIN creators c ON c.creatorID = ic.creatorID
                JOIN creatorData cd ON cd.creatorDataID = c.creatorDataID
                WHERE ic.itemID = i.itemID
              ), '未知作者') AS author
            FROM items i
            WHERE i.key IS NOT NULL
            ",
        )
        .map_err(|e| format!("读取 SQLite 文献失败: {}", e))?;

    let rows = stmt
        .query_map([], |row| {
            let item_id: i64 = row.get(0)?;
            let item_key: String = row.get(1)?;
            let title: String = row.get(2)?;
            let year_raw: String = row.get(3)?;
            let author: String = row.get(4)?;
            Ok((item_id, item_key, title, year_raw, author))
        })
        .map_err(|e| format!("解析 SQLite 文献失败: {}", e))?;

    let mut result: Vec<ZoteroLibraryItem> = Vec::new();
    for row in rows {
        let (item_id, item_key, title, year_raw, author) =
            row.map_err(|e| format!("读取 SQLite 文献行失败: {}", e))?;

        let year = year_raw
            .chars()
            .take(4)
            .collect::<String>()
            .trim()
            .to_string();

        result.push(ZoteroLibraryItem {
            id: format!("{}", item_id),
            item_key: item_key.clone(),
            title,
            author: if author.trim().is_empty() {
                "未知作者".to_string()
            } else {
                author
            },
            year,
            cite_key: item_key.clone(),
            item_uri: format!("zotero://select/items/{}", item_key),
            pdf_uri: format!("zotero://open-pdf/library/items/{}", item_key),
        });
    }

    Ok(result)
}

fn default_zotero_sqlite_path() -> PathBuf {
    let mut path = dirs::home_dir().unwrap_or_else(|| PathBuf::from("."));
    path.push("Zotero");
    path.push("zotero.sqlite");
    path
}

fn build_collection_tree(
    roots: &[i64],
    children_map: &HashMap<i64, Vec<i64>>,
    base_map: &HashMap<i64, ZoteroCollectionNode>,
) -> Vec<ZoteroCollectionNode> {
    fn build_node(
        id: i64,
        children_map: &HashMap<i64, Vec<i64>>,
        base_map: &HashMap<i64, ZoteroCollectionNode>,
    ) -> Option<ZoteroCollectionNode> {
        let base = base_map.get(&id)?.clone();
        let mut node = base;
        let child_ids = children_map.get(&id).cloned().unwrap_or_default();
        let mut children = Vec::new();
        for child_id in child_ids {
            if let Some(child) = build_node(child_id, children_map, base_map) {
                children.push(child);
            }
        }
        children.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
        node.children = children;
        Some(node)
    }

    let mut tree = Vec::new();
    for id in roots {
        if let Some(node) = build_node(*id, children_map, base_map) {
            tree.push(node);
        }
    }
    tree.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    tree
}

#[tauri::command]
fn fetch_zotero_collections_tree(db_path: Option<String>) -> Result<ZoteroCollectionsPayload, String> {
    let sqlite_path = db_path
        .map(PathBuf::from)
        .unwrap_or_else(default_zotero_sqlite_path);

    if !sqlite_path.exists() {
        return Err(format!(
            "未找到 Zotero 数据库: {}",
            sqlite_path.to_string_lossy()
        ));
    }

    let conn = rusqlite::Connection::open_with_flags(
        &sqlite_path,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
    )
    .map_err(|e| format!("打开 Zotero SQLite 失败: {}", e))?;

    let mut item_keys_by_collection: HashMap<i64, Vec<String>> = HashMap::new();
    {
        let mut stmt = conn
            .prepare(
                "
                SELECT ci.collectionID, i.key
                FROM collectionItems ci
                JOIN items i ON i.itemID = ci.itemID
                WHERE i.key IS NOT NULL
                ",
            )
            .map_err(|e| format!("读取 collectionItems 失败: {}", e))?;

        let rows = stmt
            .query_map([], |row| {
                let collection_id: i64 = row.get(0)?;
                let item_key: String = row.get(1)?;
                Ok((collection_id, item_key))
            })
            .map_err(|e| format!("解析 collectionItems 失败: {}", e))?;

        for pair in rows {
            let (collection_id, item_key) = pair.map_err(|e| format!("读取条目映射失败: {}", e))?;
            item_keys_by_collection
                .entry(collection_id)
                .or_default()
                .push(item_key);
        }
    }

    let mut base_map: HashMap<i64, ZoteroCollectionNode> = HashMap::new();
    let mut children_map: HashMap<i64, Vec<i64>> = HashMap::new();
    let mut roots: Vec<i64> = Vec::new();

    {
        let mut stmt = conn
            .prepare(
                "
                SELECT collectionID, parentCollectionID, key, collectionName
                FROM collections
                ",
            )
            .map_err(|e| format!("读取 collections 失败: {}", e))?;

        let rows = stmt
            .query_map([], |row| {
                let collection_id: i64 = row.get(0)?;
                let parent_id: Option<i64> = row.get(1)?;
                let key: String = row.get(2)?;
                let name: String = row.get(3)?;
                Ok((collection_id, parent_id, key, name))
            })
            .map_err(|e| format!("解析 collections 失败: {}", e))?;

        for row in rows {
            let (collection_id, parent_id, key, name) =
                row.map_err(|e| format!("读取目录行失败: {}", e))?;
            let item_keys = item_keys_by_collection
                .get(&collection_id)
                .cloned()
                .unwrap_or_default();

            let node = ZoteroCollectionNode {
                id: collection_id,
                parent_id,
                key,
                name,
                item_count: item_keys.len(),
                item_keys,
                children: Vec::new(),
            };

            base_map.insert(collection_id, node);
            if let Some(parent) = parent_id {
                children_map.entry(parent).or_default().push(collection_id);
            } else {
                roots.push(collection_id);
            }
        }
    }

    let collections = build_collection_tree(&roots, &children_map, &base_map);

    Ok(ZoteroCollectionsPayload {
        db_path: sqlite_path.to_string_lossy().to_string(),
        collections,
    })
}

#[tauri::command]
fn fetch_zotero_library(source: Option<String>, endpoint: Option<String>, db_path: Option<String>) -> Result<Vec<ZoteroLibraryItem>, String> {
    let selected_source = source.unwrap_or_else(|| "bbt-local-api".to_string());
    if selected_source == "sqlite-readonly" {
        return fetch_zotero_library_from_sqlite(db_path);
    }

    if selected_source != "bbt-local-api" {
        return Err("仅支持 bbt-local-api 或 sqlite-readonly".to_string());
    }

    let api_endpoint = endpoint.unwrap_or_else(|| {
        "http://127.0.0.1:23119/better-bibtex/export/library?/translator=csljson".to_string()
    });

    let response_text = reqwest::blocking::Client::new()
        .get(&api_endpoint)
        .send()
        .map_err(|e| format!("无法连接 BBT 本地 API: {}", e))?
        .error_for_status()
        .map_err(|e| format!("BBT API 返回错误状态: {}", e))?
        .text()
        .map_err(|e| format!("读取 BBT API 响应失败: {}", e))?;

    let value: serde_json::Value =
        serde_json::from_str(&response_text).map_err(|e| format!("解析 CSL-JSON 失败: {}", e))?;

    let items_source = if let Some(arr) = value.as_array() {
        arr.clone()
    } else if let Some(arr) = value.get("items").and_then(|x| x.as_array()) {
        arr.clone()
    } else {
        return Err("未从 BBT 获取到文献数组，请检查导出接口".to_string());
    };

    let mut result: Vec<ZoteroLibraryItem> = Vec::new();
    for item in items_source {
        let id = item
            .get("id")
            .and_then(|x| x.as_str())
            .unwrap_or("")
            .trim()
            .to_string();

        let item_key = id
            .split('/')
            .last()
            .unwrap_or("")
            .trim()
            .to_string();

        let title = item
            .get("title")
            .and_then(|x| x.as_str())
            .unwrap_or("未命名文献")
            .trim()
            .to_string();

        let author = parse_authors(item.get("author").unwrap_or(&serde_json::Value::Null));
        let year = parse_year(&item);
        let cite_key = parse_cite_key(&item);

        let item_uri = item
            .get("URL")
            .and_then(|x| x.as_str())
            .filter(|url| url.starts_with("zotero://"))
            .map(|x| x.to_string())
            .unwrap_or_else(|| {
                if item_key.is_empty() {
                    String::new()
                } else {
                    format!("zotero://select/items/{}", item_key)
                }
            });

        let pdf_uri = if item_key.is_empty() {
            String::new()
        } else {
            format!("zotero://open-pdf/library/items/{}", item_key)
        };

        result.push(ZoteroLibraryItem {
            id,
            item_key,
            title,
            author,
            year,
            cite_key,
            item_uri,
            pdf_uri,
        });
    }

    Ok(result)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            get_notes, 
            read_note, 
            save_note, 
            create_note,
            delete_note,
            fetch_zotero_library,
            fetch_zotero_collections_tree
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
