"""Tests of the build and maintenance scripts in tools/ that need no engine build,
for CI's quick checks:

    python -m unittest discover -s tests/tools
"""

import re
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import assets  # noqa: E402
import icu_data  # noqa: E402
import size  # noqa: E402


def read_archive(archive):
    """Parses an archive back as the engine does (MemoryFS.cpp): path -> (MIME, bytes)."""
    magic, version, count, flags = assets.HEADER.unpack_from(archive, 0)
    files = {}
    for i in range(count):
        offset, stored_size, size_, path_offset, path_length, mime_offset, mime_length, compression, _ = \
            assets.ENTRY.unpack_from(archive, assets.HEADER.size + i * assets.ENTRY.size)
        data = archive[offset:offset + stored_size]
        if compression == 1:
            data = zlib.decompress(data)
        assert len(data) == size_
        path = archive[path_offset:path_offset + path_length].decode()
        files[path] = (archive[mime_offset:mime_offset + mime_length].decode(), data)
    return magic, version, flags, files


class AssetsTest(unittest.TestCase):
    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            (root / "js").mkdir()
            (root / "index.html").write_text("<p>" + "hello " * 200 + "</p>")
            (root / "js" / "app.js").write_text("console.log(1)")
            (root / "logo.png").write_bytes(bytes(range(256)) * 4)
            (root / "app.js.map").write_text("{}")
            files = assets.collect(root, ["*.map"])
            archive, _ = assets.pack(files, {".js": "text/javascript"}, compress=True)
            magic, version, flags, unpacked = read_archive(archive)
        self.assertEqual((magic, version, flags), (assets.MAGIC, assets.VERSION, 0))
        self.assertEqual(sorted(unpacked), ["/index.html", "/js/app.js", "/logo.png"])  # excludes applied, sorted
        self.assertTrue(unpacked["/index.html"][1].startswith(b"<p>hello"))
        self.assertEqual(unpacked["/js/app.js"], ("text/javascript", b"console.log(1)"))
        self.assertEqual(unpacked["/logo.png"][1], bytes(range(256)) * 4)

    def test_compression_only_where_it_pays(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            (root / "text.txt").write_text("a" * 10000)
            (root / "image.png").write_bytes(b"\0" * 10000)  # compressible, but a stored format
            archive, entries = assets.pack(assets.collect(root, []), {}, compress=True)
        compression = {path.decode(): entry_compression for path, _, _, _, entry_compression in entries}
        self.assertEqual(compression, {"/image.png": 0, "/text.txt": 1})

    def test_reproducible(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            for name in ("b.txt", "a.txt", "c/d.txt"):
                (root / name).parent.mkdir(exist_ok=True)
                (root / name).write_text(name)
            first, _ = assets.pack(assets.collect(root, []), {}, compress=True)
            second, _ = assets.pack(assets.collect(root, []), {}, compress=True)
        self.assertEqual(first, second)

    def test_rejects_unsafe_names(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            # A control character (a backslash would be a separator on Windows).
            with self.assertRaises(SystemExit):
                assets.archive_path(root, root / "a\x01b")


class ICUDataTest(unittest.TestCase):
    def matches(self, item):
        return any(re.fullmatch(pattern, item) for pattern in icu_data.UNUSED_SERVICES)

    def test_unused_services(self):
        for item in ("translit/root.res", "translit/el.res", "unames.icu", "confusables.cfu", "rfc3491.spp", "nfkc_cf.nrm", "nfkc_scf.nrm"):
            self.assertTrue(self.matches(item), item)

    def test_used_data_is_kept(self):
        # Normalization the engine uses (String.prototype.normalize, IDNA), break
        # iteration, collation and locales.
        for item in ("nfkc.nrm", "uts46.nrm", "brkitr/cjdict.dict", "coll/root.res", "root.res", "zoneinfo64.res"):
            self.assertFalse(self.matches(item), item)

    def test_language_filter(self):
        languages = ["en", "it", "zh"]
        self.assertTrue(icu_data.keeps_language("it_IT", languages))
        self.assertTrue(icu_data.keeps_language("zh_Hant_TW", languages))
        self.assertFalse(icu_data.keeps_language("de_DE", languages))


class SizeTest(unittest.TestCase):
    # lld-link's /MAP, abridged: a section table, then symbols by address.
    MAP = """ ZeroGHello

 Timestamp is 6ae62503 (Sat Oct 31 18:27:31 2026)

 Preferred load address is 0000000140000000

 Start         Length     Name                   Class
 0001:00000000 00000100H .text                   CODE
 0002:00000000 00000040H .rdata                  DATA
 0004:00000000 00001000H .pdata                  DATA

  Address         Publics by Value              Rva+Base               Lib:Object

 0001:00000000       f1                         0000000140001000 f   WebCore:dom.cpp.obj
 0001:00000060       f2                         0000000140001060 f   JavaScriptCore:jit.cpp.obj
 0002:00000000       d1                         0000000140002000     WebCore:dom.cpp.obj
 0004:00000000       $pdata$f2                  0000000140004000     libcpmt:xonce2.obj
 0001:000000f0       f3                         00000001400010f0 f   main.obj
"""

    def test_windows_map_breakdown(self):
        sizes = size.windows_map_breakdown(self.MAP.splitlines())
        # A symbol runs to the next one in its section, or to the section's end;
        # .pdata is left out (its long unnamed runs would go to one entry).
        self.assertEqual(sizes, {"WebCore": 0x60 + 0x40, "JavaScriptCore": 0x90, "(objects)": 0x10})


class APIDocTest(unittest.TestCase):
    def test_every_function_documented(self):
        import apidoc
        lines = apidoc.HEADER.read_text(encoding="utf-8").splitlines()
        pages = apidoc.render(apidoc.parse(lines), apidoc.preamble(lines))
        declared = set(re.findall(r"\bZG_API\b[^;(]*?\b(zg_\w+)\s*\(", "\n".join(lines)))
        indexed = set(re.findall(r"\[`(zg_\w+)`\]", pages["index.md"]))
        self.assertEqual(declared - indexed, set())
        # Each function has its own heading on a section page.
        headings = set(re.findall(r"^### `(zg_\w+)`", "".join(text for name, text in pages.items() if name != "index.md"), re.M))
        self.assertEqual(declared - headings, set())

    def test_declaration_with_trailing_comment(self):
        import apidoc
        header = ["/* " + "-" * 72 + " */", "/* Things */", "",
                  "ZG_API int zg_a(void); /* first */", "/* The second. */", "ZG_API int zg_b(int x);"]
        items = apidoc.parse(header)[0]["items"]
        self.assertEqual([(i[1], i[3]) for i in items], [("zg_a", ""), ("zg_b", "The second.")])


class SymbolsTest(unittest.TestCase):
    def test_pe_identity(self):
        import symbols
        # A minimal PE header: e_lfanew at 0x3C, then "PE\0\0", the file header
        # (TimeDateStamp at +8) and the optional header (SizeOfImage at +56).
        data = bytearray(0x200)
        struct.pack_into("<I", data, 0x3C, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<I", data, 0x80 + 8, 0x6AE62503)
        struct.pack_into("<I", data, 0x80 + 24 + 56, 0x5D000)
        with tempfile.TemporaryDirectory() as work:
            exe = Path(work) / "app.exe"
            exe.write_bytes(bytes(data))
            self.assertEqual(symbols.pe_identity(exe), "6AE625035D000")


if __name__ == "__main__":
    unittest.main()
