// src/App.tsx
import { useState, useEffect, useRef, useMemo } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./App.css";

import CodeMirror from "@uiw/react-codemirror";
import { markdown } from "@codemirror/lang-markdown";
import { githubDark, githubLight } from "@uiw/codemirror-theme-github";
// 移除了 ViewPlugin, ViewUpdate，因为不再需要手动写插件
import { EditorView } from "@codemirror/view";

function App() {
  // === 状态管理 ===
  const [theme, setTheme] = useState<'dark' | 'light'>('light');
  const [files, setFiles] = useState<string[]>([]);
  const [activeFile, setActiveFile] = useState<string>("");
  const [content, setContent] = useState<string>("");
  
  // 视窗控制状态
  const [sidebarWidth, setSidebarWidth] = useState(250);
  const [isSidebarVisible, setIsSidebarVisible] = useState(true);
  const [isResizing, setIsResizing] = useState(false);
  const [isTypewriterMode, setIsTypewriterMode] = useState(false);
  
  // 移除了 typewriterOffsetRef 和 isPluginScrolling，因为原生 API 不需要它们
  const viewRef = useRef<EditorView | null>(null);

  // 新建文件状态
  const [isCreating, setIsCreating] = useState(false);
  const [newFileName, setNewFileName] = useState("");

  const sidebarRef = useRef<HTMLDivElement>(null);

  // === 打字机模式扩展 (极简版) ===
  const typewriterExtension = useMemo(() => {
    if (!isTypewriterMode) return [];

    // 1. 核心逻辑：监听光标变化，强制居中
    const centerCursorListener = EditorView.updateListener.of((update) => {
      // 当光标位置改变(selectionSet) 或 文档内容改变(docChanged) 时触发
      if (update.selectionSet || update.docChanged) {
        update.view.dispatch({
          effects: EditorView.scrollIntoView(update.state.selection.main, {
            y: "center" // 这一行代码替代了之前几十行的数学计算
          })
        });
      }
    });

    // 2. 样式逻辑：给编辑器上下添加巨大的 padding，确保首尾行也能居中
    const typewriterTheme = EditorView.theme({
      ".cm-content": {
        paddingBlock: "100vh" // 使用 paddingBlock 同时设置上下，50vh 保证正中
      }
    });

    return [centerCursorListener, typewriterTheme];
  }, [isTypewriterMode]);

  // 切换打字机模式时，立即执行一次居中
  useEffect(() => {
    if (isTypewriterMode && viewRef.current) {
        const cursor = viewRef.current.state.selection.main.head;
        viewRef.current.dispatch({
            effects: EditorView.scrollIntoView(cursor, { y: "center" })
        });
    }
  }, [isTypewriterMode]);

  // === 初始化 ===
  useEffect(() => { refreshFileList(); }, []);

  // === 主题切换 ===
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);

  // === 快捷键监听 ===
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      // Ctrl+S / Command+S 保存
      if ((e.ctrlKey || e.metaKey) && e.key === 's') {
        e.preventDefault();
        if (activeFile) {
            invoke("save_note", { filename: activeFile, content });
            // 可以加个简单的提示，这里先略过
        }
      }
      // Ctrl+N / Command+N 新建
      if ((e.ctrlKey || e.metaKey) && e.key === 'n') {
        e.preventDefault();
        setIsCreating(true);
        if (!isSidebarVisible) setIsSidebarVisible(true);
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [activeFile, content, isSidebarVisible]);

  // === 核心逻辑函数 ===
  const refreshFileList = async () => {
    try {
      const list = await invoke("get_notes") as string[];
      setFiles(list);
    } catch (e) { console.error(e); }
  };

  const handleFileClick = async (filename: string) => {
    setActiveFile(filename);
    const text = await invoke("read_note", { filename }) as string;
    setContent(text);
  };

  const saveContent = (val: string) => {
    setContent(val);
    if (activeFile) invoke("save_note", { filename: activeFile, content: val });
  };

  // === 新建文件逻辑 ===
  const handleCreateFile = async () => {
    if (!newFileName.trim()) { setIsCreating(false); return; }
    
    let finalName = newFileName;
    if (!finalName.endsWith(".md") && !finalName.endsWith(".txt")) {
        finalName = `${finalName}.md`;
    }

    try {
      await invoke("create_note", { filename: finalName });
      await refreshFileList();
      handleFileClick(finalName);
      setIsCreating(false);
      setNewFileName("");
    } catch (err) {
      alert("创建失败: " + err);
    }
  };

  // === 删除文件逻辑 ===
  const handleDeleteFile = async (e: React.MouseEvent, filename: string) => {
    e.stopPropagation();
    if (!confirm(`确定要删除 ${filename} 吗？`)) return;

    try {
      await invoke("delete_note", { filename });
      await refreshFileList();
      if (activeFile === filename) {
        setActiveFile("");
        setContent("");
      }
    } catch (err) {
      alert("删除失败: " + err);
    }
  };

  // === 拖拽调整宽度逻辑 ===
  const startResizing = (e: React.MouseEvent) => {
    e.preventDefault();
    setIsResizing(true);
    document.addEventListener("mousemove", handleMouseMove);
    document.addEventListener("mouseup", stopResizing);
  };

  const handleMouseMove = (e: MouseEvent) => {
    let newWidth = e.clientX;
    if (newWidth < 150) newWidth = 150;
    if (newWidth > 600) newWidth = 600;
    setSidebarWidth(newWidth);
  };

  const stopResizing = () => {
    setIsResizing(false);
    document.removeEventListener("mousemove", handleMouseMove);
    document.removeEventListener("mouseup", stopResizing);
  };

  return (
    <div className="container" style={{ userSelect: isResizing ? "none" : "auto" }}>
      
      {/* === 顶部工具栏 === */}
      <div className="toolbar">
        <div className="toolbar-left">
            <button 
                className="icon-btn" 
                onClick={() => setIsSidebarVisible(!isSidebarVisible)} 
                title={isSidebarVisible ? "隐藏侧边栏" : "展开侧边栏"}
            >
                {isSidebarVisible ? "◀" : "▶"}
            </button>
            <span className="app-title">My Editor</span>
        </div>
        <div className="toolbar-right">
            <button 
                className="icon-btn" 
                onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')} 
                title={theme === 'dark' ? "切换到浅色模式" : "切换到深色模式"}
            >
                {theme === 'dark' ? "☀" : "🌙"}
            </button>
            <button 
                className={`icon-btn ${isTypewriterMode ? 'active' : ''}`}
                onClick={() => setIsTypewriterMode(!isTypewriterMode)} 
                title={isTypewriterMode ? "关闭打字机模式" : "开启打字机模式"}
            >
                ⌨ 打字机
            </button>
            <button 
                className="icon-btn" 
                onClick={() => {
                    setIsCreating(true);
                    if (!isSidebarVisible) setIsSidebarVisible(true);
                }} 
                title="新建文件 (Ctrl+N)"
            >
                ➕ 新建
            </button>
            <button className="icon-btn" onClick={refreshFileList} title="刷新列表">↻ 刷新</button>
        </div>
      </div>

      {/* === 主内容区 === */}
      <div className="main-content">
        {/* === 左侧侧边栏 === */}
        {isSidebarVisible && (
            <div 
            className="sidebar" 
            style={{ width: sidebarWidth }}
            ref={sidebarRef}
            >
            <div className="sidebar-header">
                <span>我的笔记</span>
            </div>

            {isCreating && (
                <input
                autoFocus
                className="new-file-input"
                placeholder="输入文件名按回车..."
                value={newFileName}
                onChange={(e) => setNewFileName(e.target.value)}
                onBlur={() => setIsCreating(false)}
                onKeyDown={(e) => {
                    if (e.key === "Enter") handleCreateFile();
                    if (e.key === "Escape") setIsCreating(false);
                }}
                />
            )}

            <div className="file-list">
                {files.map((file) => (
                <div 
                    key={file} 
                    className={`file-item ${activeFile === file ? 'active' : ''}`}
                    onClick={() => handleFileClick(file)}
                >
                    <span className="file-name">
                        📄 {file}
                    </span>
                    <button 
                        className="delete-btn"
                        onClick={(e) => handleDeleteFile(e, file)}
                        title="删除"
                    >
                        ✖
                    </button>
                </div>
                ))}
            </div>
            </div>
        )}

        {/* === 拖拽条 (Resizer) === */}
        {isSidebarVisible && (
            <div 
            className={`resizer ${isResizing ? "active" : ""}`} 
            onMouseDown={startResizing}
            />
        )}

        {/* === 右侧编辑区 === */}
        <div className="editor-area">
            {activeFile ? (
                <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
                    <div className="editor-header">{activeFile}</div>
                    <div style={{ flex: 1, overflow: 'auto' }}>
                        <CodeMirror
                        value={content}
                        height="100%"
                        theme={theme === 'dark' ? githubDark : githubLight}
                        extensions={[markdown(), ...typewriterExtension]}
                        onChange={saveContent}
                        onCreateEditor={(view) => { viewRef.current = view; }}
                        style={{ fontSize: '16px' }}
                        />
                    </div>
                </div>
            ) : (
                <div className="empty-state">
                    <h2>👋 欢迎回来</h2>
                    <p>点击上方 "+" 或按 Ctrl+N 创建新笔记</p>
                </div>
            )}
        </div>
      </div>
    </div>
  );
}

export default App;