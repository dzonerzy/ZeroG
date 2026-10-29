import collections
import re
import sys

# Group a Ninja log into failed units and the distinct errors of each.
log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
units, current = collections.OrderedDict(), None
for line in log:
    m = re.match(r"^FAILED: (\S+)", line)
    if m:
        current = m.group(1)
        units[current] = collections.Counter()
        continue
    if current and " error: " in line:
        units[current][re.sub(r"^.*? error: ", "", line).strip()[:140]] += 1

kinds = collections.defaultdict(list)
for unit, errors in units.items():
    kinds[tuple(sorted(e.split(" '")[0] for e in errors))].append(unit)
for key, members in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
    print(f"{len(members):4d} units: {' | '.join(key)[:220]}")
    for unit in members[:4]:
        print("        ", unit.split("/")[-1])
