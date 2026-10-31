#!/usr/bin/env python3
"""The C API reference, generated from include/zerog.h.

The header is the reference: its comments document every type and function.
This script turns it into Markdown pages for the documentation site
(docs/reference/), one per section of the header (the banners
"/* ---...--- */ /* Title */"), with a heading, the declaration and its comment
for each type, function and macro, its subsections ("/* ---- Name ---- */") as
headings with their text, and an index of every function.

    python tools/apidoc.py            # write docs/reference/
    python tools/apidoc.py --check    # fail if docs/reference/ is out of date
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include" / "zerog.h"
OUT = ROOT / "docs" / "reference"

BANNER = re.compile(r"^/\* -{20,} \*/$")
TITLE = re.compile(r"^/\* (.+?)\s*\*/$")
SUBSECTION = re.compile(r"^/\* ---- (.+?) ----(?: \*/)?\s*$")


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def comment_text(lines):
    """A C comment's text, without the delimiters and the common indentation."""
    text = "\n".join(lines).strip()
    text = re.sub(r"^/\*+", "", text)
    text = re.sub(r"\*+/$", "", text)
    rows = [re.sub(r"^ ?\* ?", "", row) if row.lstrip().startswith("*") else row for row in text.split("\n")]
    indent = min((len(r) - len(r.lstrip()) for r in rows[1:] if r.strip()), default=0)
    rows = [rows[0].strip()] + [r[indent:] for r in rows[1:]]
    return "\n".join(rows).strip()


def declaration_name(code):
    if match := re.search(r"^#define\s+(\w+)", code, re.M):
        return match.group(1)
    if match := re.search(r"\(\s*\*\s*(zg_\w+)\s*\)\s*\(", code):  # function pointer typedef
        return match.group(1)
    if match := re.search(r"\bZG_API\b[^;(]*?\b(zg_\w+)\s*\(", code):
        return match.group(1)
    if match := re.search(r"}\s*(zg_\w+)\s*;\s*$", code):  # typedef struct/enum { ... } name;
        return match.group(1)
    if match := re.search(r"typedef\s+(?:struct|enum)\s+\w+\s+(zg_\w+)\s*;", code):  # opaque handle
        return match.group(1)
    if match := re.search(r"typedef\s+.*?\b(zg_\w+)\s*;", code):
        return match.group(1)
    return None


def parse(lines):
    """Sections of the header: (title, intro, items), items being ("sub", title,
    text) or ("decl", name, code, doc)."""
    sections = []
    current = None
    pending = []  # the comment before a declaration
    i = 0
    while i < len(lines):
        line = lines[i]
        if BANNER.match(line) and i + 1 < len(lines) and TITLE.match(lines[i + 1]):
            current = {"title": TITLE.match(lines[i + 1]).group(1), "items": []}
            sections.append(current)
            pending = []
            i += 2
            continue
        if current is None:
            i += 1
            continue
        if match := SUBSECTION.match(line):
            # "/* ---- Name ---- */" or a subsection whose text follows in the comment.
            text_lines = []
            if not line.rstrip().endswith("*/"):
                i += 1
                while i < len(lines) and "*/" not in lines[i]:
                    text_lines.append(lines[i])
                    i += 1
                text_lines.append(lines[i].replace("*/", "") if i < len(lines) else "")
            current["items"].append(("sub", match.group(1), "\n".join(l.strip() for l in text_lines).strip()))
            pending = []
            i += 1
            continue
        stripped = line.strip()
        if stripped.startswith("/*"):
            block = [line]
            while "*/" not in block[-1] and i + 1 < len(lines):
                i += 1
                block.append(lines[i])
            pending = block
            i += 1
            continue
        if stripped.startswith(("typedef", "ZG_API", "#define ZG_")):
            # A macro is one line; anything else ends at a ";" with its braces closed.
            code = [line]
            depth = line.count("{") - line.count("}")
            def ends(row):  # ";" last, before any trailing comment
                return re.sub(r"/\*.*?\*/\s*$", "", row).rstrip().endswith(";")
            if not stripped.startswith("#define"):
                while not (depth <= 0 and ends(code[-1])) and i + 1 < len(lines):
                    i += 1
                    code.append(lines[i])
                    depth += lines[i].count("{") - lines[i].count("}")
            text = "\n".join(code)
            name = declaration_name(text)
            if name:
                current["items"].append(("decl", name, text, comment_text(pending) if pending else ""))
            pending = []
            i += 1
            continue
        if stripped:
            pending = []
        i += 1
    return sections


def preamble(lines):
    """The header's opening comment: the conventions every function follows."""
    block = []
    for line in lines:
        block.append(line)
        if "*/" in line:
            break
    return comment_text(block)


def render(sections, conventions):
    pages = {}
    index_rows = []
    for section in sections:
        title = section["title"]
        name = slug(title.split(":")[0].split("(")[0])
        out = [f"# {title}", "", "<!-- Generated by tools/apidoc.py from include/zerog.h: edit the header, not this page. -->", ""]
        for item in section["items"]:
            if item[0] == "sub":
                out += [f"## {item[1]}", ""]
                if item[2]:
                    out += [item[2], ""]
                continue
            _, decl, code, doc = item
            out += [f"### `{decl}`", "", "```c", code.rstrip(), "```", ""]
            if doc:
                out += [doc, ""]
            if "(" in code and "ZG_API" in code:
                index_rows.append((decl, f"{name}.md#{decl.lower()}"))
        pages[f"{name}.md"] = "\n".join(out).rstrip() + "\n"

    index = ["# C API reference", "",
             "<!-- Generated by tools/apidoc.py from include/zerog.h: edit the header, not this page. -->", "",
             "Everything public is in one header, [`include/zerog.h`](https://github.com/dzonerzy/ZeroG/blob/main/include/zerog.h). "
             "These pages are generated from it.", "", "## Conventions", "", "```text", conventions, "```", "", "## Sections", ""]
    for section in sections:
        name = slug(section["title"].split(":")[0].split("(")[0])
        index.append(f"- [{section['title']}]({name}.md)")
    index += ["", "## Functions", ""]
    for decl, link in sorted(index_rows):
        index.append(f"- [`{decl}`]({link})")
    pages["index.md"] = "\n".join(index) + "\n"
    return pages


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if docs/reference/ differs from what the header gives")
    args = parser.parse_args()
    lines = HEADER.read_text(encoding="utf-8").splitlines()
    pages = render(parse(lines), preamble(lines))
    if args.check:
        stale = [name for name, text in pages.items() if not (OUT / name).exists() or (OUT / name).read_text(encoding="utf-8") != text]
        extra = [p.name for p in OUT.glob("*.md") if p.name not in pages] if OUT.exists() else []
        if stale or extra:
            sys.exit(f"docs/reference/ is out of date ({', '.join(stale + extra)}): run python tools/apidoc.py")
        print(f"docs/reference/ is up to date ({len(pages)} pages)")
        return
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.md"):
        if old.name not in pages:
            old.unlink()
    for name, text in pages.items():
        (OUT / name).write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {len(pages)} pages to {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
