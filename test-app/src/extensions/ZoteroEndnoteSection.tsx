import React from "react";
import { Node, mergeAttributes } from "@tiptap/core";
import { NodeViewWrapper, ReactNodeViewRenderer, type NodeViewProps } from "@tiptap/react";
import { openCitationUri } from "./ZoteroCitationBlock";

export const ZOTERO_ENDNOTE_SECTION_TYPE = "zoteroEndnoteSection";

export interface ZoteroEndnoteEntry {
  citationId: string;
  number: number;
  entryHtml: string;
  key: string;
  itemUri: string;
  pdfUri: string;
}

interface ZoteroEndnoteAttrs {
  style: string;
  entriesJson: string;
}

const parseEntries = (value: string): ZoteroEndnoteEntry[] => {
  if (!value.trim()) return [];

  try {
    const parsed = JSON.parse(value) as ZoteroEndnoteEntry[];
    if (Array.isArray(parsed)) {
      return parsed.filter((entry) => typeof entry?.number === "number");
    }
  } catch {
    // Ignore parse failure and return empty state.
  }

  return [];
};

const EndnoteNodeView: React.FC<NodeViewProps> = ({ node }) => {
  const attrs = node.attrs as ZoteroEndnoteAttrs;
  const entries = parseEntries(attrs.entriesJson || "");

  return (
    <NodeViewWrapper className="endnote-section">
      <div className="endnote-section__title">
        参考文献（自动同步 {attrs.style ? attrs.style.toUpperCase() : ""}）
      </div>
      {entries.length === 0 ? (
        <div className="endnote-empty">暂无引用块，点击“同步尾注”后将自动生成。</div>
      ) : (
        <ol className="endnote-list">
          {entries.map((entry) => (
            <li key={entry.citationId || `entry-${entry.number}`} id={`endnote-entry-${entry.citationId}`}>
              <span className="endnote-number">[{entry.number}] </span>
              <span dangerouslySetInnerHTML={{ __html: entry.entryHtml }} />
              {entry.key ? <span className="endnote-key"> ({entry.key})</span> : null}
              <span className="endnote-links">
                {entry.itemUri ? (
                  <button type="button" className="endnote-link-btn" onClick={() => openCitationUri(entry.itemUri)}>
                    Zotero
                  </button>
                ) : null}
                {entry.pdfUri ? (
                  <button type="button" className="endnote-link-btn" onClick={() => openCitationUri(entry.pdfUri)}>
                    PDF
                  </button>
                ) : null}
                {entry.citationId ? (
                  <button
                    type="button"
                    className="endnote-link-btn"
                    onClick={() => {
                      const target = document.getElementById(`citation-block-${entry.citationId}`);
                      target?.scrollIntoView({ behavior: "smooth", block: "center" });
                    }}
                  >
                    跳回引用
                  </button>
                ) : null}
              </span>
            </li>
          ))}
        </ol>
      )}
    </NodeViewWrapper>
  );
};

export const ZoteroEndnoteSection = Node.create({
  name: ZOTERO_ENDNOTE_SECTION_TYPE,

  group: "block",
  atom: true,
  draggable: false,
  selectable: true,
  isolating: true,

  addAttributes() {
    return {
      style: { default: "apa" },
      entriesJson: { default: "[]" },
    };
  },

  parseHTML() {
    return [{ tag: `div[data-type="${ZOTERO_ENDNOTE_SECTION_TYPE}"]` }];
  },

  renderHTML({ HTMLAttributes }) {
    return [
      "div",
      mergeAttributes(HTMLAttributes, {
        "data-type": ZOTERO_ENDNOTE_SECTION_TYPE,
        class: "endnote-section endnote-section--serialized",
      }),
    ];
  },

  addNodeView() {
    return ReactNodeViewRenderer(EndnoteNodeView);
  },
});
