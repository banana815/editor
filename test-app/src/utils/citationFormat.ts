import type { ZoteroLibraryItem } from "../api/zotero";

export type CitationStyle = "apa" | "ieee" | "gb-t-7714";

const styleTemplateMap: Record<CitationStyle, string> = {
  apa: "apa",
  ieee: "ieee",
  "gb-t-7714": "china-national-standard-gb-t-7714-2015-numeric",
};

const toCslAuthor = (author: string) => {
  if (!author.trim()) return [{ literal: "未知作者" }];

  return author
    .split(";")
    .map((name) => name.trim())
    .filter(Boolean)
    .map((name) => {
      if (name.includes(",")) {
        const [family, given] = name.split(",");
        return { family: family.trim(), given: (given || "").trim() };
      }

      const parts = name.split(/\s+/).filter(Boolean);
      if (parts.length >= 2) {
        return {
          family: parts[0],
          given: parts.slice(1).join(" "),
        };
      }

      return { literal: name };
    });
};

const toCslItem = (item: ZoteroLibraryItem) => ({
  id: item.id || item.item_key,
  type: "article-journal",
  title: item.title || "未命名文献",
  author: toCslAuthor(item.author || ""),
  issued: {
    "date-parts": [[Number.parseInt(item.year, 10) || 0]],
  },
});

const fallbackFormat = (item: ZoteroLibraryItem, index: number, style: CitationStyle) => {
  const author = item.author || "未知作者";
  const year = item.year || "n.d.";
  const title = item.title || "未命名文献";

  if (style === "ieee") {
    return `[${index}] ${author}, \"${title},\" ${year}.`;
  }

  if (style === "gb-t-7714") {
    return `[${index}] ${author}. ${title}. ${year}.`;
  }

  return `${author} (${year}). ${title}.`;
};

const extractEntriesFromHtml = (html: string) => {
  const matches = Array.from(html.matchAll(/<div class="csl-entry">([\s\S]*?)<\/div>/g));
  return matches.map((match) => match[1].trim()).filter(Boolean);
};

let citationJsReady: Promise<{ Cite: any }> | null = null;

const loadCitationJs = async () => {
  if (!citationJsReady) {
    citationJsReady = Promise.all([
      import("@citation-js/core"),
      import("@citation-js/plugin-csl"),
    ]).then(([core]) => ({
      Cite: (core as { Cite: any }).Cite,
    }));
  }

  return citationJsReady;
};

export const formatBibliographyEntries = async (
  items: ZoteroLibraryItem[],
  style: CitationStyle,
) => {
  if (items.length === 0) return [];

  try {
    const { Cite } = await loadCitationJs();
    const cite = new Cite(items.map(toCslItem));
    const template = styleTemplateMap[style];
    const html = cite.format("bibliography", {
      format: "html",
      template,
      lang: "zh-CN",
    });

    const entries = extractEntriesFromHtml(html);
    if (entries.length > 0) return entries;
  } catch {
    // Fall back to deterministic local formatting.
  }

  return items.map((item, index) => fallbackFormat(item, index + 1, style));
};
