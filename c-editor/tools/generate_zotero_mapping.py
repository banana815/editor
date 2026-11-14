"""Generate C lookup data from Zotero's published data schema (no runtime Python).

Usage: python3 tools/generate_zotero_mapping.py schema.json output.h COMMIT
Obtain schema.json from https://github.com/zotero/zotero-schema at COMMIT.
Only type/field/creator relationships are emitted, not implementation code.
"""
import json
import pathlib
import re
import sys


def generate(schema, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("A full source commit is required")
    quote = lambda value: json.dumps(value, ensure_ascii=True)
    csl = schema["csl"]
    types = {source: target for target, sources in csl["types"].items() for source in sources}
    fields = [(source, target, 0) for target, sources in csl["fields"]["text"].items() for source in sources]
    fields += [(source, target, 1) for target, source in csl["fields"]["date"].items()]
    lines = ["/* Generated lookup relationships from Zotero schema version " + str(schema["version"]),
             " * https://github.com/zotero/zotero-schema/blob/" + commit + "/schema.json",
             " * Regenerate with tools/generate_zotero_mapping.py. */",
             "typedef struct { const char *zotero, *csl, *primary; } ZoteroTypeMap;",
             "static const ZoteroTypeMap zotero_type_map[] = {"]
    for item in schema["itemTypes"]:
        name = item["itemType"]
        primary = next((c["creatorType"] for c in item.get("creatorTypes", []) if c.get("primary")), item.get("primary", ""))
        lines.append("    {" + ", ".join(map(quote, (name, types.get(name, "document"), primary))) + "},")
    lines += ["};", "typedef struct { const char *type, *zotero, *csl; int date; } ZoteroFieldMap;",
              "static const ZoteroFieldMap zotero_field_map[] = {"]
    for source, target, date in fields:
        lines.append("    {\"\", " + quote(source) + ", " + quote(target) + ", " + str(date) + "},")
    for item in schema["itemTypes"]:
        for field in item["fields"]:
            for source, target, date in fields:
                if field.get("baseField") == source:
                    lines.append("    {" + ", ".join(map(quote, (item["itemType"], field["field"], target))) + ", " + str(date) + "},")
    lines += ["};", "typedef struct { const char *zotero, *csl; } ZoteroCreatorMap;",
              "static const ZoteroCreatorMap zotero_creator_map[] = {"]
    for source, target in csl["names"].items():
        lines.append("    {" + quote(source) + ", " + quote(target) + "},")
    lines += ["};", ""]
    return "\n".join(lines)


if __name__ == "__main__":
    source, target, commit = sys.argv[1:]
    schema = json.loads(pathlib.Path(source).read_text(encoding="utf-8"))
    pathlib.Path(target).write_text(generate(schema, commit), encoding="utf-8")
