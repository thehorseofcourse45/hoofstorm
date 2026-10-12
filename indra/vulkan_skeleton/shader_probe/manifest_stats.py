import sys, collections
d = sys.argv[1]
programs = []          # (name, [attached object file names in order])
cur = None
for line in open(d + '/manifest.txt', encoding='utf-8'):
    line = line.strip()
    if line.startswith('PROGRAM '):
        cur = (line[8:], [])
        programs.append(cur)
    elif line.startswith('ATTACH ') and cur is not None:
        cur[1].append(line[7:])
print('programs:', len(programs))
per_stage = collections.Counter()
multi = 0
sizes = collections.Counter()
lib_users = collections.Counter()
for name, objs in programs:
    by = collections.defaultdict(list)
    for o in objs:
        by[o.rsplit('.', 1)[1]].append(o)
    for st, lst in by.items():
        sizes[(st, len(lst))] += 1
        if len(lst) > 1:
            multi += 1
        for o in lst[:-1] if False else lst:
            lib_users[o.split('_', 1)[1]] += 1
print('stage-modules (program x stage): ', sum(sizes.values()))
print('  with exactly 1 object :', sum(c for (st, n), c in sizes.items() if n == 1))
print('  with 2+ objects       :', sum(c for (st, n), c in sizes.items() if n > 1))
print('objects per stage-module distribution:')
for (st, n), c in sorted(sizes.items()):
    print('   %s x%d objects: %d' % (st, n, c))
print('most-shared objects (attached to how many programs):')
for k, c in lib_users.most_common(12):
    print('  %4d  %s' % (c, k))
