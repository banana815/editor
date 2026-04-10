import { useEffect } from "react";

export function useKeyboardShortcuts(
  activeFile: string,
  content: string,
  onSave: (filename: string, content: string) => Promise<void>,
  onCreate: () => void
) {
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      // Ctrl+S / Command+S
      if ((e.ctrlKey || e.metaKey) && e.key === "s") {
        e.preventDefault();
        if (activeFile) {
          onSave(activeFile, content);
        }
      }
      // Ctrl+N / Command+N
      if ((e.ctrlKey || e.metaKey) && e.key === "n") {
        e.preventDefault();
        onCreate();
      }
    };

    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [activeFile, content, onSave, onCreate]);
}