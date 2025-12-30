// src-tauri/src/main.rs
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::fs;
use std::path::PathBuf;

// === 核心辅助函数 ===
// 作用：自动找到用户的【文档/MyNotes】文件夹路径
// 如果没有这个函数，后面的命令都会报错找不到路径
fn get_notes_dir() -> PathBuf {
    // 使用 dirs 库获取系统的文档目录
    let mut path = dirs::document_dir().unwrap_or(PathBuf::from("."));
    path.push("MyNotes");
    
    // 如果文件夹不存在，自动创建它
    if !path.exists() {
        let _ = fs::create_dir(&path);
    }
    path
}

// === 命令 1: 获取笔记列表 ===
#[tauri::command]
fn get_notes() -> Vec<String> {
    let mut notes = Vec::new();
    let path = get_notes_dir(); // 调用辅助函数

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
fn read_note(filename: String) -> String {
    let mut path = get_notes_dir();
    path.push(filename);
    // 如果读取失败，返回空字符串
    fs::read_to_string(path).unwrap_or_default()
}

// === 命令 3: 保存笔记 ===
#[tauri::command]
fn save_note(filename: String, content: String) {
    let mut path = get_notes_dir();
    path.push(&filename); // 注意这里用了 & 符号借用
    let _ = fs::write(path, content);
    println!("Saved: {}", filename);
}

// === 命令 4: 新建笔记 (这是您新增的功能) ===
#[tauri::command]
fn create_note(filename: String) -> Result<String, String> {
    let mut path = get_notes_dir();
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
fn delete_note(filename: String) -> Result<String, String> {
    let mut path = get_notes_dir();
    path.push(&filename);

    if !path.exists() {
        return Err("文件不存在".to_string());
    }

    match fs::remove_file(&path) {
        Ok(_) => Ok("删除成功".to_string()),
        Err(e) => Err(format!("删除失败: {}", e)),
    }
}

fn main() {
    tauri::Builder::default()
        // 注册所有命令
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