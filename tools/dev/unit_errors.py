import re
import sys

# First distinct errors (with location) of failed units whose name matches an argument.
log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
wanted = sys.argv[2:]
current = None
shown = {}
for line in log:
    m = re.match(r"^FAILED: (\S+)", line)
    if m:
        name = m.group(1).split("/")[-1]
        current = name if any(w in name for w in wanted) else None
        if current:
            shown[current] = []
        continue
    if current and (" error: " in line or "fatal error" in line):
        loc = line.replace("\\", "/").split("/Source/")[-1].split("/zerog-minimal/")[-1]
        msg = loc[:230]
        if msg not in shown[current] and len(shown[current]) < 3:
            shown[current].append(msg)
for unit, errors in shown.items():
    print("==", unit)
    for e in errors:
        print("  ", e)
