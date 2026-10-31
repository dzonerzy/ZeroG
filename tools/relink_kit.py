#!/usr/bin/env python3
"""Relink kit for applications that link the engine statically (docs/design/
licensing-lgpl.md).

WebCore and JavaScriptCore are LGPL: whoever receives an application linked
with them statically must be able to relink it with a modified engine. A relink
kit is what that takes, besides the engine's source: the application's own
object files, the exact link (driver, arguments, system libraries) and which
engine it was linked with. With the kit and an engine bundle built from (a
modified) ZeroG (`tools/build.py all`, `lib/zerog-engine.lib` or
`lib/libzerog-engine.a`), `relink` produces the application again.

    python tools/relink_kit.py create --name APP --object a.obj [--object ...]
                                      [--system-libs FILE] [--link-arg ARG ...]
                                      [--profile minimal] --out KIT_DIR
    python KIT_DIR/relink.py relink KIT_DIR --bundle ENGINE_ARCHIVE --out APP

The kit carries this script as relink.py, so relinking needs neither the
application's sources nor this repository.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"

README = """# Relink kit for {name}

{name} links the ZeroG engine statically. The engine contains LGPL code
(WebCore, JavaScriptCore, parts of WTF): this kit lets you relink {name} with
a modified engine, as the LGPL allows.

- `objects/`: {name}'s own object files, as they were linked.
- `kit.json`: the link: driver, arguments, system libraries, platform.
- `relink.py`: relinks them with an engine bundle you build.

The engine it was linked with: ZeroG {zerog_commit}, WebKit {webkit_revision},
profile `{profile}`.

## Relinking

1. Get the ZeroG source of that version (or your modified one) and build its
   bundle with the same profile:
   `python tools/build.py all --profile {profile}`; the bundle is
   `build/zerog-{profile}/lib/{bundle_name}`.
2. Relink:
   `python relink.py relink . --bundle path/to/{bundle_name} --out {output_name}`
   {toolchain}
"""


def git_commit(root):
    try:
        return subprocess.run(["git", "-C", str(root), "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def cmd_create(args):
    root = Path(__file__).resolve().parent.parent
    kit = Path(args.out)
    objects = kit / "objects"
    shutil.rmtree(kit, ignore_errors=True)
    objects.mkdir(parents=True)
    names = []
    for path in map(Path, args.object):
        # Distinct names: two sources may have had the same stem.
        target = objects / (f"{len(names):02d}-{path.name}")
        shutil.copy2(path, target)
        names.append(target.name)
    system = []
    if args.system_libs:
        system = [line.strip() for line in Path(args.system_libs).read_text().splitlines() if line.strip()]
    webkit_revision = (root / "WEBKIT_REVISION").read_text().strip() if (root / "WEBKIT_REVISION").exists() else "unknown"
    description = {
        "name": args.name,
        "platform": sys.platform,
        "objects": names,
        "system_libraries": system,
        "link_arguments": args.link_arg or [],
        "bundle_name": "zerog-engine.lib" if IS_WINDOWS else "libzerog-engine.a",
        "engine": {"zerog_commit": git_commit(root), "webkit_revision": webkit_revision, "profile": args.profile},
    }
    (kit / "kit.json").write_text(json.dumps(description, indent=2) + "\n")
    shutil.copy2(Path(__file__), kit / "relink.py")
    toolchain = ("From a Visual Studio x64 developer prompt, with clang-cl (the LLVM version in ZeroG's "
                 "toolchain.json) on PATH." if IS_WINDOWS else
                 "With the C compiler ZeroG's bundle was built for (zig cc for the release builds), as CC.")
    (kit / "README.md").write_text(README.format(
        name=args.name, bundle_name=description["bundle_name"],
        output_name=args.name + (".exe" if IS_WINDOWS else ""), toolchain=toolchain, **description["engine"]))
    print(f"relink kit for {args.name}: {kit} ({len(names)} objects)")


def cmd_relink(args):
    kit = Path(args.kit)
    description = json.loads((kit / "kit.json").read_text())
    if description["platform"] != sys.platform:
        sys.exit(f"this kit is for {description['platform']}, not {sys.platform}")
    objects = [str(kit / "objects" / name) for name in description["objects"]]
    out = str(Path(args.out))
    if IS_WINDOWS:
        # The objects name their C runtime themselves (/MT); clang-cl drives lld.
        cmd = ["clang-cl", "/nologo", "-fuse-ld=lld", *objects, f"/Fe{out}", "/link",
               args.bundle, *description["system_libraries"], *description["link_arguments"]]
    else:
        cc = os.environ.get("CC", "cc")
        cmd = [*cc.split(), *objects, args.bundle, *description["system_libraries"], *description["link_arguments"], "-o", out]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    print(f"relinked {description['name']}: {out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create", help="make a kit from an application's objects")
    create.add_argument("--name", required=True)
    create.add_argument("--object", action="append", required=True)
    create.add_argument("--system-libs", help="file listing the system libraries, one per line (lib/zerog-engine.libs.txt)")
    create.add_argument("--link-arg", action="append", help="the application's own link arguments")
    create.add_argument("--profile", default="minimal")
    create.add_argument("--out", required=True)
    relink = commands.add_parser("relink", help="link a kit's objects with an engine bundle")
    relink.add_argument("kit")
    relink.add_argument("--bundle", required=True)
    relink.add_argument("--out", required=True)
    args = parser.parse_args()
    {"create": cmd_create, "relink": cmd_relink}[args.command](args)


if __name__ == "__main__":
    main()
