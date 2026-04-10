import { useRef, useCallback } from "react";

export function useDebouncedSave(
  activeFile: string,
  onSave: (filename: string, content: string) => Promise<void>,
  delay: number = 1000
) {
  const autoSaveTimerRef = useRef<number | null>(null);

  const saveContent = useCallback(
    (val: string) => {
      if (autoSaveTimerRef.current) {
        window.clearTimeout(autoSaveTimerRef.current);
      }

      if (activeFile) {
        autoSaveTimerRef.current = window.setTimeout(() => {
          onSave(activeFile, val)
            .then(() => console.log(`Auto-saved ${activeFile}`))
            .catch((err) => console.error("Auto-save failed:", err));
        }, delay);
      }
    },
    [activeFile, onSave, delay]
  );

  return saveContent;
}