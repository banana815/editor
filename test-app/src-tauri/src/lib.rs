use std::fs;
use std::path::PathBuf;
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            get_notes, 
            read_note, 
            save_note, 
            create_note,
            delete_note
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
