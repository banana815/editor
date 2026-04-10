import React from "react";
import type { Editor } from "@tiptap/react";
import {
  OPEN_ZOTERO_CITATION_PICKER_EVENT,
  type OpenZoteroCitationPickerDetail,
} from "../utils/editorEvents";

interface ToolbarProps {
  isSidebarVisible: boolean;
  setIsSidebarVisible: (visible: boolean) => void;
  theme: "dark" | "light";
  setTheme: (theme: "dark" | "light") => void;
  isMobile: boolean;
  isTypewriterMode: boolean;
  setIsTypewriterMode: (mode: boolean) => void;
  setIsCreating: (creating: boolean) => void;
  refreshFileList: () => void;
  editor: Editor | null;
  onExportJson: () => void;
}

export const Toolbar: React.FC<ToolbarProps> = ({
  isSidebarVisible,
  setIsSidebarVisible,
  theme,
  setTheme,
  isMobile,
  isTypewriterMode,
  setIsTypewriterMode,
  setIsCreating,
  refreshFileList,
  editor,
  onExportJson,
}) => {
  const setLink = () => {
    if (!editor) return;

    const previousUrl = editor.getAttributes("link").href as string | undefined;
    const nextUrl = window.prompt("输入链接 URL", previousUrl ?? "https://");

    if (nextUrl === null) return;

    if (nextUrl.trim() === "") {
      editor.chain().focus().extendMarkRange("link").unsetLink().run();
      return;
    }

    editor.chain().focus().extendMarkRange("link").setLink({ href: nextUrl.trim() }).run();
  };

  return (
    <div className="toolbar">
      <div className="toolbar-left">
        <button
          className="icon-btn"
          onClick={() => setIsSidebarVisible(!isSidebarVisible)}
          title={isSidebarVisible ? "隐藏侧边栏" : "展开侧边栏"}
        >
          {isSidebarVisible ? "◀" : "▶"}
        </button>
        <span className="app-title">Zotero Note Lab</span>
      </div>
      <div className="toolbar-right">
        <button
          className={`icon-btn ${editor?.isActive("bold") ? "active" : ""}`}
          onClick={() => editor?.chain().focus().toggleBold().run()}
          title="加粗"
        >
          B
        </button>
        <button
          className={`icon-btn ${editor?.isActive("italic") ? "active" : ""}`}
          onClick={() => editor?.chain().focus().toggleItalic().run()}
          title="斜体"
        >
          I
        </button>
        <button
          className={`icon-btn ${editor?.isActive("underline") ? "active" : ""}`}
          onClick={() => editor?.chain().focus().toggleUnderline().run()}
          title="下划线"
        >
          U
        </button>
        <button
          className={`icon-btn ${editor?.isActive("highlight") ? "active" : ""}`}
          onClick={() => editor?.chain().focus().toggleHighlight().run()}
          title="高亮"
        >
          H
        </button>
        <button
          className="icon-btn"
          onClick={() => editor?.chain().focus().setColor("#c74343").run()}
          title="红色文字"
        >
          A
        </button>
        <button className="icon-btn" onClick={setLink} title="插入链接">
          🔗
        </button>
        <button
          className={`icon-btn ${editor?.isActive("bulletList") ? "active" : ""}`}
          onClick={() => editor?.chain().focus().toggleBulletList().run()}
          title="无序列表"
        >
          • 列表
        </button>
        <button
          className={`icon-btn ${editor?.isActive("orderedList") ? "active" : ""}`}
          onClick={() => editor?.chain().focus().toggleOrderedList().run()}
          title="有序列表"
        >
          1. 列表
        </button>
        <button
          className="icon-btn"
          onClick={() => {
            if (!editor) return;
            const selectionText = editor.state.doc
              .textBetween(editor.state.selection.from, editor.state.selection.to, " ")
              .trim();
            const detail: OpenZoteroCitationPickerDetail = { note: selectionText };
            window.dispatchEvent(new CustomEvent(OPEN_ZOTERO_CITATION_PICKER_EVENT, { detail }));
          }}
          title="插入 Zotero 引用块"
        >
          引用块
        </button>
        <button
          className="icon-btn"
          onClick={() => editor?.chain().focus().unsetAllMarks().clearNodes().run()}
          title="清除格式"
        >
          橡皮擦
        </button>
        <button
          className="icon-btn"
          onClick={() => setTheme(theme === "dark" ? "light" : "dark")}
          title={theme === "dark" ? "切换到浅色模式" : "切换到深色模式"}
        >
          {theme === "dark" ? "☀" : "🌙"}
        </button>
        {!isMobile && (
          <>
            <button
              className={`icon-btn ${isTypewriterMode ? "active" : ""}`}
              onClick={() => setIsTypewriterMode(!isTypewriterMode)}
              title={isTypewriterMode ? "关闭打字机模式" : "开启打字机模式"}
            >
              ⌨ 打字机
            </button>
          </>
        )}
        <button className="icon-btn" onClick={onExportJson} title="导出 TipTap JSON">
          JSON
        </button>
        <button
          className="icon-btn"
          onClick={() => {
            setIsCreating(true);
            if (!isSidebarVisible) setIsSidebarVisible(true);
          }}
          title="新建文件 (Ctrl+N)"
        >
          ➕ <span className="btn-text">新建</span>
        </button>
        <button className="icon-btn" onClick={refreshFileList} title="刷新列表">
          ↻
        </button>
      </div>
    </div>
  );
};