"""Shader feasibility probe: how much of the viewer's real GLSL compiles to Vulkan SPIR-V?

Input:  a directory written by the viewer with FS_SHADER_DUMP_DIR (final source of every shader object).
Output: a report on stdout and per-shader results in <out_dir>.

For each shader it compiles three variants with glslc:
  gl      untouched source, OpenGL target. A control: failures here are problems with the dump, not with Vulkan.
  vk_raw  untouched source, Vulkan target.
  vk_xlat the source after the mechanical translations below, Vulkan target.

Translations (what a real Vulkan path would have to do automatically):
  1. #version -> 450
  2. loose (non-opaque) uniforms -> uniform blocks (one block per uniform here, to stay safe inside #if blocks)
  3. legacy no-op #extension lines removed
  4. bindings and locations assigned with glslc -fauto-bind-uniforms -fauto-map-locations
"""
import os, re, subprocess, sys, collections, concurrent.futures

SDK_BIN = os.path.join(os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.357.0'), 'Bin')
GLSLC = os.path.join(SDK_BIN, 'glslc.exe')
GLSLANG = os.path.join(SDK_BIN, 'glslangValidator.exe')
HAS_MAIN = re.compile(r'\bvoid\s+main\s*\(')
STAGES = {'vert': 'vertex', 'frag': 'fragment', 'geom': 'geometry'}
OPAQUE = re.compile(r'^\s*uniform\s+(?:(?:highp|mediump|lowp)\s+)?(?:[iu]?sampler|[iu]?image|atomic_uint|[iu]?texture|subpassInput)', re.I)
UNIFORM = re.compile(r'^(\s*)uniform\s+([^;{}]+?)\s*;\s*(//.*)?$')
LEGACY_EXT = re.compile(r'^\s*#extension\s+(GL_ARB_shader_texture_lod|GL_EXT_gpu_shader4|GL_ARB_texture_rectangle)\b')


def translate(src):
    out, counter, stats = [], 0, collections.Counter()
    for line in src.split('\n'):
        if re.match(r'^\s*#version\b', line):
            out.append('#version 450')
            continue
        if LEGACY_EXT.match(line):
            stats['legacy_ext_removed'] += 1
            continue
        m = UNIFORM.match(line)
        if m and not OPAQUE.match(line):
            decl = m.group(2)
            if '=' in decl:                      # uniform initializers are not allowed in blocks
                stats['uniform_initializer'] += 1
                decl = decl.split('=')[0].strip()
            # "type name[, name2]": name the block after the uniform(s) so the same uniform in two stages is the same block
            names = re.findall(r'\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:,|$)', decl)
            out.append('%slayout(std140) uniform LLU_%s { %s; };' % (m.group(1), '_'.join(names) or str(counter), decl))
            counter += 1
            stats['loose_uniform'] += 1
            continue
        out.append(line)
    return '\n'.join(out), stats


def validate_gl(path, stage):
    # control: plain GLSL validation with OpenGL semantics (no SPIR-V generation)
    short = {'vertex': 'vert', 'fragment': 'frag', 'geometry': 'geom'}[stage]
    p = subprocess.run([GLSLANG, '-S', short, path], capture_output=True, text=True)
    return p.returncode == 0, ((p.stdout or '') + (p.stderr or '')).strip()


def glslc(path, stage, env, extra, out_spv):
    cmd = [GLSLC, '-fshader-stage=' + stage, '--target-env=' + env] + extra + ['-x', 'glsl', path, '-o', out_spv]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode == 0, (p.stderr or '').strip()


def first_error(text):
    for line in text.split('\n'):
        if 'error' in line.lower():
            # drop file path and line number so identical problems group together
            line = re.sub(r'^.*?:\d+:\s*', '', line)
            line = re.sub(r"'[^']*'", "'X'", line)
            return line.strip()[:140]
    return text.split('\n')[0][:140] if text else 'unknown'


def probe(args):
    path, out_dir = args
    name = os.path.basename(path)
    ext = name.rsplit('.', 1)[1]
    stage = STAGES.get(ext)
    if not stage:
        return None
    src = open(path, encoding='utf-8', errors='replace').read()
    library = not HAS_MAIN.search(src)
    if library:
        # a shader object without main(): GL links several objects into one program. Add an empty main so the rest can be checked.
        src += '\nvoid main() {}\n'
    gpath = os.path.join(out_dir, name + '.gl.glsl')
    open(gpath, 'w', encoding='utf-8').write(src)
    xlat, stats = translate(src)
    if library:
        stats['library_object_no_main'] += 1
    xpath = os.path.join(out_dir, name + '.vk.glsl')
    open(xpath, 'w', encoding='utf-8').write(xlat)
    res = {'name': name, 'stage': stage, 'stats': stats, 'loose': stats['loose_uniform']}
    res['gl'] = validate_gl(gpath, stage)
    res['vk_raw'] = glslc(gpath, stage, 'vulkan1.3', ['-fauto-bind-uniforms', '-fauto-map-locations'], os.path.join(out_dir, name + '.raw.spv'))
    res['vk_xlat'] = glslc(xpath, stage, 'vulkan1.3', ['-fauto-bind-uniforms', '-fauto-map-locations'], os.path.join(out_dir, name + '.vk.spv'))
    return res


def main():
    dump_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    files = sorted(os.path.join(dump_dir, f) for f in os.listdir(dump_dir) if f.rsplit('.', 1)[-1] in STAGES)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = [r for r in ex.map(probe, [(f, out_dir) for f in files]) if r]

    n = len(results)
    gl_ok = [r for r in results if r['gl'][0]]
    print('shader objects dumped: %d (vertex %d, fragment %d, geometry %d)' % (
        n, sum(r['stage'] == 'vertex' for r in results), sum(r['stage'] == 'fragment' for r in results),
        sum(r['stage'] == 'geometry' for r in results)))
    print('control: compile for OpenGL target     : %4d / %d' % (len(gl_ok), n))
    print('Vulkan, source untouched               : %4d / %d' % (sum(r['vk_raw'][0] for r in results), n))
    print('Vulkan, after mechanical translation   : %4d / %d' % (sum(r['vk_xlat'][0] for r in results), n))
    both = [r for r in gl_ok]
    print('of those that compile for OpenGL: Vulkan after translation: %d / %d' % (sum(r['vk_xlat'][0] for r in both), len(both)))
    tot = collections.Counter()
    for r in results:
        tot.update(r['stats'])
    print('translation work: %s' % dict(tot))

    def group(key, only=None):
        g = collections.Counter()
        ex_names = {}
        for r in results:
            if only is not None and r not in only:
                continue
            ok, err = r[key]
            if not ok:
                k = first_error(err)
                g[k] += 1
                ex_names.setdefault(k, r['name'])
        return g, ex_names

    print('\ncontrol (OpenGL) failures, by first error:')
    g, ex = group('gl')
    for k, c in g.most_common(8):
        print('  %4d  %s   e.g. %s' % (c, k, ex[k]))
    print('\nVulkan failures after translation, among shaders that compile for OpenGL:')
    g, ex = group('vk_xlat', only=both)
    for k, c in g.most_common(25):
        print('  %4d  %s   e.g. %s' % (c, k, ex[k]))

    with open(os.path.join(out_dir, 'summary.tsv'), 'w') as f:
        f.write('name\tstage\tgl\tvk_raw\tvk_xlat\tfirst_vk_error\n')
        for r in results:
            f.write('%s\t%s\t%d\t%d\t%d\t%s\n' % (r['name'], r['stage'], r['gl'][0], r['vk_raw'][0], r['vk_xlat'][0],
                                                   '' if r['vk_xlat'][0] else first_error(r['vk_xlat'][1])))


if __name__ == '__main__':
    main()
