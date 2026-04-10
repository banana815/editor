import { invoke } from "@tauri-apps/api/core";

export type ZoteroSource = "bbt-local-api" | "sqlite-readonly";

export interface ZoteroLibraryItem {
  id: string;
  item_key: string;
  title: string;
  author: string;
  year: string;
  cite_key: string;
  item_uri: string;
  pdf_uri: string;
}

export interface ZoteroCollectionNode {
  id: number;
  parent_id: number | null;
  key: string;
  name: string;
  item_count: number;
  item_keys: string[];
  children: ZoteroCollectionNode[];
}

export interface ZoteroCollectionsPayload {
  db_path: string;
  collections: ZoteroCollectionNode[];
}

export const zoteroApi = {
  fetchLibrary: (source: ZoteroSource, endpoint?: string, dbPath?: string): Promise<ZoteroLibraryItem[]> =>
    invoke("fetch_zotero_library", {
      source,
      endpoint: endpoint?.trim() ? endpoint.trim() : null,
      dbPath: dbPath?.trim() ? dbPath.trim() : null,
    }),
  fetchCollectionsTree: (dbPath?: string): Promise<ZoteroCollectionsPayload> =>
    invoke("fetch_zotero_collections_tree", {
      dbPath: dbPath?.trim() ? dbPath.trim() : null,
    }),
};
