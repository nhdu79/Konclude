# Groups LeakSanitizer reports by owner, the first Konclude frame that is not
# an allocator or container helper, and diffs two runs (task 5d,
# docs/INCREMENTAL_WORK_LOG.md).
#
#   python3 Tools/HeapProfiler/lsan_owners.py lsan-100.log lsan-200.log [top]
import collections
import re
import sys

SKIP = re.compile(
    r'(operator new|malloc|calloc|realloc|__interceptor|QListData|QHashData|QMapData|QArrayData|'
    r'QString|QByteArray|QMapNode|QHash<|QMap<|QList<|QSet<|QVector<|std::|'
    r'Konclude::Utilities::|Konclude::Context::|qMalloc)')


def parse(path):
    groups = collections.defaultdict(lambda: [0, 0])
    kind, size, count, frames = None, 0, 0, []

    def flush():
        if kind is None:
            return
        owner = next((f for f, src in frames if src.startswith('Source/') and not SKIP.search(f)), '?')
        groups[(kind, owner)][0] += size
        groups[(kind, owner)][1] += count

    for line in open(path, errors='replace'):
        m = re.match(r'(Direct|Indirect) leak of (\d+) byte\(s\) in (\d+) object', line)
        if m:
            flush()
            kind, size, count, frames = m.group(1), int(m.group(2)), int(m.group(3)), []
            continue
        m = re.match(r'\s+#\d+ 0x[0-9a-f]+ in (.*?) (Source/\S+|\.\./\S+|/\S+|\(\S+)', line)
        if m and kind:
            frames.append((re.sub(r'\(.*', '', m.group(1))[:130], m.group(2)))
    flush()
    return groups


a, b = parse(sys.argv[1]), parse(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 20
for kind in ('Direct', 'Indirect'):
    total = lambda g: sum(v[0] for k, v in g.items() if k[0] == kind)
    print(f"\n== {kind}: {total(a) / 1024:.0f} kB -> {total(b) / 1024:.0f} kB")
    rows = [(b.get(k, [0, 0])[0] - a.get(k, [0, 0])[0], b.get(k, [0, 0])[1] - a.get(k, [0, 0])[1], k[1])
            for k in set(a) | set(b) if k[0] == kind]
    for delta, count, owner in sorted(rows, reverse=True)[:top]:
        print(f"  {delta / 1024:8.0f} kB {count:7d} obj  {owner}")
