#!/usr/bin/env python3
"""Build a filtered ICU data library (icudt) for one profile of deps/icu-data.json.

The ICU sources ship only the prebuilt data package (data/in/icudt<N>l.dat), so
filtering works on that package with ICU's own tools: icupkg to unpack and
repack, genrb to regenerate each tree's res_index, genccode to turn the result
into an object file. Output: <out>/icudt-<profile>.lib (Windows) or
libicudt-<profile>.a, to link instead of ICU's stock data library.

What is removed:
  - locale bundles of languages not in the profile, in every locale tree
    (root, lang, region, curr, unit, zone, coll, rbnf, brkitr), with res_index
    rewritten so Intl does not advertise them;
  - every converter table except those WebCore routes to ICU, read from
    Source/WebCore/PAL/pal/text/TextCodecICU.cpp.
Everything else (normalization, break rules and dictionaries, time zones,
properties) is kept.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCALE_TREES = ["", "lang", "region", "curr", "unit", "zone", "coll", "rbnf", "brkitr"]
# Deprecated language codes still shipped as alias bundles, mapped to the current code.
LANGUAGE_ALIASES = {"in": "id", "iw": "he", "ji": "yi", "mo": "ro", "no": "nb", "sh": "sr",
                    "tl": "fil", "ars": "ar"}


def run(tool, *args, cwd=None):
    cmd = [str(tool), *map(str, args)]
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        sys.exit(f"{' '.join(cmd)} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def tool(tools, name):
    for candidate in (name, name + ".exe"):
        if (tools / candidate).exists():
            return tools / candidate
    sys.exit(f"ICU tool {name} not found in {tools}")


def extract_package(tarball, work):
    with tarfile.open(tarball) as tar:
        member = next(m for m in tar.getmembers() if re.search(r"source/data/in/icudt\d+l\.dat$", m.name))
        member.name = Path(member.name).name
        tar.extract(member, work, filter="data")
    return work / member.name


def webcore_encodings():
    """Every converter WebCore asks ICU for at runtime:
    - TextCodecICU's encodings (DECLARE_ENCODING_NAME and _NO_ALIASES);
    - converters opened by name anywhere in WebCore and WTF, e.g. EncodingTables.cpp,
      which builds the CJK decoders' tables in release builds by running EUC-JP,
      Big-5, windows-949 and gb18030 over every code point and then RELEASE_ASSERTs
      the count: without that data the first CJK page crashes."""
    webkit = ROOT / "third_party/webkit/Source"
    codec = webkit / "WebCore/PAL/pal/text/TextCodecICU.cpp"
    names = set(re.findall(r'DECLARE_ENCODING_NAME(?:_NO_ALIASES)?\("([^"]+)"_s', codec.read_text()))
    if len(names) < 15:
        sys.exit(f"only {len(names)} encodings found in {codec}: has TextCodecICU.cpp changed?")
    opened = set()
    for root in (webkit / "WebCore", webkit / "WTF/wtf"):
        for path in root.rglob("*.cpp"):
            text = path.read_text(errors="replace")
            if "ucnv_open(" in text:
                opened |= set(re.findall(r'ucnv_open\("([^"]+)"', text))
    if not opened:
        sys.exit("no ucnv_open(\"...\") calls found in WebCore: has EncodingTables.cpp changed?")
    return {n.lower() for n in names | opened}


def converter_key(name):
    """ICU matches converter names ignoring case and punctuation (ucnv_compareNames):
    "Big-5" opens the converter aliased "Big5"."""
    return re.sub(r"[^a-z0-9]", "", name.lower())


def converter_files(uconv, encodings):
    """Converter names (= .cnv items) whose alias list contains a WebCore encoding."""
    wanted = {converter_key(e): e for e in encodings}
    keep, matched, current = set(), set(), None
    for line in run(uconv, "-l", "--canon").splitlines():
        if not line.strip():
            continue
        name = line.split("{")[0].strip()
        if not line[0].isspace():
            current = name
        if converter_key(name) in wanted:
            keep.add(current)
            matched.add(wanted[converter_key(name)])
    config = json.loads((ROOT / "deps/icu-data.json").read_text())
    allowed = {k for k in config.get("converters_missing_upstream", {}) if not k.startswith("_")}
    unexpected = encodings - matched - allowed
    if unexpected:
        sys.exit(f"WebCore encodings with no ICU converter: {sorted(unexpected)}")
    return keep


def res_index_names(derb, tree_dir, work):
    """InstalledLocales and AliasLocales listed in a tree's res_index."""
    out = work / "derb" / (tree_dir.name or "root")
    out.mkdir(parents=True, exist_ok=True)
    # derb takes the bundle's directory from the path and cannot parse a Windows drive
    # letter (it fails and still exits 0): pass paths relative to the work directory.
    run(derb, "-d", os.path.relpath(out, work), os.path.relpath(tree_dir / "res_index.res", work), cwd=work)
    if not (out / "res_index.txt").exists():
        sys.exit(f"derb produced nothing for {tree_dir}")
    text = (out / "res_index.txt").read_text(encoding="utf-8")
    tables = {}
    for table in ("InstalledLocales", "AliasLocales"):
        block = re.search(table + r"\{(.*?)\n    \}", text, re.S)
        tables[table] = re.findall(r"^\s+(\w+) \{ \"\" \}", block.group(1), re.M) if block else []
    return tables


def keeps_language(locale, languages):
    if languages == "*" or locale == "root":
        return True
    lang = locale.split("_")[0]
    return lang in languages or LANGUAGE_ALIASES.get(lang) in languages


def write_res_index(genrb, tree_dir, work, tables):
    lines = ["res_index:table(nofallback) {"]
    for table, names in tables.items():
        if names:
            lines.append(f"    {table} {{")
            lines += [f'        {n} {{ "" }}' for n in names]
            lines.append("    }")
    lines.append("}")
    src = work / "res_index_src" / (tree_dir.name or "root") / "res_index.txt"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_text("\n".join(lines) + "\n", encoding="utf-8")
    run(genrb, "-q", "-d", tree_dir, src)


def build(args):
    profiles = json.loads((ROOT / "deps/icu-data.json").read_text())["profiles"]
    languages = profiles[args.profile]["languages"]
    tools = Path(args.tools)
    icupkg, genrb, derb, uconv, genccode = (tool(tools, n) for n in ("icupkg", "genrb", "derb", "uconv", "genccode"))

    work = Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    package = extract_package(args.icu_sources, work)
    name = package.stem                       # icudt79l
    entry = name[:-1]                         # icudt79 -> symbol icudt79_dat
    items = work / "items"
    items.mkdir()
    run(icupkg, "-d", items, "-x", "*", package)
    all_items = [l.strip() for l in run(icupkg, "-l", package).splitlines() if l.strip()]

    removed = set()
    for tree in LOCALE_TREES:
        tree_dir = items / tree
        if not (tree_dir / "res_index.res").exists():
            continue
        tables = res_index_names(derb, tree_dir, work)
        kept = {t: [n for n in names if keeps_language(n, languages)] for t, names in tables.items()}
        write_res_index(genrb, tree_dir, work, kept)
        # Filter every bundle of a known language, including ones res_index does not
        # list (e.g. th_TH_TRADITIONAL); non-locale items (pool, zoneinfo64, ...) stay.
        known = {n.split("_")[0] for names in tables.values() for n in names}
        prefix = f"{tree}/" if tree else ""
        for item in all_items:
            if not item.startswith(prefix) or "/" in item[len(prefix):] or not item.endswith(".res"):
                continue
            locale = item[len(prefix):-4]
            if locale.split("_")[0] in known and not keeps_language(locale, languages):
                removed.add(item)

    keep_cnv = converter_files(uconv, webcore_encodings())
    removed |= {i for i in all_items if i.endswith(".cnv") and i[:-4] not in keep_cnv}

    filtered = work / "out" / package.name
    filtered.parent.mkdir()
    restored = []
    while True:
        # icupkg checks dependencies; bundles inherited across languages (rbnf/zh_Hant_HK
        # from rbnf/yue) are restored without being advertised in res_index.
        kept_items = [i for i in all_items if i not in removed]
        (work / "keep.txt").write_text("".join(i + "\n" for i in kept_items))
        filtered.unlink(missing_ok=True)
        result = subprocess.run([str(icupkg), "-tl", "-s", str(items), "-a", str(work / "keep.txt"),
                                 "new", str(filtered)], text=True, capture_output=True)
        deps = set(re.findall(r"depends on missing item (\S+)", result.stdout + result.stderr)) & removed
        if not result.returncode and not deps:
            break
        if not deps:
            sys.exit(f"icupkg failed:\n{result.stdout}{result.stderr}")
        removed -= deps
        restored += sorted(deps)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    obj_dir = work / "obj"
    obj_dir.mkdir()
    if sys.platform == "win32":
        # genccode copies the COFF machine type from a reference object (else "unknown").
        ref_src = work / "match_arch.c"
        ref_src.write_text("int zerog_icu_match_arch;\n")
        ref_obj = work / "match_arch.obj"
        run(shutil.which("clang-cl") or "cl", "/nologo", "/c", ref_src, f"/Fo{ref_obj}")
        run(genccode, "-o", "-m", ref_obj, "-e", entry, "-d", obj_dir, filtered)
        objects = list(obj_dir.glob("*.obj"))
        library = out / f"icudt-{args.profile}.lib"
        library.unlink(missing_ok=True)
        run(shutil.which("llvm-lib") or "lib", "/nologo", f"/out:{library}", *objects)
    else:
        run(genccode, "-a", "gcc", "-e", entry, "-d", obj_dir, filtered)
        asm = next(obj_dir.glob("*.S"))
        obj = asm.with_suffix(".o")
        run(os.environ.get("CC") or shutil.which("clang") or "cc", "-c", asm, "-o", obj)
        library = out / f"libicudt-{args.profile}.a"
        library.unlink(missing_ok=True)
        run(os.environ.get("AR") or shutil.which("llvm-ar") or "ar", "rcs", library, obj)

    cnv = sorted(keep_cnv)
    report = {
        "profile": args.profile,
        "package_bytes": {"original": package.stat().st_size, "filtered": filtered.stat().st_size},
        "items": {"original": len(all_items), "kept": len(kept_items)},
        "converters_kept": cnv,
        "restored_dependencies": restored,
        "library": library.name,
    }
    (out / f"icudt-{args.profile}.json").write_text(json.dumps(report, indent=2))
    print(f"{library.name}: {package.stat().st_size / 1e6:.1f} MB -> {filtered.stat().st_size / 1e6:.1f} MB, "
          f"{len(kept_items)}/{len(all_items)} items, {len(cnv)} converters")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--tools", required=True, help="directory with icupkg, genrb, derb, uconv, genccode")
    parser.add_argument("--icu-sources", required=True, help="ICU source tarball (icu4c-*-sources.tgz)")
    parser.add_argument("--work", required=True)
    parser.add_argument("--out", required=True)
    build(parser.parse_args())


if __name__ == "__main__":
    main()
