import React, { Suspense, useEffect, useMemo, useRef, useState } from "react";
import { EditorContent, useEditor, type Editor, type JSONContent } from "@tiptap/react";

import {
  zoteroApi,
  type ZoteroCollectionNode,
  type ZoteroLibraryItem,
  type ZoteroSource,
} from "../api/zotero";
import { formatBibliographyEntries, type CitationStyle } from "../utils/citationFormat";
import {
  OPEN_ZOTERO_CITATION_PICKER_EVENT,
  type OpenZoteroCitationPickerDetail,
} from "../utils/editorEvents";

const LazyZoteroLibraryPanel = React.lazy(async () => {
  const mod = await import("./ZoteroLibraryPanel");
  return { default: mod.ZoteroLibraryPanel };
});

const ZOTERO_CITATION_TYPE = "zoteroCitationBlock";
const ZOTERO_ENDNOTE_SECTION_TYPE = "zoteroEndnoteSection";

interface ZoteroCitationAttrs {
  citationId: string;
  endnoteNumber: number;
  itemKey: string;
  itemUri: string;
  pdfUri: string;
  title: string;
  author: string;
  year: string;
  citeKey: string;
  page: string;
  note: string;
}

interface ZoteroEndnoteEntry {
  citationId: string;
  number: number;
  entryHtml: string;
  key: string;
  itemUri: string;
  pdfUri: string;
}

interface EditorAreaProps {
  activeFile: string;
  isMobile: boolean;
  rawContent: string;
  theme: "dark" | "light";
  isTypewriterMode: boolean;
  saveContent: (val: string) => void;
  onJsonChange: (json: string) => void;
  onEditorReady: (editor: Editor | null) => void;
}

const looksLikeHtml = (value: string) => /<\/?[a-z][\s\S]*>/i.test(value);

const escapeHtml = (text: string) =>
  text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#039;");

const parseEditorContent = (value: string): string | JSONContent => {
  const input = value.trim();
  if (!input) return "<p></p>";

  try {
    const parsed = JSON.parse(input) as { type?: string; doc?: JSONContent };
    if (parsed.type === "doc") {
      return parsed as JSONContent;
    }
    if (parsed.doc && parsed.doc.type === "doc") {
      return parsed.doc;
    }
  } catch {
    // Not JSON. Continue with text/HTML parsing.
  }

  if (looksLikeHtml(input)) {
    return value;
  }

  return input
    .split(/\r?\n/)
    .map((line) => (line.trim() ? `<p>${escapeHtml(line)}</p>` : "<p><br/></p>"))
    .join("");
};

const findAllTextMatches = (editor: Editor, keyword: string) => {
  const term = keyword.trim().toLowerCase();
  const matches: Array<{ from: number; to: number }> = [];

  if (!term) return matches;

  editor.state.doc.descendants((node, pos) => {
    if (!node.isText || !node.text) return;

    const text = node.text.toLowerCase();
    let index = text.indexOf(term);
    while (index !== -1) {
      matches.push({
        from: pos + index,
        to: pos + index + term.length,
      });
      index = text.indexOf(term, index + term.length);
    }
  });

  return matches;
};

export const EditorArea: React.FC<EditorAreaProps> = ({
  activeFile,
  isMobile,
  rawContent,
  theme,
  isTypewriterMode,
  saveContent,
  onJsonChange,
  onEditorReady,
}) => {
  const editorHostRef = useRef<HTMLDivElement>(null);
  const dragHandleRef = useRef<HTMLButtonElement>(null);
  const [searchInput, setSearchInput] = useState("");
  const [searchMatches, setSearchMatches] = useState(0);
  const [isLibraryOpen, setIsLibraryOpen] = useState(false);
  const [libraryMode, setLibraryMode] = useState<"endnotes" | "citation">("endnotes");
  const [pendingCitationNote, setPendingCitationNote] = useState("");
  const [zoteroSource, setZoteroSource] = useState<ZoteroSource>("bbt-local-api");
  const [zoteroEndpoint, setZoteroEndpoint] = useState(
    "http://127.0.0.1:23119/better-bibtex/export/library?/translator=csljson",
  );
  const [sqliteDbPath, setSqliteDbPath] = useState("");
  const [zoteroItems, setZoteroItems] = useState<ZoteroLibraryItem[]>([]);
  const [zoteroLoading, setZoteroLoading] = useState(false);
  const [zoteroError, setZoteroError] = useState("");
  const [zoteroCollections, setZoteroCollections] = useState<ZoteroCollectionNode[]>([]);
  const [zoteroCollectionsDbPath, setZoteroCollectionsDbPath] = useState("");
  const [zoteroCollectionsLoading, setZoteroCollectionsLoading] = useState(false);
  const [zoteroCollectionsError, setZoteroCollectionsError] = useState("");
  const [activeCollectionId, setActiveCollectionId] = useState<number | null>(null);
  const [citationStyle, setCitationStyle] = useState<CitationStyle>("apa");
  const [loadedExtensions, setLoadedExtensions] = useState<any[] | null>(null);

  useEffect(() => {
    let active = true;

    const loadEditorExtensions = async () => {
      const [
        documentMod,
        paragraphMod,
        textMod,
        boldMod,
        italicMod,
        headingMod,
        bulletListMod,
        orderedListMod,
        listItemMod,
        historyMod,
        underlineMod,
        textStyleMod,
        colorMod,
        highlightMod,
        linkMod,
        placeholderMod,
        textAlignMod,
        citationBlockMod,
        endnoteSectionMod,
      ] = await Promise.all([
        import("@tiptap/extension-document"),
        import("@tiptap/extension-paragraph"),
        import("@tiptap/extension-text"),
        import("@tiptap/extension-bold"),
        import("@tiptap/extension-italic"),
        import("@tiptap/extension-heading"),
        import("@tiptap/extension-bullet-list"),
        import("@tiptap/extension-ordered-list"),
        import("@tiptap/extension-list-item"),
        import("@tiptap/extension-history"),
        import("@tiptap/extension-underline"),
        import("@tiptap/extension-text-style"),
        import("@tiptap/extension-color"),
        import("@tiptap/extension-highlight"),
        import("@tiptap/extension-link"),
        import("@tiptap/extension-placeholder"),
        import("@tiptap/extension-text-align"),
        import("../extensions/ZoteroCitationBlock"),
        import("../extensions/ZoteroEndnoteSection"),
      ]);

      if (!active) return;

      setLoadedExtensions([
        documentMod.default,
        paragraphMod.default,
        textMod.default,
        boldMod.default,
        italicMod.default,
        headingMod.default,
        bulletListMod.default,
        orderedListMod.default,
        listItemMod.default,
        historyMod.default,
        underlineMod.default,
        textStyleMod.TextStyle,
        colorMod.default,
        highlightMod.default.configure({ multicolor: true }),
        linkMod.default.configure({ openOnClick: false, autolink: true }),
        textAlignMod.default.configure({ types: ["heading", "paragraph"] }),
        citationBlockMod.ZoteroCitationBlock,
        endnoteSectionMod.ZoteroEndnoteSection,
        placeholderMod.default.configure({ placeholder: "在这里输入你的笔记..." }),
      ]);
    };

    void loadEditorExtensions();

    return () => {
      active = false;
    };
  }, []);

  const editor = useEditor({
    extensions: loadedExtensions ?? [],
    content: "<p></p>",
    autofocus: loadedExtensions ? false : undefined,
    editorProps: {
      attributes: {
        class: "zotero-prosemirror",
      },
    },
    onUpdate: ({ editor: instance }) => {
      const html = instance.getHTML();
      saveContent(html);
      onJsonChange(JSON.stringify(instance.getJSON(), null, 2));
    },
  }, [loadedExtensions]);

  useEffect(() => {
    onEditorReady(editor ?? null);
    return () => onEditorReady(null);
  }, [editor, onEditorReady]);

  useEffect(() => {
    if (!editor || !activeFile) return;

    const nextContent = parseEditorContent(rawContent);
    editor.commands.setContent(nextContent, { emitUpdate: false });
    onJsonChange(JSON.stringify(editor.getJSON(), null, 2));
    setSearchInput("");
    setSearchMatches(0);
  }, [activeFile, rawContent, editor, onJsonChange]);

  useEffect(() => {
    const onOpenPicker = (event: Event) => {
      const customEvent = event as CustomEvent<OpenZoteroCitationPickerDetail>;
      const detail = customEvent.detail;

      setLibraryMode("citation");
      setPendingCitationNote(detail?.note || "");
      setIsLibraryOpen(true);

      if (zoteroItems.length === 0 && !zoteroLoading) {
        void refreshZoteroLibrary();
      }
      if (zoteroCollections.length === 0 && !zoteroCollectionsLoading) {
        void refreshZoteroCollections();
      }
    };

    window.addEventListener(OPEN_ZOTERO_CITATION_PICKER_EVENT, onOpenPicker as EventListener);
    return () => {
      window.removeEventListener(OPEN_ZOTERO_CITATION_PICKER_EVENT, onOpenPicker as EventListener);
    };
  }, [zoteroItems.length, zoteroLoading, zoteroCollections.length, zoteroCollectionsLoading]);

  useEffect(() => {
    if (!editor) return;

    const host = editorHostRef.current;
    const prose = host?.querySelector(".ProseMirror") as HTMLElement | null;
    const handle = dragHandleRef.current;

    if (!host || !prose || !handle) return;

    let hoveredIndex: number | null = null;
    let draggedIndex: number | null = null;
    let dropIndex: number | null = null;

    const getBlockElement = (target: EventTarget | null): HTMLElement | null => {
      if (!(target instanceof HTMLElement)) return null;
      const block = target.closest(".ProseMirror > *") as HTMLElement | null;
      if (!block || block.parentElement !== prose) return null;
      return block;
    };

    const clearDropClasses = () => {
      Array.from(prose.children).forEach((item) => {
        (item as HTMLElement).classList.remove("drop-target");
      });
    };

    const refreshBlockIndexes = () => {
      Array.from(prose.children).forEach((child, index) => {
        (child as HTMLElement).dataset.blockIndex = String(index);
      });
    };

    const hideHandle = () => {
      handle.style.opacity = "0";
      hoveredIndex = null;
    };

    const showHandleForBlock = (block: HTMLElement, index: number) => {
      const hostBox = host.getBoundingClientRect();
      const blockBox = block.getBoundingClientRect();
      const top = blockBox.top - hostBox.top + blockBox.height / 2 - 11;

      handle.style.top = `${Math.max(8, top)}px`;
      handle.style.opacity = "1";
      hoveredIndex = index;
    };

    const moveBlock = (from: number, to: number) => {
      const doc = editor.getJSON();
      const list = [...(doc.content ?? [])];
      if (from < 0 || to < 0 || from >= list.length || to >= list.length || from === to) return;

      const [node] = list.splice(from, 1);
      list.splice(to, 0, node);
      editor.commands.setContent({ type: "doc", content: list }, { emitUpdate: true });
    };

    const handleMouseMove = (event: MouseEvent) => {
      refreshBlockIndexes();
      const block = getBlockElement(event.target);
      if (!block) {
        hideHandle();
        return;
      }

      const index = Number.parseInt(block.dataset.blockIndex ?? "", 10);
      if (Number.isNaN(index)) {
        hideHandle();
        return;
      }

      showHandleForBlock(block, index);
    };

    const handleMouseLeave = () => {
      if (draggedIndex === null) {
        hideHandle();
        clearDropClasses();
      }
    };

    const onHandleDragStart = (event: DragEvent) => {
      if (hoveredIndex === null) {
        event.preventDefault();
        return;
      }

      draggedIndex = hoveredIndex;
      if (event.dataTransfer) {
        event.dataTransfer.effectAllowed = "move";
        event.dataTransfer.setData("text/plain", String(hoveredIndex));
      }
    };

    const onHandleDragEnd = () => {
      draggedIndex = null;
      dropIndex = null;
      clearDropClasses();
      hideHandle();
    };

    const handleDragOver = (event: DragEvent) => {
      if (draggedIndex === null) return;
      event.preventDefault();

      const block = getBlockElement(event.target);
      clearDropClasses();
      if (!block) return;

      const index = Number.parseInt(block.dataset.blockIndex ?? "", 10);
      if (Number.isNaN(index)) return;

      dropIndex = index;
      block.classList.add("drop-target");
    };

    const handleDrop = (event: DragEvent) => {
      if (draggedIndex === null) return;
      event.preventDefault();

      if (dropIndex !== null) {
        moveBlock(draggedIndex, dropIndex);
      }

      draggedIndex = null;
      dropIndex = null;
      clearDropClasses();
      hideHandle();
      refreshBlockIndexes();
    };

    const stopNativeDrop = (event: DragEvent) => {
      if (draggedIndex !== null) {
        event.preventDefault();
      }
    };

    refreshBlockIndexes();
    prose.addEventListener("mousemove", handleMouseMove);
    prose.addEventListener("mouseleave", handleMouseLeave);
    prose.addEventListener("dragover", handleDragOver);
    prose.addEventListener("drop", handleDrop);
    prose.addEventListener("dragenter", stopNativeDrop);
    handle.addEventListener("dragstart", onHandleDragStart);
    handle.addEventListener("dragend", onHandleDragEnd);

    return () => {
      prose.removeEventListener("mousemove", handleMouseMove);
      prose.removeEventListener("mouseleave", handleMouseLeave);
      prose.removeEventListener("dragover", handleDragOver);
      prose.removeEventListener("drop", handleDrop);
      prose.removeEventListener("dragenter", stopNativeDrop);
      handle.removeEventListener("dragstart", onHandleDragStart);
      handle.removeEventListener("dragend", onHandleDragEnd);
    };
  }, [editor]);

  const goToSearchResult = (direction: "next" | "prev") => {
    if (!editor) return;

    const matches = findAllTextMatches(editor, searchInput);
    setSearchMatches(matches.length);
    if (matches.length === 0) return;

    const currentFrom = editor.state.selection.from;
    const sorted = direction === "next" ? matches : [...matches].reverse();

    const target =
      sorted.find((item) =>
        direction === "next" ? item.from > currentFrom : item.to < currentFrom,
      ) ?? sorted[0];

    editor.chain().focus().setTextSelection(target).scrollIntoView().run();
  };

  const collectionItemKeySet = useMemo(() => {
    if (activeCollectionId === null) return null;

    const walk = (nodes: ZoteroCollectionNode[]): Set<string> => {
      const bucket = new Set<string>();
      for (const node of nodes) {
        for (const key of node.item_keys) bucket.add(key);
        const childKeys = walk(node.children);
        childKeys.forEach((key) => bucket.add(key));
      }
      return bucket;
    };

    const search = (nodes: ZoteroCollectionNode[]): Set<string> | null => {
      for (const node of nodes) {
        if (node.id === activeCollectionId) {
          return walk([node]);
        }
        const fromChild = search(node.children);
        if (fromChild) return fromChild;
      }
      return null;
    };

    return search(zoteroCollections);
  }, [activeCollectionId, zoteroCollections]);

  const visibleZoteroItems = useMemo(() => {
    if (!collectionItemKeySet || activeCollectionId === null) return zoteroItems;
    return zoteroItems.filter((item) => collectionItemKeySet.has(item.item_key));
  }, [activeCollectionId, collectionItemKeySet, zoteroItems]);

  const escapeInlineHtml = (text: string) =>
    text
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/\"/g, "&quot;")
      .replace(/'/g, "&#039;");

  const toSafeHtml = (value: string) => {
    if (/<[a-z][\s\S]*>/i.test(value)) return value;
    return escapeInlineHtml(value);
  };

  const buildEndnoteEntries = (
    items: ZoteroLibraryItem[],
    formattedEntries: string[],
    citationIds?: string[],
  ): ZoteroEndnoteEntry[] => {
    return items.map((item, idx) => ({
      citationId: citationIds?.[idx] || `list-${Date.now()}-${idx + 1}`,
      number: idx + 1,
      entryHtml: toSafeHtml(formattedEntries[idx] || item.title || "未命名文献"),
      key: item.cite_key || item.item_key,
      itemUri: item.item_uri,
      pdfUri: item.pdf_uri,
    }));
  };

  const replaceEndnoteNode = (doc: JSONContent, entries: ZoteroEndnoteEntry[]) => {
    const stripEndnoteNodes = (nodes: JSONContent[] | undefined): JSONContent[] => {
      if (!nodes) return [];

      return nodes
        .filter((node) => node.type !== ZOTERO_ENDNOTE_SECTION_TYPE)
        .map((node) => ({
          ...node,
          content: node.content ? stripEndnoteNodes(node.content) : node.content,
        }));
    };

    doc.content = stripEndnoteNodes(doc.content);
    if (entries.length > 0) {
      doc.content = [
        ...(doc.content ?? []),
        {
          type: ZOTERO_ENDNOTE_SECTION_TYPE,
          attrs: {
            style: citationStyle,
            entriesJson: JSON.stringify(entries),
          },
        },
      ];
    }
  };

  const citationAttrsToLibraryItem = (attrs: Partial<ZoteroCitationAttrs>): ZoteroLibraryItem => ({
    id: attrs.citationId || attrs.itemKey || attrs.citeKey || "",
    item_key: attrs.itemKey || "",
    title: attrs.title || "未命名文献",
    author: attrs.author || "未知作者",
    year: attrs.year || "",
    cite_key: attrs.citeKey || attrs.itemKey || "",
    item_uri: attrs.itemUri || "",
    pdf_uri: attrs.pdfUri || "",
  });

  const syncCitationEndnotes = async () => {
    if (!editor) return;

    const doc = editor.getJSON();
    const now = Date.now();
    let generatedCount = 0;
    const dedupeOrder = new Map<string, number>();
    const uniqueCitations: Array<Partial<ZoteroCitationAttrs>> = [];

    const walk = (nodes: JSONContent[] | undefined) => {
      if (!nodes) return;

      for (const node of nodes) {
        if (node.type === ZOTERO_CITATION_TYPE) {
          const attrs = (node.attrs ?? {}) as Partial<ZoteroCitationAttrs>;
          const citationId = attrs.citationId || `cite-${now}-${generatedCount++}`;
          const dedupeKey = attrs.itemKey || attrs.citeKey || attrs.itemUri || citationId;

          let number = dedupeOrder.get(dedupeKey);
          if (!number) {
            number = dedupeOrder.size + 1;
            dedupeOrder.set(dedupeKey, number);
            uniqueCitations.push({ ...attrs, citationId, endnoteNumber: number });
          }

          node.attrs = {
            ...attrs,
            citationId,
            endnoteNumber: number,
          };
        }

        walk(node.content);
      }
    };

    walk(doc.content);

    const bibItems = uniqueCitations.map(citationAttrsToLibraryItem);
    const entries = await formatBibliographyEntries(bibItems, citationStyle);
    const endnoteEntries = buildEndnoteEntries(
      bibItems,
      entries,
      uniqueCitations.map((citation) => citation.citationId || ""),
    );

    replaceEndnoteNode(doc, endnoteEntries);

    editor.commands.setContent(doc, { emitUpdate: true });
  };

  const refreshZoteroLibrary = async () => {
    setZoteroLoading(true);
    setZoteroError("");

    try {
      const list = await zoteroApi.fetchLibrary(zoteroSource, zoteroEndpoint, sqliteDbPath);
      setZoteroItems(list);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setZoteroError(message || "读取 Zotero 文献库失败");
    } finally {
      setZoteroLoading(false);
    }
  };

  const refreshZoteroCollections = async () => {
    setZoteroCollectionsLoading(true);
    setZoteroCollectionsError("");

    try {
      const payload = await zoteroApi.fetchCollectionsTree(sqliteDbPath);
      setZoteroCollections(payload.collections);
      setZoteroCollectionsDbPath(payload.db_path);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setZoteroCollectionsError(message || "读取 Zotero 目录树失败");
    } finally {
      setZoteroCollectionsLoading(false);
    }
  };

  const handleInsertCitationFromPicker = (item: ZoteroLibraryItem) => {
    if (!editor) return;

    editor
      .chain()
      .focus()
      .insertContent({
        type: ZOTERO_CITATION_TYPE,
        attrs: {
          citationId: `cite-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
          endnoteNumber: 0,
          itemKey: item.item_key,
          itemUri: item.item_uri,
          pdfUri: item.pdf_uri,
          title: item.title,
          author: item.author,
          year: item.year,
          citeKey: item.cite_key || item.item_key,
          page: "",
          note: pendingCitationNote,
        },
      })
      .run();

    setPendingCitationNote("");
    setIsLibraryOpen(false);
    void syncCitationEndnotes();
  };

  const handleImportEndnotes = async (items: ZoteroLibraryItem[]) => {
    if (!editor) return;
    if (items.length === 0) return;

    const doc = editor.getJSON();
    const entries = await formatBibliographyEntries(items, citationStyle);
    const endnoteEntries = buildEndnoteEntries(items, entries);
    replaceEndnoteNode(doc, endnoteEntries);
    editor.commands.setContent(doc, { emitUpdate: true });
    setIsLibraryOpen(false);
  };

  if (!activeFile) {
    return (
      <div className="empty-state">
        <h2>欢迎回来</h2>
        <p>点击上方 + {isMobile ? "" : "或按 Ctrl+N"} 创建新笔记</p>
      </div>
    );
  }

  return (
    <div className="editor-shell" data-theme-mode={theme}>
      <div className="editor-header">{activeFile}</div>

      <div className="editor-inline-toolbar">
        <span className="toolbar-badge">块编辑模式</span>
        <button
          className="icon-btn"
          onClick={() => {
            setLibraryMode("endnotes");
            setIsLibraryOpen(true);
            if (zoteroItems.length === 0 && !zoteroLoading) {
              void refreshZoteroLibrary();
            }
            if (zoteroCollections.length === 0 && !zoteroCollectionsLoading) {
              void refreshZoteroCollections();
            }
          }}
          type="button"
        >
          文献库
        </button>
        <button className="icon-btn" onClick={() => void syncCitationEndnotes()} type="button">
          同步尾注
        </button>
        <input
          className="search-input"
          placeholder="搜索内容..."
          value={searchInput}
          onChange={(event) => {
            setSearchInput(event.target.value);
            setSearchMatches(0);
          }}
        />
        <button className="icon-btn" onClick={() => goToSearchResult("prev")}>上一个</button>
        <button className="icon-btn" onClick={() => goToSearchResult("next")}>下一个</button>
        <span className="search-count">{searchMatches > 0 ? `${searchMatches} 匹配` : ""}</span>
      </div>

      <div className={`editor-content-host ${isTypewriterMode ? "typewriter" : ""}`} ref={editorHostRef}>
        <button
          className="block-drag-handle"
          draggable
          ref={dragHandleRef}
          title="拖拽移动块"
          type="button"
        >
          ⋮⋮
        </button>
        <EditorContent editor={editor} />
      </div>

      {isLibraryOpen ? (
        <Suspense fallback={<div className="zotero-panel-overlay"><div className="zotero-panel">加载文献库...</div></div>}>
          <LazyZoteroLibraryPanel
            isOpen={isLibraryOpen}
            mode={libraryMode}
            source={zoteroSource}
            endpoint={zoteroEndpoint}
            sqliteDbPath={sqliteDbPath}
            collectionsDbPath={zoteroCollectionsDbPath}
            loading={zoteroLoading}
            collectionsLoading={zoteroCollectionsLoading}
            error={zoteroError}
            collectionsError={zoteroCollectionsError}
            items={visibleZoteroItems}
            collections={zoteroCollections}
            citationStyle={citationStyle}
            activeCollectionId={activeCollectionId}
            onClose={() => setIsLibraryOpen(false)}
            onSourceChange={(source) => {
              setZoteroSource(source);
              setActiveCollectionId(null);
            }}
            onEndpointChange={setZoteroEndpoint}
            onSqliteDbPathChange={setSqliteDbPath}
            onRefresh={() => void refreshZoteroLibrary()}
            onRefreshCollections={() => void refreshZoteroCollections()}
            onSelectCollection={setActiveCollectionId}
            onCitationStyleChange={setCitationStyle}
            onImportEndnotes={(items) => void handleImportEndnotes(items)}
            onInsertCitation={handleInsertCitationFromPicker}
          />
        </Suspense>
      ) : null}
    </div>
  );
};
