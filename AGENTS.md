# Agent Guide for test-app (Tauri Editor)

This repository contains a desktop markdown editor built with **Tauri v2** (Rust) and **React** (TypeScript/Vite).

## 🚀 Quick Start

### Prerequisites
- Node.js (v20+ recommended)
- Rust (stable)
- OS-specific build tools (Xcode for macOS, VS C++ Build Tools for Windows, libwebkit2gtk for Linux)

### Commands

**Frontend (React/Vite)**
- `npm install` - Install dependencies
- `npm run dev` - Start frontend dev server only (rarely used alone)

**Tauri (Desktop App)**
- `npm run tauri dev` - Start the app in development mode (hot reload)
- `npm run tauri build` - Build production binary
- `npm run tauri android` - Android development (requires Android Studio)
- `npm run tauri ios` - iOS development (requires Xcode)

## 📂 Code Structure

### Frontend (`test-app/src`)
- `App.tsx`: Main logic. Handles state, UI, shortcuts, and invokes Rust commands.
- `App.css`: Styles for the editor (dark/light themes).
- `components/`: (None currently, all logic is in App.tsx)
- **Key Libraries**:
  - `@uiw/react-codemirror`: The editor component.
  - `react-markdown`: For the preview pane.
  - `@tauri-apps/api`: For communicating with the Rust backend.

### Backend (`test-app/src-tauri`)
- `src/main.rs`: **Entry point & Logic**. Contains all file system operations.
- `src/lib.rs`: Mobile entry point (currently minimal).
- `tauri.conf.json`: App configuration (permissions, windows, bundle settings).
- `capabilities/`: Permission sets (Tauri v2 security model).

## 🧩 Key Patterns

### 1. IPC (Inter-Process Communication)
The frontend calls Rust functions using `invoke("command_name", { args })`.
- **Rust Side**: Defined in `main.rs` with `#[tauri::command]`.
- **React Side**: Called in `App.tsx`.

| Command | Rust Function | Description |
| :--- | :--- | :--- |
| `get_notes` | `get_notes` | Lists `.md`/`.txt` files in `~/Documents/MyNotes` |
| `read_note` | `read_note` | Reads file content |
| `save_note` | `save_note` | Writes content to file |
| `create_note`| `create_note`| Creates empty file |
| `delete_note`| `delete_note`| Deletes file |

### 2. File Storage
- **Location**: Hardcoded to `~/Documents/MyNotes` (via `dirs` crate).
- **Behavior**: Auto-creates directory if missing.
- **Limitation**: Only supports flat structure (no subfolders).

### 3. Editor Features
- **Typewriter Mode**: Custom CodeMirror extension in `App.tsx` forces cursor to center.
- **Theming**: Toggles `data-theme` attribute on `<html>`.

## ⚠️ Gotchas & Limitations

1. **Path Handling**: The backend manually constructs paths using `dirs::document_dir()`. It does **not** use Tauri's `fs` scope scope permissions in `tauri.conf.json`, but rather raw Rust `std::fs`. This bypasses Tauri's allowlist but requires careful security handling.
2. **Security**: Input filenames are directly appended to paths. Potential path traversal risk if not sanitized (though `read_dir` listing limits what users see).
3. **State Sync**: Frontend state (`files`, `activeFile`) is manually synced after operations. No file watchers implemented.
4. **Mobile Support**: `lib.rs` exists but `main.rs` contains the logic. Mobile builds might fail because `dirs` crate behavior on mobile needs verification, and logic is currently in `main` not `lib`.

## 🧪 Testing

- **Manual Testing**: Run `npm run tauri dev` and interact with the UI.
- **Unit Tests**: None currently implemented.

## 📦 Build & Release

- GitHub Actions workflow defined in `.github/workflows/release.yml`.
- builds for macOS (Universal) and Windows.
- Linux builds require system dependencies (`libwebkit2gtk-4.0-dev`, etc.).
