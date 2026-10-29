#!/usr/bin/env python3
"""Manage the upstream WebKit checkout used by the ZeroG port.

The checkout lives in third_party/webkit and is never edited by hand:

    fetch     shallow-fetch the commit pinned in WEBKIT_REVISION
    prepare   reset the checkout to that commit, apply patches/series,
              then copy port/overlay/ on top (new files only)
    status    show pinned vs checked-out revision and patch/overlay counts
    diff      print the full diff of the checkout against WEBKIT_REVISION
              (patched + hand-edited); redirect it to write a new patch

Changes to upstream files belong in patches/; new port files belong in
port/overlay/ (mirroring the WebKit tree). `prepare` refuses to let the
overlay replace an existing upstream file, so the two never get mixed up.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEBKIT = ROOT / "third_party" / "webkit"
PATCHES = ROOT / "patches"
OVERLAY = ROOT / "port" / "overlay"
REMOTE = "https://github.com/WebKit/WebKit.git"


def pinned_revision():
    return (ROOT / "WEBKIT_REVISION").read_text().strip()


def git(*args, capture=False, check=True):
    cmd = ["git", "-C", str(WEBKIT), *args]
    if capture:
        return subprocess.run(cmd, check=check, text=True, capture_output=True).stdout.strip()
    return subprocess.run(cmd, check=check)


def head_revision():
    if not (WEBKIT / ".git").exists():
        return None
    return git("rev-parse", "HEAD", capture=True, check=False) or None


def series():
    path = PATCHES / "series"
    if not path.exists():
        return []
    lines = (l.split("#", 1)[0].strip() for l in path.read_text().splitlines())
    return [PATCHES / l for l in lines if l]


def overlay_files():
    if not OVERLAY.exists():
        return []
    return sorted(p for p in OVERLAY.rglob("*") if p.is_file() and p.name != ".gitkeep")


def cmd_fetch(args):
    rev = pinned_revision()
    if not (WEBKIT / ".git").exists():
        WEBKIT.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "-q", str(WEBKIT)], check=True)
        git("config", "core.longpaths", "true")
        # Keep upstream line endings: generator scripts and patches expect LF.
        git("config", "core.autocrlf", "false")
        git("remote", "add", "origin", REMOTE)
    if head_revision() == rev and not args.force:
        print(f"webkit already at {rev}")
        return
    git("fetch", "--depth", "1", "origin", rev)
    git("-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD")
    print(f"webkit checked out at {rev}")


def modified_files():
    """Files of the checkout that differ from HEAD: patched, overlaid or hand-edited."""
    changed = git("diff", "--name-only", "HEAD", capture=True).splitlines()
    untracked = git("ls-files", "--others", "--exclude-standard", capture=True).splitlines()
    return [WEBKIT / p for p in changed + untracked if p]


def snapshot(paths):
    return {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths if p.is_file()}


def restore_mtimes(before):
    """Give unchanged files their old mtime back, so the build only redoes what changed."""
    for path, (content, mtime) in before.items():
        if path.is_file() and path.read_bytes() == content:
            os.utime(path, ns=(mtime, mtime))


def cmd_prepare(args):
    rev = pinned_revision()
    if head_revision() is None:
        sys.exit("no webkit checkout: run `tools/webkit.py fetch` first")
    before = snapshot(modified_files())
    # Back to a pristine tree at the pinned commit. Only third_party/webkit is touched.
    git("-c", "advice.detachedHead=false", "checkout", "-q", "--force", rev)
    git("clean", "-q", "-fdx", "--exclude=/WebKitBuild/")

    for patch in series():
        print(f"applying {patch.relative_to(ROOT)}")
        git("apply", "--index", "--whitespace=nowarn", str(patch))

    for src in overlay_files():
        rel = src.relative_to(OVERLAY)
        dst = WEBKIT / rel
        tracked = git("ls-files", "--error-unmatch", rel.as_posix(), capture=True, check=False)
        if tracked:
            sys.exit(f"overlay file {rel} replaces an upstream file: make it a patch instead")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    restore_mtimes(before)
    print(f"prepared {rev[:12]}: {len(series())} patches, {len(overlay_files())} overlay files")


def cmd_status(args):
    rev, head = pinned_revision(), head_revision()
    print(f"pinned:  {rev}")
    print(f"checkout: {head or '(none)'}{'' if head == rev else '  <-- differs'}")
    print(f"patches: {len(series())}   overlay files: {len(overlay_files())}")


def cmd_diff(args):
    # Against the commit, not the index: `prepare` stages applied patches, so a
    # plain `git diff` would only show edits made since then.
    cmd = ["diff", pinned_revision()]
    if args.stat:
        cmd.insert(1, "--stat")
    git(*cmd)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    fetch = sub.add_parser("fetch")
    fetch.add_argument("--force", action="store_true")
    fetch.set_defaults(func=cmd_fetch)
    sub.add_parser("prepare").set_defaults(func=cmd_prepare)
    sub.add_parser("status").set_defaults(func=cmd_status)
    diff = sub.add_parser("diff")
    diff.add_argument("--stat", action="store_true")
    diff.set_defaults(func=cmd_diff)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
