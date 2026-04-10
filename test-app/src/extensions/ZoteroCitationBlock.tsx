import React from "react";
import { Node, mergeAttributes } from "@tiptap/core";
import { NodeViewWrapper, ReactNodeViewRenderer, type NodeViewProps } from "@tiptap/react";
import { openUrl } from "@tauri-apps/plugin-opener";

export interface ZoteroCitationAttrs {
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

export const ZOTERO_CITATION_TYPE = "zoteroCitationBlock";

const defaultCitationAttrs = (): ZoteroCitationAttrs => ({
  citationId: "",
  endnoteNumber: 0,
  itemKey: "",
  itemUri: "",
  pdfUri: "",
  title: "",
  author: "",
  year: "",
  citeKey: "",
  page: "",
  note: "",
});

const normalizeRawCitationInput = (raw: string) => {
  const trimmed = raw.trim();

  if (!trimmed) {
    return { itemKey: "", itemUri: "", pdfUri: "" };
  }

  if (/^[a-z]+:\/\//i.test(trimmed)) {
    const itemKeyMatch = trimmed.match(/(?:items|library\/items)\/([^/?#]+)/i);
    const itemKey = itemKeyMatch?.[1] ?? "";
    return {
      itemKey,
      itemUri: trimmed,
      pdfUri: itemKey ? `zotero://open-pdf/library/items/${itemKey}` : "",
    };
  }

  return {
    itemKey: trimmed,
    itemUri: `zotero://select/items/${trimmed}`,
    pdfUri: `zotero://open-pdf/library/items/${trimmed}`,
  };
};

const promptField = (label: string, initialValue = "") => {
  const result = window.prompt(label, initialValue);
  if (result === null) return null;
  return result.trim();
};

export const promptCitationAttrs = (initialValues: Partial<ZoteroCitationAttrs> = {}) => {
  const rawSource = promptField(
    "Zotero 条目 key 或 URI",
    initialValues.itemUri || initialValues.itemKey || "",
  );
  if (rawSource === null) return null;

  const normalizedSource = normalizeRawCitationInput(rawSource);
  const title = promptField("文献标题", initialValues.title || "") ?? "";
  const author = promptField("作者", initialValues.author || "") ?? "";
  const year = promptField("年份", initialValues.year || "") ?? "";
  const citeKey = promptField("Citekey", initialValues.citeKey || normalizedSource.itemKey || "") ?? "";
  const page = promptField("页码 / 位置", initialValues.page || "") ?? "";
  const note = promptField("摘录 / 备注", initialValues.note || "") ?? "";

  const customPdfUri = promptField("PDF URI（留空自动生成）", initialValues.pdfUri || normalizedSource.pdfUri || "");
  if (customPdfUri === null) return null;

  return {
    ...defaultCitationAttrs(),
    ...initialValues,
    ...normalizedSource,
    title,
    author,
    year,
    citeKey: citeKey || normalizedSource.itemKey,
    page,
    note,
    pdfUri: customPdfUri || normalizedSource.pdfUri,
  } satisfies ZoteroCitationAttrs;
};

export const openCitationUri = async (uri: string) => {
  if (!uri) return;

  try {
    await openUrl(uri);
  } catch {
    window.open(uri, "_blank", "noopener,noreferrer");
  }
};

const CitationNodeView: React.FC<NodeViewProps> = ({ node, updateAttributes, selected }) => {
  const attrs = node.attrs as ZoteroCitationAttrs;

  const handleEdit = () => {
    const nextAttrs = promptCitationAttrs(attrs);
    if (!nextAttrs) return;

    updateAttributes(nextAttrs);
  };

  const handleCopy = async () => {
    const value = attrs.citeKey || attrs.itemKey || attrs.title;
    if (!value) return;

    try {
      await navigator.clipboard.writeText(value);
    } catch {
      window.prompt("复制 Citekey", value);
    }
  };

  const jumpToEndnote = () => {
    if (!attrs.citationId) return;
    const el = document.getElementById(`endnote-entry-${attrs.citationId}`);
    if (!el) return;
    el.scrollIntoView({ behavior: "smooth", block: "center" });
  };

  return (
    <NodeViewWrapper
      className={`citation-block ${selected ? "is-selected" : ""}`}
      data-drag-handle
      id={attrs.citationId ? `citation-block-${attrs.citationId}` : undefined}
    >
      <div className="citation-block__accent" />
      <div className="citation-block__body">
        <div className="citation-block__topline">
          <button
            className="citation-block__title-button"
            onClick={() => openCitationUri(attrs.itemUri || attrs.pdfUri)}
            type="button"
          >
            {attrs.endnoteNumber > 0 ? `[${attrs.endnoteNumber}] ` : ""}
            {attrs.citeKey || attrs.itemKey || "Zotero 引用块"}
          </button>
          <span className="citation-block__badge">{attrs.page ? `p. ${attrs.page}` : "引用"}</span>
        </div>

        <div className="citation-block__title">{attrs.title || "未命名文献"}</div>
        <div className="citation-block__meta">
          <span>{attrs.author || "作者未填"}</span>
          <span>{attrs.year || "年份未填"}</span>
          <span>{attrs.itemKey || "未绑定 Zotero 条目"}</span>
        </div>

        <div className="citation-block__note">{attrs.note || "没有附带摘录。"}</div>

        <div className="citation-block__actions">
          <button className="citation-action" onClick={jumpToEndnote} type="button">
            跳到尾注
          </button>
          <button className="citation-action" onClick={() => openCitationUri(attrs.itemUri)} type="button">
            打开 Zotero
          </button>
          <button className="citation-action" onClick={() => openCitationUri(attrs.pdfUri)} type="button">
            打开 PDF
          </button>
          <button className="citation-action" onClick={handleEdit} type="button">
            编辑
          </button>
          <button className="citation-action" onClick={handleCopy} type="button">
            复制 Citekey
          </button>
        </div>
      </div>
    </NodeViewWrapper>
  );
};

export const ZoteroCitationBlock = Node.create({
  name: ZOTERO_CITATION_TYPE,

  group: "block",
  atom: true,
  draggable: true,
  selectable: true,
  isolating: true,

  addAttributes() {
    return {
      itemKey: { default: "" },
      citationId: { default: "" },
      endnoteNumber: { default: 0 },
      itemUri: { default: "" },
      pdfUri: { default: "" },
      title: { default: "" },
      author: { default: "" },
      year: { default: "" },
      citeKey: { default: "" },
      page: { default: "" },
      note: { default: "" },
    };
  },

  parseHTML() {
    return [
      {
        tag: `div[data-type="${ZOTERO_CITATION_TYPE}"]`,
      },
    ];
  },

  renderHTML({ HTMLAttributes }) {
    return [
      "div",
      mergeAttributes(HTMLAttributes, {
        "data-type": ZOTERO_CITATION_TYPE,
        "data-citation-id": HTMLAttributes.citationId || "",
        "data-endnote-number": HTMLAttributes.endnoteNumber || 0,
        class: "citation-block citation-block--serialized",
      }),
    ];
  },

  addNodeView() {
    return ReactNodeViewRenderer(CitationNodeView);
  },
});
