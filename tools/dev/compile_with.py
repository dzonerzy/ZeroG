"""Recompile one unit from compile_commands.json with another clang-cl, to compare compilers.

    compile_with.py <build dir> <unit file suffix> <clang-cl path>
"""
import json
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import build  # noqa: E402

build_dir, suffix, compiler = sys.argv[1:4]
entries = json.load(open(Path(build_dir) / "compile_commands.json"))
entry = next(e for e in entries if e["file"].replace("\\", "/").endswith(suffix))
args = shlex.split(entry["command"], posix=False)
args[0] = compiler
out = (Path(build_dir) / "compare.obj").resolve()
# Drop outputs and the precompiled header (built by the other compiler); keep /FI.
args = [a for a in args if not a.lower().startswith(("/fo", "-fo", "/fd", "-fd", "/yu", "-yu", "/fp", "-fp"))]
args.insert(args.index("--") if "--" in args else len(args) - 1, f"/Fo{out}")
result = subprocess.run(args, cwd=entry["directory"], env=build.build_env(), capture_output=True, text=True)
errors = [l for l in (result.stdout + result.stderr).splitlines() if " error: " in l]
print(f"{Path(compiler).name} exit {result.returncode}, {len(errors)} errors")
print("\n".join(errors[:5] or (result.stdout + result.stderr).splitlines()[-8:]))
