import React from "react";

interface SidebarProps {
  isSidebarVisible: boolean;
  isMobile: boolean;
  sidebarWidth: number;
  sidebarRef: React.RefObject<HTMLDivElement | null>;
  setIsSidebarVisible: (visible: boolean) => void;
  isCreating: boolean;
  newFileName: string;
  setNewFileName: (name: string) => void;
  setIsCreating: (creating: boolean) => void;
  handleCreateFile: () => void;
  files: string[];
  activeFile: string;
  handleFileClick: (filename: string) => void;
  handleDeleteFile: (e: React.MouseEvent, filename: string) => void;
}

export const Sidebar: React.FC<SidebarProps> = ({
  isSidebarVisible,
  isMobile,
  sidebarWidth,
  sidebarRef,
  setIsSidebarVisible,
  isCreating,
  newFileName,
  setNewFileName,
  setIsCreating,
  handleCreateFile,
  files,
  activeFile,
  handleFileClick,
  handleDeleteFile,
}) => {
  if (!isSidebarVisible) return null;

  return (
    <div
      className={`sidebar ${isMobile ? "mobile-sidebar" : ""}`}
      style={{ width: isMobile ? "100%" : sidebarWidth }}
      ref={sidebarRef}
    >
      <div className="sidebar-header">
        <span>我的笔记</span>
        {isMobile && <button onClick={() => setIsSidebarVisible(false)}>✖</button>}
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
            className={`file-item ${activeFile === file ? "active" : ""}`}
            onClick={() => handleFileClick(file)}
          >
            <span className="file-name">📄 {file}</span>
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
  );
};