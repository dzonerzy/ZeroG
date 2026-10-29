import collections
import re
import sys

log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
patterns = sys.argv[2:]
for pattern in patterns:
    sites = collections.Counter()
    for line in log:
        m = re.match(r"(.+?)\((\d+),\d+\): error: (.*)", line)
        if m and re.search(pattern, m.group(3)):
            path = m.group(1).replace("\\", "/").split("/Source/")[-1]
            sites[(path, m.group(2))] += 1
    print(f"== {pattern}")
    for (path, line_no), n in sites.most_common(8):
        print(f"  {n:4d} {path}:{line_no}")
