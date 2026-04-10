// src/App.tsx
import { lazy, Suspense, useState, useEffect, useRef } from "react";
import "./App.css";

import { useMobileDetect } from "./hooks/useMobileDetect";
import { useKeyboardShortcuts } from "./hooks/useKeyboardShortcuts";
import { useDebouncedSave } from "./hooks/useDebouncedSave";

import { Toolbar } from "./components/Toolbar";
import { Sidebar } from "./components/Sidebar";

import type { Editor } from "@tiptap/react";
import { noteApi } from "./api/notes";

const LazyEditorArea = lazy(async () => {
  const mod = await import("./components/EditorArea");
  return { default: mod.EditorArea };
});

function App() {
  // === 状态管理 ===
  const [theme, setTheme] = useState<'dark' | 'light'>('light');
  const [files, setFiles] = useState<string[]>([]);
  const [activeFile, setActiveFile] = useState<string>("");
  const [content, setContent] = useState<string>("");
  const [editorJson, setEditorJson] = useState<string>("");
  const [editorInstance, setEditorInstance] = useState<Editor | null>(null);
  
  // 视窗控制状态
  const [sidebarWidth, setSidebarWidth] = useState(250);
  const [isSidebarVisible, setIsSidebarVisible] = useState(true);
  const [isResizing, setIsResizing] = useState(false);
  const [isTypewriterMode, setIsTypewriterMode] = useState(false);
  const [isMobile, setIsMobile] = useState(false);

  // 新建文件状态
  const [isCreating, setIsCreating] = useState(false);
  const [newFileName, setNewFileName] = useState("");

  const sidebarRef = useRef<HTMLDivElement>(null);

  // === 响应式检测 ===
  const isMobileDetected = useMobileDetect();
  useEffect(() => {
    setIsMobile(isMobileDetected);
    if (isMobileDetected) {
      setIsSidebarVisible(false);
      setSidebarWidth(window.innerWidth * 0.8);
    }
  }, [isMobileDetected]);

  // === 自动保存逻辑 ===
  const saveContent = useDebouncedSave(activeFile, async (filename, content) => {
    setContent(content);
    await noteApi.saveNote(filename, content);
  });

  useEffect(() => {
    if (isTypewriterMode && editorInstance) {
      editorInstance.commands.focus("end");
      editorInstance.commands.scrollIntoView();
    }
  }, [isTypewriterMode, editorInstance]);

  // === 初始化 ===
  useEffect(() => { refreshFileList(); }, []);

  // === 主题切换 ===
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
  }, [theme]);

  // === 快捷键监听 ===
  useKeyboardShortcuts(
    activeFile,
    content,
    async (file, txt) => {
      await noteApi.saveNote(file, txt);
    },
    () => {
      setIsCreating(true);
      if (!isSidebarVisible) setIsSidebarVisible(true);
    }
  );

  // === 核心逻辑函数 ===
  const refreshFileList = async () => {
    try {
      const list = await noteApi.getNotes();
      setFiles(list);
    } catch (e) { console.error(e); }
  };

  const handleFileClick = async (filename: string) => {
    setActiveFile(filename);
    const text = await noteApi.readNote(filename);
    setContent(text);
    setEditorJson("");
    if (isMobile) setIsSidebarVisible(false); // 移动端点击文件后收起侧边栏
  };

  // === 新建文件逻辑 ===
  const handleCreateFile = async () => {
    if (!newFileName.trim()) { setIsCreating(false); return; }
    
    let finalName = newFileName;
    if (!finalName.endsWith(".md") && !finalName.endsWith(".txt")) {
        finalName = `${finalName}.md`;
    }

    try {
      await noteApi.createNote(finalName);
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
      await noteApi.deleteNote(filename);
      await refreshFileList();
      if (activeFile === filename) {
        setActiveFile("");
        setContent("");
        setEditorJson("");
      }
    } catch (err) {
      alert("删除失败: " + err);
    }
  };

  const handleExportJson = () => {
    if (!activeFile || !editorJson) {
      alert("当前没有可导出的编辑内容");
      return;
    }

    const blob = new Blob([editorJson], { type: "application/json;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    const safeName = activeFile.replace(/\.(md|txt)$/i, "");
    link.href = url;
    link.download = `${safeName || "note"}.json`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
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

  useEffect(() => {
    if (!activeFile) {
      setEditorInstance(null);
    }
  }, [activeFile]);

  return (
    <div className="container" style={{ userSelect: isResizing ? "none" : "auto" }}>
      <Toolbar
        isSidebarVisible={isSidebarVisible}
        setIsSidebarVisible={setIsSidebarVisible}
        theme={theme}
        setTheme={setTheme}
        isMobile={isMobile}
        isTypewriterMode={isTypewriterMode}
        setIsTypewriterMode={setIsTypewriterMode}
        setIsCreating={setIsCreating}
        refreshFileList={refreshFileList}
        editor={editorInstance}
        onExportJson={handleExportJson}
      />

      <div className="main-content">
        <Sidebar
          isSidebarVisible={isSidebarVisible}
          isMobile={isMobile}
          sidebarWidth={sidebarWidth}
          sidebarRef={sidebarRef}
          setIsSidebarVisible={setIsSidebarVisible}
          isCreating={isCreating}
          newFileName={newFileName}
          setNewFileName={setNewFileName}
          setIsCreating={setIsCreating}
          handleCreateFile={handleCreateFile}
          files={files}
          activeFile={activeFile}
          handleFileClick={handleFileClick}
          handleDeleteFile={handleDeleteFile}
        />

        {isSidebarVisible && !isMobile && (
          <div
            className={`resizer ${isResizing ? "active" : ""}`}
            onMouseDown={startResizing}
          />
        )}

        <div className="editor-area">
          {activeFile ? (
            <Suspense fallback={<div className="empty-state"><p>编辑器加载中...</p></div>}>
              <LazyEditorArea
                activeFile={activeFile}
                isMobile={isMobile}
                rawContent={content}
                theme={theme}
                isTypewriterMode={isTypewriterMode}
                saveContent={saveContent}
                onJsonChange={setEditorJson}
                onEditorReady={setEditorInstance}
              />
            </Suspense>
          ) : (
            <div className="empty-state">
              <h2>欢迎回来</h2>
              <p>点击上方 + {isMobile ? "" : "或按 Ctrl+N"} 创建新笔记</p>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

export default App;