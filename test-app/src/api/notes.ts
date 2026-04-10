import { invoke } from "@tauri-apps/api/core";

export const noteApi = {
  getNotes: (): Promise<string[]> => invoke("get_notes"),
  readNote: (filename: string): Promise<string> => invoke("read_note", { filename }),
  saveNote: (filename: string, content: string): Promise<void> => invoke("save_note", { filename, content }),
  createNote: (filename: string): Promise<string> => invoke("create_note", { filename }),
  deleteNote: (filename: string): Promise<string> => invoke("delete_note", { filename }),
};