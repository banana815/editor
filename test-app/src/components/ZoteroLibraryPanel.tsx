import React, { useMemo, useState } from "react";
import type { ZoteroCollectionNode, ZoteroLibraryItem, ZoteroSource } from "../api/zotero";
import type { CitationStyle } from "../utils/citationFormat";

interface ZoteroLibraryPanelProps {
  isOpen: boolean;
  mode: "endnotes" | "citation";
  source: ZoteroSource;
  endpoint: string;
  sqliteDbPath: string;
  collectionsDbPath: string;
  loading: boolean;
  collectionsLoading: boolean;
  error: string;
  collectionsError: string;
  items: ZoteroLibraryItem[];
  collections: ZoteroCollectionNode[];
  citationStyle: CitationStyle;
  activeCollectionId: number | null;
  onClose: () => void;
  onSourceChange: (source: ZoteroSource) => void;
  onEndpointChange: (endpoint: string) => void;
  onSqliteDbPathChange: (dbPath: string) => void;
  onRefresh: () => void;
  onRefreshCollections: () => void;
  onSelectCollection: (collectionId: number | null) => void;
  onCitationStyleChange: (style: CitationStyle) => void;
  onImportEndnotes: (items: ZoteroLibraryItem[]) => void;
  onInsertCitation: (item: ZoteroLibraryItem) => void;
}

const CollectionTree: React.FC<{
  nodes: ZoteroCollectionNode[];
  expanded: Set<number>;
  activeId: number | null;
  onToggle: (id: number) => void;
  onSelect: (id: number) => void;
}> = ({ nodes, expanded, activeId, onToggle, onSelect }) => {
  if (nodes.length === 0) return null;

  return (
    <ul className="zotero-tree">
      {nodes.map((node) => {
        const isExpanded = expanded.has(node.id);
        const hasChildren = node.children.length > 0;
        const isActive = activeId === node.id;

        return (
          <li key={node.id}>
            <div className={`zotero-tree-row ${isActive ? "active" : ""}`}>
              {hasChildren ? (
                <button
                  className="zotero-tree-toggle"
                  type="button"
                  onClick={() => onToggle(node.id)}
                  title={isExpanded ? "折叠" : "展开"}
                >
                  {isExpanded ? "▾" : "▸"}
                </button>
              ) : (
                <span className="zotero-tree-toggle placeholder">•</span>
              )}
              <button className="zotero-tree-label" type="button" onClick={() => onSelect(node.id)}>
                {node.name}
              </button>
              <span className="zotero-tree-count">{node.item_count}</span>
            </div>

            {hasChildren && isExpanded ? (
              <CollectionTree
                nodes={node.children}
                expanded={expanded}
                activeId={activeId}
                onToggle={onToggle}
                onSelect={onSelect}
              />
            ) : null}
          </li>
        );
      })}
    </ul>
  );
};

export const ZoteroLibraryPanel: React.FC<ZoteroLibraryPanelProps> = ({
  isOpen,
  mode,
  source,
  endpoint,
  sqliteDbPath,
  collectionsDbPath,
  loading,
  collectionsLoading,
  error,
  collectionsError,
  items,
  collections,
  citationStyle,
  activeCollectionId,
  onClose,
  onSourceChange,
  onEndpointChange,
  onSqliteDbPathChange,
  onRefresh,
  onRefreshCollections,
  onSelectCollection,
  onCitationStyleChange,
  onImportEndnotes,
  onInsertCitation,
}) => {
  const [keyword, setKeyword] = useState("");
  const [selectedIds, setSelectedIds] = useState<Set<string>>(new Set());
  const [selectedSingleId, setSelectedSingleId] = useState("");
  const [expandedCollectionIds, setExpandedCollectionIds] = useState<Set<number>>(new Set());

  const filteredItems = useMemo(() => {
    const term = keyword.trim().toLowerCase();
    if (!term) return items;
    return items.filter((item) => {
      const haystack = `${item.title} ${item.author} ${item.year} ${item.cite_key} ${item.item_key}`.toLowerCase();
      return haystack.includes(term);
    });
  }, [items, keyword]);

  if (!isOpen) return null;

  const toggleSelect = (id: string) => {
    setSelectedIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) {
        next.delete(id);
      } else {
        next.add(id);
      }
      return next;
    });
  };

  const selectFiltered = () => {
    const ids = filteredItems.map((x) => x.id || x.item_key).filter(Boolean);
    setSelectedIds(new Set(ids));
  };

  const clearSelected = () => setSelectedIds(new Set());

  const importSelected = () => {
    const selectedItems = items.filter((item) => selectedIds.has(item.id || item.item_key));
    onImportEndnotes(selectedItems);
  };

  const insertSingleCitation = () => {
    const selected = items.find((item) => (item.id || item.item_key) === selectedSingleId);
    if (!selected) return;
    onInsertCitation(selected);
  };

  const toggleCollection = (id: number) => {
    setExpandedCollectionIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) {
        next.delete(id);
      } else {
        next.add(id);
      }
      return next;
    });
  };

  return (
    <div className="zotero-panel-overlay" role="dialog" aria-modal="true">
      <div className="zotero-panel">
        <div className="zotero-panel__header">
          <h3>Zotero 文献库</h3>
          <button className="icon-btn" onClick={onClose} type="button">关闭</button>
        </div>

        <div className="zotero-panel__controls">
          <label>
            数据源
            <select
              value={source}
              onChange={(event) => onSourceChange(event.target.value as ZoteroSource)}
              className="zotero-input"
            >
              <option value="bbt-local-api">Better BibTeX 本地 API</option>
              <option value="sqlite-readonly">SQLite 只读</option>
            </select>
          </label>

          {source === "bbt-local-api" ? (
            <label>
              API 地址
              <input
                className="zotero-input"
                value={endpoint}
                onChange={(event) => onEndpointChange(event.target.value)}
                placeholder="http://127.0.0.1:23119/better-bibtex/export/library?/translator=csljson"
              />
            </label>
          ) : (
            <label>
              SQLite 路径
              <input
                className="zotero-input"
                value={sqliteDbPath}
                onChange={(event) => onSqliteDbPathChange(event.target.value)}
                placeholder="~/Zotero/zotero.sqlite"
              />
            </label>
          )}

          <div className="zotero-panel__actions">
            <button className="icon-btn" onClick={onRefresh} type="button" disabled={loading}>
              {loading ? "读取中..." : "刷新文献库"}
            </button>
            <select
              value={citationStyle}
              onChange={(event) => onCitationStyleChange(event.target.value as CitationStyle)}
              className="zotero-input"
              title="尾注样式"
            >
              <option value="apa">APA</option>
              <option value="ieee">IEEE</option>
              <option value="gb-t-7714">GB/T 7714</option>
            </select>
            <button className="icon-btn" onClick={selectFiltered} type="button">全选过滤结果</button>
            <button className="icon-btn" onClick={clearSelected} type="button">清空选择</button>
          </div>
        </div>

        <div className="zotero-panel__content-grid">
          <aside className="zotero-panel__sidebar">
            <div className="zotero-panel__sidebar-header">
              <strong>目录树</strong>
              <button className="icon-btn" type="button" onClick={onRefreshCollections} disabled={collectionsLoading}>
                {collectionsLoading ? "读取中..." : "刷新目录"}
              </button>
            </div>
            {collectionsDbPath ? <div className="zotero-panel__db-hint">DB: {collectionsDbPath}</div> : null}
            {collectionsError ? <div className="zotero-panel__error">{collectionsError}</div> : null}
            <div className="zotero-tree-wrapper">
              <button
                className={`zotero-tree-root ${activeCollectionId === null ? "active" : ""}`}
                type="button"
                onClick={() => onSelectCollection(null)}
              >
                全部文献
              </button>
              <CollectionTree
                nodes={collections}
                expanded={expandedCollectionIds}
                activeId={activeCollectionId}
                onToggle={toggleCollection}
                onSelect={onSelectCollection}
              />
            </div>
          </aside>

          <section className="zotero-panel__main">
            <div className="zotero-panel__search-row">
              <input
                className="zotero-input"
                placeholder="按标题、作者、年份、citekey 搜索"
                value={keyword}
                onChange={(event) => setKeyword(event.target.value)}
              />
              <span className="zotero-panel__count">{filteredItems.length} 条</span>
            </div>

            {error ? <div className="zotero-panel__error">{error}</div> : null}

            <div className="zotero-panel__list">
              {filteredItems.map((item) => {
                const id = item.id || item.item_key;
                const checked = selectedIds.has(id);
                return (
                  <label key={id} className={`zotero-row ${checked ? "active" : ""}`}>
                    {mode === "endnotes" ? (
                      <input type="checkbox" checked={checked} onChange={() => toggleSelect(id)} />
                    ) : (
                      <input
                        type="radio"
                        checked={selectedSingleId === id}
                        onChange={() => setSelectedSingleId(id)}
                        name="citation-single-select"
                      />
                    )}
                    <div className="zotero-row__body">
                      <div className="zotero-row__title">{item.title || "未命名文献"}</div>
                      <div className="zotero-row__meta">
                        <span>{item.author || "未知作者"}</span>
                        <span>{item.year || "n.d."}</span>
                        <span>{item.cite_key || item.item_key || "无 key"}</span>
                      </div>
                    </div>
                  </label>
                );
              })}
            </div>
          </section>
        </div>

        <div className="zotero-panel__footer">
          {mode === "endnotes" ? (
            <button className="icon-btn" onClick={importSelected} type="button" disabled={selectedIds.size === 0}>
              一键导入尾注引用（{selectedIds.size}）
            </button>
          ) : (
            <button className="icon-btn" onClick={insertSingleCitation} type="button" disabled={!selectedSingleId}>
              插入引用块
            </button>
          )}
        </div>
      </div>
    </div>
  );
};
