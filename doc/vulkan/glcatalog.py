import re, sys, os, collections, json

ROOT = r"C:\phoenix-firestorm\indra"
MODS = ["llrender", "llwindow", "newview", "llui", "llappearance", "llcharacter", "llimage", "llprimitive", "llmath", "llcommon", "llfilesystem"]
CALL = re.compile(r"\b(gl[A-Z][A-Za-z0-9]+)\s*\(")

# category -> regex on function name (first match wins, order matters)
CATS = [
    ("legacy fixed-function (not in core GL)", r"^gl(MatrixMode|LoadMatrixf|LoadIdentity|PushMatrix|PopMatrix|PushAttrib|PopAttrib|PushClientAttrib|PopClientAttrib|TexGen\w+|AlphaFunc|Material\w+|Color4ubv|EnableClientState|DisableClientState|VertexPointer|TexCoordPointer|ClientActiveTexture|Hint|AreTexturesResident|PointToScreen|RectToScreen)$"),
    ("shader / program objects", r"^gl(CreateShader|ShaderSource|CompileShader|DeleteShader|IsShader|GetShader\w+|CreateProgram|AttachShader|DetachShader|LinkProgram|DeleteProgram|IsProgram|UseProgram|ValidateProgram|GetProgram\w+|ProgramParameteri|ProgramBinary|GetProgramBinary|GetAttachedShaders|BindAttribLocation|GetAttribLocation|GetActiveUniform)$"),
    ("uniforms / UBOs", r"^gl(Uniform\w*|GetUniformLocation|GetUniformBlockIndex|UniformBlockBinding|BindBufferBase|VertexAttrib4f|VertexAttrib4fv)$"),
    ("vertex buffers / VAO / draw", r"^gl(GenBuffers|DeleteBuffers|BindBuffer|BufferData|BufferSubData|GetBufferParameteriv|VertexAttribPointer|VertexAttribIPointer|EnableVertexAttribArray|DisableVertexAttribArray|GenVertexArrays|BindVertexArray|DrawElements|DrawRangeElements|DrawArrays)$"),
    ("textures / samplers / images", r"^gl(GenTextures|DeleteTextures|BindTexture|ActiveTexture|Tex(Image|SubImage|Parameter)\w*|CompressedTex\w+|GetCompressedTexImage|GetTexImage|GetTexLevelParameteriv|GenerateMipmap\w*|CopyTex\w+|SetSubImage2D|PixelStorei)$"),
    ("framebuffers / render targets", r"^gl(GenFramebuffers\w*|DeleteFramebuffers\w*|BindFramebuffer\w*|FramebufferTexture\w+|FramebufferRenderbuffer\w*|CheckFramebufferStatus\w*|GenRenderbuffers\w*|DeleteRenderbuffers\w*|BindRenderbuffer\w*|RenderbufferStorage\w*|IsRenderbuffer\w*|IsFramebuffer\w*|GetRenderbufferParameteriv\w*|GetFramebufferAttachmentParameteriv\w*|DrawBuffers?|ReadBuffer|ReadPixels|Clear|ClearColor|ClearStencil)$"),
    ("fixed pipeline state (blend/depth/cull/stencil/raster)", r"^gl(Enable|Disable|IsEnabled|BlendFunc\w*|DepthFunc|DepthMask|ColorMask|CullFace|PolygonMode|PolygonOffset|Stencil\w+|Scissor|Viewport|LineWidth|PointSize)$"),
    ("queries / sync / fences", r"^gl(GenQueries|DeleteQueries|BeginQuery|EndQuery|GetQueryObject\w+|FenceSync|DeleteSync|ClientWaitSync|WaitSync|Flush|Finish)$"),
    ("context / debug / introspection", r"^gl(GetError|GetString|GetStringi|GetIntegerv|GetFloatv|GetBooleanv|DebugMessage\w+|Ready|SwapBuffers)$"),
]
CATS = [(n, re.compile(r)) for n, r in CATS]

NOT_GL = {"glPointToScreen","glRectToScreen","glReady"}
EXTRA = {"glObjectLabel":"context / debug / introspection","glMapBuffer":"vertex buffers / VAO / draw","glUnmapBuffer":"vertex buffers / VAO / draw","glIsBuffer":"vertex buffers / VAO / draw","glGetBufferSubData":"vertex buffers / VAO / draw"}
def cat_of(fn):
    fn = re.sub(r"(ARB|EXT)$", "", fn)
    if fn in EXTRA:
        return EXTRA[fn]
    for n, rx in CATS:
        if rx.match(fn):
            return n
    return "UNCATEGORIZED"

def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)

per_file = collections.defaultdict(collections.Counter)  # file -> fn counts
fn_total = collections.Counter()
fn_files = collections.defaultdict(set)
wrapper_use = collections.Counter()  # newview file -> uses of abstraction layer
WRAP = re.compile(r"\b(gGL\.|LLGLSLShader|LLVertexBuffer|LLRenderTarget|LLGLEnable|LLGLDisable|LLGLState|LLGLDepthTest|LLImageGL|LLTexUnit)")

for mod in MODS:
    base = os.path.join(ROOT, mod)
    for dp, _, fs in os.walk(base):
        for f in fs:
            if not f.endswith((".cpp", ".h", ".mm", ".hpp", ".inl")):
                continue
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, ROOT).replace("\\", "/")
            try:
                src = strip_comments(open(p, encoding="utf-8", errors="replace").read())
            except OSError:
                continue
            for m in CALL.finditer(src):
                fn = m.group(1)
                if fn in NOT_GL:
                    continue
                per_file[rel][fn] += 1
                fn_total[fn] += 1
                fn_files[fn].add(rel)
            if mod == "newview":
                n = len(WRAP.findall(src))
                if n:
                    wrapper_use[rel] = n

# --- aggregate
cat_total = collections.Counter()
cat_fns = collections.defaultdict(collections.Counter)
for fn, n in fn_total.items():
    c = cat_of(fn)
    cat_total[c] += n
    cat_fns[c][fn] = n

mod_cat = collections.defaultdict(collections.Counter)
for rel, ctr in per_file.items():
    mod = rel.split("/")[0]
    for fn, n in ctr.items():
        mod_cat[mod][cat_of(fn)] += n

out = []
w = out.append
w("# OpenGL call catalog\n")
w("Generated from `indra/` by `glcatalog.py` (comments stripped, so commented-out calls are not counted).\n")
w(f"- Distinct GL functions: **{len(fn_total)}**")
w(f"- Total call sites: **{sum(fn_total.values())}** in **{len(per_file)}** files\n")
w("Counts are call sites in source, not runtime calls. Preprocessor-disabled blocks (`#if 0`, other platforms) are still counted, so treat as an upper bound.\n")

w("## 1. By category\n")
w("| Category | Call sites | Distinct fns | Vulkan equivalent |")
w("|---|---:|---:|---|")
VK = {
 "legacy fixed-function (not in core GL)": "None needed if dead; port or drop",
 "shader / program objects": "Pipeline + shader module creation (SPIR-V)",
 "uniforms / UBOs": "Descriptor sets, UBOs, push constants",
 "vertex buffers / VAO / draw": "VkBuffer, vertex input state, vkCmdDraw*",
 "textures / samplers / images": "VkImage, VkSampler, staging uploads",
 "framebuffers / render targets": "Render passes / dynamic rendering, attachments",
 "fixed pipeline state (blend/depth/cull/stencil/raster)": "Baked into pipeline objects (or dynamic state)",
 "queries / sync / fences": "Query pools, fences, semaphores, barriers",
 "context / debug / introspection": "Instance/device setup, validation layers",
 "UNCATEGORIZED": "?",
}
for c, n in cat_total.most_common():
    w(f"| {c} | {n} | {len(cat_fns[c])} | {VK.get(c,'')} |")

w("\n## 2. By module and category\n")
cats_order = [c for c, _ in cat_total.most_common()]
w("| Module | " + " | ".join(c.split(" (")[0] for c in cats_order) + " | Total |")
w("|---|" + "---:|" * (len(cats_order) + 1))
for mod in MODS:
    if mod not in mod_cat:
        continue
    row = [str(mod_cat[mod].get(c, 0)) for c in cats_order]
    w(f"| {mod} | " + " | ".join(row) + f" | {sum(mod_cat[mod].values())} |")

w("\n## 3. Functions per category\n")
for c, _ in cat_total.most_common():
    w(f"### {c}\n")
    w(", ".join(f"`{fn}` ×{n}" for fn, n in cat_fns[c].most_common()) + "\n")

w("## 4. Files that call GL directly, outside `llrender/` (the migration list)\n")
w("These bypass the abstraction layer and must be routed through it before a second backend is possible.\n")
w("| File | GL calls | Top categories |")
w("|---|---:|---|")
rows = []
for rel, ctr in per_file.items():
    if rel.startswith("llrender/"):
        continue
    cc = collections.Counter()
    for fn, n in ctr.items():
        cc[cat_of(fn).split(" (")[0]] += n
    rows.append((sum(ctr.values()), rel, cc))
for tot, rel, cc in sorted(rows, reverse=True):
    top = ", ".join(f"{k} {v}" for k, v in cc.most_common(3))
    w(f"| `{rel}` | {tot} | {top} |")

w("\n## 5. Files inside `llrender/` (the layer to put the backend interface under)\n")
w("| File | GL calls | Top categories |")
w("|---|---:|---|")
rows = []
for rel, ctr in per_file.items():
    if not rel.startswith("llrender/"):
        continue
    cc = collections.Counter()
    for fn, n in ctr.items():
        cc[cat_of(fn).split(" (")[0]] += n
    rows.append((sum(ctr.values()), rel, cc))
for tot, rel, cc in sorted(rows, reverse=True):
    top = ", ".join(f"{k} {v}" for k, v in cc.most_common(3))
    w(f"| `{rel}` | {tot} | {top} |")

w("\n## 6. newview files that use the wrapper layer but make no direct GL calls\n")
clean = [(n, r) for r, n in wrapper_use.items() if r not in per_file]
w(f"**{len(clean)}** newview files already render only through `gGL` / `LLVertexBuffer` / `LLGLSLShader` / `LLRenderTarget` etc. These need no migration, only the backend underneath them to change. Top 15 by wrapper use:\n")
for n, r in sorted(clean, reverse=True)[:15]:
    w(f"- `{r}` ({n} wrapper references)")

dead_lines = open(os.path.join(ROOT, "newview", "lldrawpoolterrain.cpp"), encoding="utf-8", errors="replace").read().split("\n")
# renderFull4TU, renderFull2TU, renderSimple (line 659 on): declared in lldrawpoolterrain.h, never called
dead_src = strip_comments("\n".join(dead_lines[658:]))
dead_n = len(CALL.findall(dead_src))
w("\n## 6b. Dead code to delete instead of port\n")
w(f"`newview/lldrawpoolterrain.cpp` `renderFull4TU()`, `renderFull2TU()` and `renderSimple()` (from line 659) are declared in `lldrawpoolterrain.h` and have no callers anywhere in `indra/`. They hold **{dead_n}** GL call sites, mostly fixed-function (`glTexGen*`, `glMatrixMode`, ...). Delete them first; this removes most of the legacy-fixed-function category from the migration list.\n")
unc = cat_fns.get("UNCATEGORIZED")
if unc:
    w("\n## 7. Uncategorized (extend CATS)\n")
    w(", ".join(f"`{fn}` ×{n}" for fn, n in unc.most_common()))

text = "\n".join(out) + "\n"
dest = sys.argv[1]
os.makedirs(os.path.dirname(dest), exist_ok=True)
open(dest, "w", encoding="utf-8", newline="\n").write(text)
print(f"wrote {dest}: {len(text)} bytes, uncategorized={len(unc) if unc else 0}")
