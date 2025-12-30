// src/App.tsx
import { useState, useEffect, useRef, useMemo } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./App.css";

import CodeMirror from "@uiw/react-codemirror";
import { markdown } from "@codemirror/lang-markdown";
import { githubDark } from "@uiw/codemirror-theme-github";
import { EditorView, ViewPlugin, ViewUpdate } from "@codemirror/view";

function App() {
  // === 状态管理 ===
  const [files, setFiles] = useState<string[]>([]);
  const [activeFile, setActiveFile] = useState<string>("");
  const [content, setContent] = useState<string>("");
  
  // 视窗控制状态
  const [sidebarWidth, setSidebarWidth] = useState(250); // 侧边栏宽度
  const [isSidebarVisible, setIsSidebarVisible] = useState(true); // 是否显示侧边栏
  const [isResizing, setIsResizing] = useState(false); // 是否正在拖拽
  const [isTypewriterMode, setIsTypewriterMode] = useState(false); // 打字机模式
  const typewriterOffsetRef = useRef<number | null>(null); // 使用 ref 存储偏移量
  const isPluginScrolling = useRef(false); // 防止插件滚动触发 scrollHandler
  const viewRef = useRef<EditorView | null>(null); // 存储编辑器实例

  // 新建文件状态
  const [isCreating, setIsCreating] = useState(false); // 是否正在输入新文件名
  const [newFileName, setNewFileName] = useState("");

  const sidebarRef = useRef<HTMLDivElement>(null);

  // === 打字机模式扩展 ===
  const typewriterExtension = useMemo(() => {
    if (!isTypewriterMode) return [];

    // 监听滚动事件，更新打字机模式的固定位置
    const scrollHandler = EditorView.domEventHandlers({
        scroll: (event, view) => {
             // 如果是插件触发的滚动，忽略
             if (isPluginScrolling.current) return;

             const cursor = view.state.selection.main.head;
             const lineBlock = view.lineBlockAt(cursor);
             // 计算当前光标行距离视口顶部的距离
             const currentOffset = lineBlock.top - view.scrollDOM.scrollTop;
             
             // 只有当光标在视口内时才更新偏移量 (防止滚动到很远的地方导致锁定位置异常)
             const rect = view.dom.getBoundingClientRect();
             if (currentOffset >= 0 && currentOffset <= rect.height) {
                 typewriterOffsetRef.current = currentOffset;
             }
        }
    });

    // 使用 ViewPlugin 和 requestMeasure 来实现精确的滚动控制
    const typewriterScroll = ViewPlugin.fromClass(class {
        update(update: ViewUpdate) {
            // 只有在文档改变或光标移动时才触发
            if (update.docChanged || update.selectionSet) {
                const offset = typewriterOffsetRef.current;
                
                update.view.requestMeasure({
                    read: (view) => {
                        const cursor = view.state.selection.main.head;
                        const lineBlock = view.lineBlockAt(cursor);
                        const rect = view.dom.getBoundingClientRect();
                        const editorHeight = rect.height;
                        
                        let targetY;
                        if (offset !== null) {
                            // 锁定在 offset 位置
                            targetY = lineBlock.top - offset;
                        } else {
                            // 默认居中
                            targetY = (lineBlock.top + lineBlock.height / 2) - (editorHeight / 2);
                        }
                        return { targetY };
                    },
                    write: (measure, view) => {
                        if (Math.abs(view.scrollDOM.scrollTop - measure.targetY) > 1) {
                             isPluginScrolling.current = true;
                             view.scrollDOM.scrollTop = measure.targetY;
                             // 重置标志位
                             setTimeout(() => { isPluginScrolling.current = false; }, 50);
                        }
                    }
                });
            }
        }
    });

    const typewriterTheme = EditorView.theme({
      ".cm-content": {
        paddingTop: "80vh",
        paddingBottom: "80vh"
      }
    });

    return [typewriterScroll, typewriterTheme, scrollHandler];
  }, [isTypewriterMode]);

  // 切换打字机模式时的逻辑
  useEffect(() => {
    if (isTypewriterMode) {
        // 开启时：重置为居中，并立即执行一次居中滚动
        typewriterOffsetRef.current = null;
        if (viewRef.current) {
            const cursor = viewRef.current.state.selection.main.head;
            viewRef.current.dispatch({
                effects: EditorView.scrollIntoView(cursor, { y: "center" })
            });
        }
    } else {
        // 关闭时：重置
        typewriterOffsetRef.current = null;
    }
  }, [isTypewriterMode]);

  // 重置偏移量当关闭打字机模式时
  useEffect(() => {
    if (!isTypewriterMode) typewriterOffsetRef.current = null;
  }, [isTypewriterMode]);

  // === 初始化 ===
  useEffect(() => { refreshFileList(); }, []);

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
    
    // 自动补全 .md 后缀，如果用户没有输入后缀，或者输入的后缀不是 .md 或 .txt
    let finalName = newFileName;
    if (!finalName.endsWith(".md") && !finalName.endsWith(".txt")) {
        finalName = `${finalName}.md`;
    }

    try {
      await invoke("create_note", { filename: finalName });
      await refreshFileList(); // 刷新列表
      handleFileClick(finalName); // 自动选中新文件
      setIsCreating(false);
      setNewFileName("");
    } catch (err) {
      alert("创建失败: " + err); // 简单报错
    }
  };

  // === 删除文件逻辑 ===
  const handleDeleteFile = async (e: React.MouseEvent, filename: string) => {
    e.stopPropagation(); // 防止触发文件点击
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
    // 限制最小宽度 150px，最大宽度 600px
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
                onClick={() => setIsTypewriterMode(!isTypewriterMode)} 
                title={isTypewriterMode ? "关闭打字机模式" : "开启打字机模式"}
                style={{ color: isTypewriterMode ? '#007acc' : 'inherit' }}
            >
                ⌨ 打字机
            </button>
            <button 
                className="icon-btn" 
                onClick={() => {
                    setIsCreating(true);
                    if (!isSidebarVisible) setIsSidebarVisible(true);
                }} 
                title="新建文件"
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
            {/* 侧边栏头部：只保留标题 */}
            <div className="sidebar-header">
                <span>我的笔记</span>
            </div>

            {/* 新建文件的输入框 */}
            {isCreating && (
                <input
                autoFocus
                className="new-file-input"
                placeholder="输入文件名按回车..."
                value={newFileName}
                onChange={(e) => setNewFileName(e.target.value)}
                onBlur={() => setIsCreating(false)} // 失去焦点取消
                onKeyDown={(e) => {
                    if (e.key === "Enter") handleCreateFile();
                    if (e.key === "Escape") setIsCreating(false);
                }}
                />
            )}

            {/* 文件列表 */}
            <div style={{ flex: 1, overflowY: "auto" }}>
                {files.map((file) => (
                <div 
                    key={file} 
                    className="file-item"
                    onClick={() => handleFileClick(file)}
                    style={{ 
                    backgroundColor: activeFile === file ? '#37373d' : 'transparent',
                    fontWeight: activeFile === file ? 'bold' : 'normal',
                    color: activeFile === file ? '#fff' : '#aaa',
                    display: 'flex',
                    justifyContent: 'space-between',
                    alignItems: 'center',
                    paddingRight: '10px'
                    }}
                >
                    <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        📄 {file}
                    </span>
                    <button 
                        className="delete-btn"
                        onClick={(e) => handleDeleteFile(e, file)}
                        style={{ 
                            background: 'none', 
                            border: 'none', 
                            color: '#888', 
                            cursor: 'pointer',
                            fontSize: '12px',
                            padding: '2px 5px',
                            marginLeft: '5px'
                        }}
                        title="删除"
                        onMouseEnter={(e) => e.currentTarget.style.color = '#ff4d4f'}
                        onMouseLeave={(e) => e.currentTarget.style.color = '#888'}
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
                    <h1 style={{ padding: '0 20px', fontSize: '1.2rem', color: '#888' }}>{activeFile}</h1>
                    <div style={{ flex: 1, overflow: 'auto' }}>
                        <CodeMirror
                        value={content}
                        height="100%"
                        theme={githubDark}
                        extensions={[markdown(), ...typewriterExtension]}
                        onChange={saveContent}
                        onCreateEditor={(view) => { viewRef.current = view; }}
                        style={{ fontSize: '16px' }}
                        />
                    </div>
                </div>
            ) : (
                <div style={{ padding: 50, color: '#666', textAlign: 'center' }}>
                    <h2>👋 欢迎回来</h2>
                    <p>点击上方 "+" 创建新笔记</p>
                </div>
            )}
        </div>
      </div>
    </div>
  );
}

export default App;