"""Source-level 'linking' probe.

GL links several shader objects per stage into one program; a Vulkan stage is one SPIR-V module. For each program stage in the
manifest written by the viewer (FS_SHADER_DUMP_DIR), merge its objects the way a Vulkan path would have to, translate the result
(see shader_vk_probe.translate) and compile it with glslc for Vulkan. Control: the merged text must also validate as plain GLSL,
so that a failure can be blamed on Vulkan and not on the merge.

Merge rules (the experiment, not necessarily the final design):
  - one #version, one set of #extension lines
  - top-level declarations are split into chunks (a statement ending in ';', or a block closed back to depth 0);
    a chunk that is identical (whitespace-normalised) to an earlier one is dropped
  - everything else is concatenated in attach order
"""
import collections, concurrent.futures, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_vk_probe as P  # noqa: E402

STAGE_NAME = {'vert': 'vertex', 'frag': 'fragment', 'geom': 'geometry'}


def parse_manifest(dump_dir):
    programs, cur = [], None
    for line in open(os.path.join(dump_dir, 'manifest.txt'), encoding='utf-8'):
        line = line.strip()
        if line.startswith('PROGRAM '):
            cur = (line[8:], [])
            programs.append(cur)
        elif line.startswith('ATTACH ') and cur is not None and line[7:] != '?':
            if line[7:] not in cur[1]:
                cur[1].append(line[7:])
    return programs


def norm(chunk):
    chunk = re.sub(r'//[^\n]*', '', chunk)           # trailing comments do not make two declarations different
    chunk = re.sub(r'/\*.*?\*/', '', chunk, flags=re.S)
    return re.sub(r'\s+', ' ', chunk).strip()


STRUCT = re.compile(r'^\s*struct\s+(\w+)')


def dedupe_key(ctx, text):
    clean = norm(text)                                     # comments stripped, whitespace collapsed
    m = STRUCT.match(clean)
    body = ('struct ' + m.group(1)) if m else clean         # structs are identified by name
    return (tuple(ctx), body)


def chunks_of(src):
    """Split source into top-level chunks. Preprocessor lines are their own chunks."""
    out, cur, depth = [], [], 0
    in_block = False
    for line in src.split('\n'):
        stripped = line.strip()
        # comment-only lines (including block comments) are their own chunks, never glued to a declaration
        if not cur:
            if in_block:
                if '*/' in stripped:
                    in_block = False
                out.append(('blank', line))
                continue
            if stripped.startswith('/*'):
                if '*/' not in stripped[2:]:
                    in_block = True
                out.append(('blank', line))
                continue
            if stripped.startswith('//'):
                out.append(('blank', line))
                continue
        if depth == 0 and not cur and stripped.startswith('#'):
            out.append(('pp', line))
            continue
        if not stripped and not cur:
            out.append(('blank', line))
            continue
        cur.append(line)
        code = re.sub(r'//.*', '', line)
        depth += code.count('{') - code.count('}')
        if depth == 0:
            ends = code.rstrip().endswith(';') or code.rstrip().endswith('}')
            if ends:
                out.append(('decl', '\n'.join(cur)))
                cur = []
    if cur:
        out.append(('decl', '\n'.join(cur)))
    return out


def merge(sources):
    seen, out = set(), []
    first = True
    version_done = False
    for src in sources:
        ctx = []  # enclosing #if conditions of the current object (each object is balanced on its own)
        for kind, text in chunks_of(src):
            if kind == 'pp':
                t = text.strip()
                d = re.match(r'#\s*(\w+)\s*(.*)', t)
                word, rest = (d.group(1), d.group(2)) if d else ('', '')
                if word in ('if', 'ifdef', 'ifndef'):
                    ctx.append(norm(t))
                elif word in ('else', 'elif'):
                    if ctx:
                        ctx[-1] = ctx[-1] + ' ' + norm(t)
                elif word == 'endif':
                    if ctx:
                        ctx.pop()
                if word == 'version':
                    if version_done:
                        continue
                    version_done = True
                if word in ('extension', 'define') or t.startswith('precision'):
                    key = dedupe_key(ctx, t)
                    if key in seen:
                        continue
                    seen.add(key)
                out.append(text)
            elif kind == 'decl':
                key = dedupe_key(ctx, text)
                if key in seen:
                    continue
                seen.add(key)
                out.append(text)
            else:
                out.append(text)
        first = False
    return '\n'.join(out) + '\n'


def work(args):
    idx, pid, prog, stage_ext, objs, dump_dir, out_dir = args
    stage = STAGE_NAME[stage_ext]
    safe = re.sub(r'[^A-Za-z0-9]+', '_', prog)[:40]
    base = '%03d_%s.%s' % (idx, safe, stage_ext)
    sources = [open(os.path.join(dump_dir, o), encoding='utf-8', errors='replace').read() for o in objs]
    if os.environ.get('PREPROCESS') == '1':
        # resolve #if/#define per object first, so no conditional context is left when merging
        pre = []
        for o in objs:
            p = subprocess.run([P.GLSLANG, '-E', '-S', {'vertex': 'vert', 'fragment': 'frag', 'geometry': 'geom'}[stage],
                                os.path.join(dump_dir, o)], capture_output=True, text=True)
            pre.append(p.stdout if p.returncode == 0 else open(os.path.join(dump_dir, o), encoding='utf-8', errors='replace').read())
        sources = pre
    merged = merge(sources)
    mpath = os.path.join(out_dir, base + '.merged.glsl')
    open(mpath, 'w', encoding='utf-8').write(merged)
    ctrl = P.validate_gl(mpath, stage)
    xlat, stats = P.translate(merged)
    xpath = os.path.join(out_dir, base + '.merged.vk.glsl')
    open(xpath, 'w', encoding='utf-8').write(xlat)
    ok, err = P.glslc(xpath, stage, 'vulkan1.3', ['-fauto-bind-uniforms', '-fauto-map-locations'], os.path.join(out_dir, base + '.spv'))
    return {'idx': idx, 'pid': pid, 'base': base, 'prog': prog, 'stage': stage_ext, 'objects': len(objs), 'gl': ctrl, 'vk': (ok, err), 'stats': stats, 'loose': stats['loose_uniform']}


def main():
    dump_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    jobs = []
    for pid, (prog, objs) in enumerate(parse_manifest(dump_dir)):
        by = collections.defaultdict(list)
        for o in objs:
            by[o.rsplit('.', 1)[1]].append(o)
        for ext, lst in by.items():
            jobs.append((len(jobs), pid, prog, ext, lst, dump_dir, out_dir))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        res = list(ex.map(work, jobs))

    n = len(res)
    gl_ok = [r for r in res if r['gl'][0]]
    vk_ok = [r for r in res if r['vk'][0]]
    print('program stages (program x stage): %d  (%d programs)' % (n, len(set(r['pid'] for r in res))))
    print('control, merged text validates as GLSL : %4d / %d' % (len(gl_ok), n))
    print('Vulkan SPIR-V compile after merge + translation : %4d / %d' % (len(vk_ok), n))
    print('  of the %d whose merge validates as GLSL: %d / %d compile for Vulkan' % (len(gl_ok), sum(r['vk'][0] for r in gl_ok), len(gl_ok)))
    byst = collections.defaultdict(lambda: [0, 0])
    for r in res:
        byst[r['stage']][1] += 1
        byst[r['stage']][0] += r['vk'][0]
    print('by stage: ' + ', '.join('%s %d/%d' % (k, v[0], v[1]) for k, v in sorted(byst.items())))
    print('loose uniforms translated into blocks (summed over merged stages): %d' % sum(r['loose'] for r in res))

    def norm_err(e):
        for line in e.split('\n'):
            if 'error' in line.lower():
                line = re.sub(r'^.*?error:\s*', '', line)
                line = re.sub(r"'[^']*'", "'X'", line)
                line = re.sub(r'\d+', 'N', line)
                return line.strip()[:150]
        return e.split('\n')[0][:150] if e else 'unknown'

    for title, rows, key in (('merge itself invalid as GLSL (control failures)', [r for r in res if not r['gl'][0]], 'gl'),
                             ('Vulkan failures, among merges that validate as GLSL', [r for r in gl_ok if not r['vk'][0]], 'vk')):
        g = collections.Counter()
        ex = {}
        for r in rows:
            k = norm_err(r[key][1])
            g[k] += 1
            ex.setdefault(k, '%s [%s]' % (r['prog'], r['stage']))
        print('\n%s: %d' % (title, len(rows)))
        for k, c in g.most_common(14):
            print('  %4d  %s   e.g. %s' % (c, k, ex[k]))

    # Program-level link: stages are linked together so that glslang matches vertex outputs to fragment inputs by name
    # and assigns the locations consistently (glslc compiles each stage alone and cannot).
    by_prog = collections.defaultdict(list)
    for r in res:
        by_prog[r['pid']].append(r)

    def link_prog(item):
        pid, rows = item
        prog = rows[0]['prog']
        # glslang takes the stage from the file extension (-S would apply to every file), and -l links them into one module
        cmd = [P.GLSLANG, '-l', '-V', '--auto-map-bindings', '--auto-map-locations', '--target-env', 'vulkan1.3']
        for r in sorted(rows, key=lambda r: r['stage']):
            src = os.path.join(out_dir, r['base'] + '.merged.vk.glsl')
            dst = os.path.join(out_dir, 'prog%03d.%s' % (rows[0]['idx'], r['stage']))
            with open(src, 'rb') as a, open(dst, 'wb') as b:
                b.write(a.read())
            cmd.append(dst)
        cmd += ['-o', os.path.join(out_dir, '%03d_program.spv' % rows[0]['idx'])]
        p = subprocess.run(cmd, capture_output=True, text=True)
        return prog, p.returncode == 0, (p.stdout + p.stderr)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        linked = list(ex.map(link_prog, by_prog.items()))
    ok_progs = [x for x in linked if x[1]]
    print('\nprogram-level link (stages linked together, interfaces matched by name): %d / %d programs' % (len(ok_progs), len(linked)))
    g = collections.Counter()
    ex_name = {}
    for prog, ok, out in linked:
        if not ok:
            k = norm_err(out)
            g[k] += 1
            ex_name.setdefault(k, prog)
    for k, c in g.most_common(10):
        print('  %4d  %s   e.g. %s' % (c, k, ex_name[k]))

    with open(os.path.join(out_dir, 'link_summary.tsv'), 'w') as f:
        f.write('program\tstage\tobjects\tmerged_gl_ok\tvulkan_ok\n')
        for r in res:
            f.write('%s\t%s\t%d\t%d\t%d\n' % (r['prog'], r['stage'], r['objects'], r['gl'][0], r['vk'][0]))


if __name__ == '__main__':
    main()
