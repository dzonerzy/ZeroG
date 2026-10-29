#!/usr/bin/env python3
"""Build the engine bundle: one archive with every engine object and its static
dependencies, where only the public API (port/api/exports.txt) stays public
(ZeroG.md, "Simboli nel link statico").

Linux (ELF): everything is partially linked (`ld -r`) into one relocatable
object, then `objcopy --keep-global-symbols` makes every other symbol local.
Nothing but the API can collide with the host, C++ and C alike.

Windows (COFF): there is no partial link. Every symbol the bundle defines, C and
mangled C++ alike, is renamed to zg_<name> in all objects (definitions and
references, with llvm-objcopy --redefine-syms; names inside linker directives
are rewritten too). A host with its own copy of a dependency, even the same ICU
version, cannot collide with the engine's. Symbols the bundle only references
(CRT, STL, Windows) keep their names; debug info keeps its own names.

Usage (normally through tools/build.py bundle):
    bundle.py --objects <rsp> --archives <lib>... --exports <file> --out <archive> --work <dir>
"""

import argparse
import fnmatch
import os
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"
# Names a renamed bundle must not touch: compiler and CRT conventions.
WINDOWS_KEEP_PREFIXES = ("__", ".", "@", "$")
WINDOWS_KEEP = {"main", "wmain", "WinMain", "wWinMain", "DllMain", "_fltused", "_tls_used"}


def run(cmd, **kw):
    cmd = [str(c) for c in cmd]
    result = subprocess.run(cmd, text=True, capture_output=True, **kw)
    if result.returncode:
        sys.exit(f"{' '.join(cmd[:3])} ... failed:\n{result.stdout[-4000:]}{result.stderr[-4000:]}")
    return result.stdout


def tool(*names):
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    sys.exit(f"none of {names} found on PATH")


def export_patterns(path):
    lines = (l.split("#", 1)[0].strip() for l in Path(path).read_text().splitlines())
    return [l for l in lines if l]


def is_public(name, patterns):
    return any(fnmatch.fnmatchcase(name, p) for p in patterns)


def archive_members(path):
    """(name, data) of every member of a GNU/COFF `ar` archive, in order. Names may
    repeat inside one archive (two foo.cpp in different directories), which `ar x`
    would extract over each other; on Windows so would names differing in case."""
    data = Path(path).read_bytes()
    if not data.startswith(b"!<arch>\n"):
        sys.exit(f"{path}: not an ar archive (thin archives are not supported)")
    offset, long_names = 8, b""
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        name = header[:16].decode("latin-1").rstrip()
        size = int(header[48:58].decode("latin-1").strip())
        body = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
        if name in ("/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED") or name.startswith("/<"):
            continue  # symbol tables, ECSYMBOLS
        if name == "//":
            long_names = body
            continue
        if name.startswith("/") and name[1:].isdigit():
            # GNU ends long names with "/\n", MSVC's lib.exe and llvm-lib with NUL.
            start = int(name[1:])
            ends = [e for e in (long_names.find(b"\n", start), long_names.find(b"\0", start)) if e >= 0]
            name = long_names[start:min(ends) if ends else None].decode("latin-1").rstrip("/")
        elif name.startswith("#1/"):  # BSD: the name precedes the data
            length = int(name[3:])
            name, body = body[:length].rstrip(b"\0").decode("latin-1"), body[length:]
        yield name.rstrip("/"), body


def extract_archives(archives, work, ar):
    """Members of each archive, each in its own file: index-prefixed, so repeated
    names inside or across archives do not overwrite each other."""
    objects = []
    for index, archive in enumerate(archives):
        dest = work / "members" / f"{index}-{Path(archive).stem}"
        dest.mkdir(parents=True)
        for member, (name, body) in enumerate(archive_members(archive)):
            base = Path(name.replace("\\", "/")).name
            if Path(base).suffix not in (".o", ".obj", ".ao"):
                continue
            target = dest / f"{member:05d}-{base}"
            target.write_bytes(body)
            objects.append(target)
    return objects


def defined_globals(nm, objects, work):
    rsp = work / "nm.rsp"
    rsp.write_text("".join(f'"{Path(o).as_posix()}"\n' for o in objects))
    out = run([nm, "--defined-only", "--extern-only", "--format=just-symbols", "--no-sort", f"@{rsp}"])
    return {line.strip() for line in out.splitlines()
            if line.strip() and not line.endswith(":") and not line.startswith("/")}


def bundle_elf(objects, patterns, out, work):
    ld = tool("ld")
    objcopy = tool("objcopy")
    nm = tool("nm")
    rsp = work / "objects.rsp"
    rsp.write_text("".join(f'"{Path(o).as_posix()}"\n' for o in objects))
    combined = work / "zerog-engine.o"
    # COMDAT groups are deduplicated by signature name even when the signature
    # symbol is local: a host with the same inline code (its own ICU) would share
    # the engine's copies, inline static data included. Duplicates inside the
    # bundle are merged by this link; afterwards no group is left.
    run([ld, "-r", "--force-group-allocation", "-o", combined, f"@{rsp}"])
    keep = sorted(s for s in defined_globals(nm, [combined], work) if is_public(s, patterns))
    keep_file = work / "keep-global.txt"
    keep_file.write_text("".join(s + "\n" for s in keep))
    run([objcopy, f"--keep-global-symbols={keep_file}", combined])
    out.unlink(missing_ok=True)
    run([tool("zar", "llvm-ar", "ar"), "rcs", out, combined])
    return {"objects": len(objects), "public": len(keep)}


DRECTVE_CHARACTERISTICS = 0x00100A00  # LNK_INFO | LNK_REMOVE | ALIGN_1BYTES, as compilers emit


def coff_section_headers(data):
    """(offset of the first section header, number of sections) for regular and
    /bigobj COFF objects."""
    sig1, sig2 = struct.unpack_from("<HH", data, 0)
    if sig1 == 0 and sig2 == 0xFFFF:  # ANON_OBJECT_HEADER_BIGOBJ
        return 56, struct.unpack_from("<I", data, 44)[0]
    nsections = struct.unpack_from("<H", data, 2)[0]
    optional = struct.unpack_from("<H", data, 16)[0]
    return 20 + optional, nsections


def fix_drectve_characteristics(path):
    """llvm-objcopy --add-section cannot set IMAGE_SCN_LNK_INFO; link.exe needs it to
    read .drectve as linker directives (lld goes by name)."""
    data = bytearray(Path(path).read_bytes())
    offset, count = coff_section_headers(data)
    for i in range(count):
        header = offset + 40 * i
        if data[header:header + 8].rstrip(b"\0") == b".drectve":
            struct.pack_into("<I", data, header + 36, DRECTVE_CHARACTERISTICS)
            Path(path).write_bytes(data)
            return
    sys.exit(f"{path}: .drectve not found after rewrite")


def rewrite_directives(text, renamed, patterns):
    """Linker directives with renamed symbols, and without /EXPORT: entries for
    anything but the public API (genccode's ICU data object exports its symbol)."""
    out = []
    for token in text.split():
        name = re.match(r"(?i)[-/]export:\"?([^,\"]+)", token)
        if name and not is_public(name.group(1), patterns):
            continue
        # Names may be quoted: /INCLUDE:"?name@@...".
        out.append(re.sub(r"(?<=[:=,])(\"?)([^\s:=,\"]+)",
                          lambda m: m.group(1) + ("zg_" + m.group(2) if m.group(2) in renamed else m.group(2)), token))
    return " ".join(out) + " "


def read_directives(objcopy, obj, scratch_dir, key):
    """Text of an object's .drectve section ("" if it has none)."""
    directives = scratch_dir / f"{key}.in"
    scratch = scratch_dir / f"{key}.scratch"
    dump = subprocess.run([objcopy, f"--dump-section=.drectve={directives}", str(obj), str(scratch)],
                          capture_output=True)
    scratch.unlink(missing_ok=True)
    return directives.read_bytes().decode("latin-1") if dump.returncode == 0 and directives.exists() else ""


def alternatename_symbols(text):
    """Both sides of every /alternatename:a=b. These follow conventions shared with
    code outside the bundle (the MSVC STL emits the same pairs in every object,
    including its own static library), so they must keep their names."""
    names = set()
    for a, b in re.findall(r"(?i)[-/]alternatename:\"?([^=\s\"]+)=([^\s\"]+)", text):
        names.update((a, b))
    return names


def rename_coff_object(objcopy, obj, dst, text, mapping, renamed, patterns, work):
    """Rename symbols in one object, including names inside its linker directives
    (/INCLUDE:), which llvm-objcopy leaves as text."""
    new_text = rewrite_directives(text, renamed, patterns) if text.strip() else text
    if new_text.split() == text.split():
        run([objcopy, f"--redefine-syms={mapping}", obj, dst])
        return False
    rewritten = work / "drectve" / f"{dst.name}.out"
    rewritten.write_bytes(new_text.encode("latin-1"))
    run([objcopy, "--remove-section=.drectve", f"--add-section=.drectve={rewritten}",
         f"--redefine-syms={mapping}", obj, dst])
    fix_drectve_characteristics(dst)
    return True


def bundle_coff(objects, patterns, out, work):
    objcopy = tool("llvm-objcopy")
    nm = tool("llvm-nm")
    renamed_dir = work / "renamed"
    renamed_dir.mkdir()
    (work / "drectve").mkdir()
    outputs = [renamed_dir / f"{index:05d}-{Path(obj).name}" for index, obj in enumerate(objects)]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as pool:
        texts = list(pool.map(lambda pair: read_directives(objcopy, pair[0], work / "drectve", pair[1].name),
                              zip(objects, outputs)))
    keep = WINDOWS_KEEP | set().union(*(alternatename_symbols(t) for t in texts))
    defined = defined_globals(nm, objects, work)
    renamed = {s for s in defined
               if not s.startswith(WINDOWS_KEEP_PREFIXES) and s not in keep and not is_public(s, patterns)}
    mapping = work / "redefine-syms.txt"
    # A dllimport reference to a function the bundle defines (__imp_<name>, which
    # the linker resolves to the local definition) follows its rename.
    mapping.write_text("".join(f"{s} zg_{s}\n__imp_{s} __imp_zg_{s}\n" for s in sorted(renamed)))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as pool:
        rewrote = sum(pool.map(lambda args: rename_coff_object(objcopy, *args, mapping, renamed, patterns, work),
                               zip(objects, outputs, texts)))
    print(f"rewrote linker directives in {rewrote} objects; kept {len(keep & defined)} "
          f"/alternatename symbols")
    rsp = work / "renamed.rsp"
    rsp.write_text("".join(f'"{o.as_posix()}"\n' for o in outputs))
    out.unlink(missing_ok=True)
    run([tool("llvm-lib"), "/nologo", f"/out:{out}", f"@{rsp}"])
    public = sorted(s for s in defined if is_public(s, patterns))
    return {"objects": len(objects), "public": len(public), "renamed": len(renamed)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--objects", required=True, help="response file listing engine objects")
    parser.add_argument("--archives", nargs="*", default=[], help="static dependency archives to include")
    parser.add_argument("--exports", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    work = Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    objects = [Path(l.strip().strip('"')) for l in Path(args.objects).read_text().splitlines() if l.strip()]
    ar = tool("llvm-ar", "zar", "ar")
    objects += extract_archives(args.archives, work, ar)
    patterns = export_patterns(args.exports)
    out = Path(args.out)
    stats = (bundle_coff if IS_WINDOWS else bundle_elf)(objects, patterns, out, work)
    print(f"{out.name}: {stats['objects']} objects, {out.stat().st_size / 1e6:.1f} MB, "
          f"{stats['public']} public symbols"
          + (f", {stats['renamed']} symbols renamed zg_*" if "renamed" in stats else ""))


if __name__ == "__main__":
    main()
