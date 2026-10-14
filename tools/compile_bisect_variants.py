#!/usr/bin/env python3
"""Rewritten variants of a fragment shader a driver never finishes compiling, for bisecting what
the compiler chokes on with tests/compile_probe.c on the device.

  tools/compile_bisect_variants.py <fs.spv> <out-dir> [--docker IMAGE]

Needs spirv-dis / spirv-as / spirv-opt / spirv-val (from PATH, or `docker run IMAGE` with
the tools, e.g. vkb-arch-gamescope). Variants marked (bisect) change the shader's output and only
locate the trigger; the others keep it. Every variant is validated; invalid ones are dropped.
"""
import os, re, subprocess, sys, shutil, tempfile

args = sys.argv[1:]
docker = None
if '--docker' in args:
    i = args.index('--docker'); docker = args[i + 1]; del args[i:i + 2]
src, out = os.path.abspath(args[0]), os.path.abspath(args[1])
os.makedirs(out, exist_ok=True)
work = tempfile.mkdtemp()

def tool(name, *a):
    if docker:
        cmd = ['docker', 'run', '--rm', '-v', f'{work}:{work}', '-v', f'{out}:{out}', '-v', f'{os.path.dirname(src)}:{os.path.dirname(src)}:ro',
               docker, name] + list(a)
    else:
        cmd = [name] + list(a)
    return subprocess.run(cmd, capture_output=True, text=True)

def dis(path):
    r = tool('spirv-dis', '--raw-id', path)
    if r.returncode: raise SystemExit(r.stderr)
    return r.stdout

def assemble(text, name, note):
    t = os.path.join(work, name + '.spvasm'); o = os.path.join(out, name + '.spv')
    open(t, 'w').write(text)
    r = tool('spirv-as', '--target-env', 'spv1.6', t, '-o', o)
    if r.returncode: print(f'{name}: assemble failed: {r.stderr.strip()[:200]}'); return None
    v = tool('spirv-val', '--target-env', 'vulkan1.3', o)
    if v.returncode: print(f'{name}: invalid, dropped: {v.stderr.strip()[:200]}'); os.unlink(o); return None
    print(f'{name}: {note}')
    return o

def bound(text):
    return int(re.search(r'; Bound: (\d+)', text).group(1))

def set_bound(text, b):
    return re.sub(r'; Bound: \d+', f'; Bound: {b}', text)

orig = dis(src)
assemble(orig, '00-orig', 'unchanged')

# 01: SPV_KHR_float_controls2 out (what the bridge's fc2 fallback does)
t = orig
t = re.sub(r'^\s*OpCapability FloatControls2\n', '', t, flags=re.M)
t = re.sub(r'^\s*OpExtension "SPV_KHR_float_controls2"\n', '', t, flags=re.M)
t = re.sub(r'^\s*OpExecutionModeId \S+ FPFastMathDefault .*\n', '', t, flags=re.M)
t = re.sub(r'^(\s*)OpDecorate (\S+) FPFastMathMode None$', r'\1OpDecorate \2 NoContraction', t, flags=re.M)
t = re.sub(r'^\s*OpDecorate \S+ FPFastMathMode .*\n', '', t, flags=re.M)
base = assemble(t, '01-nofc2', 'float_controls2 stripped')
basetext = t

opt = [
    ('02-O', ['-O'], 'spirv-opt -O'),
    ('03-Os', ['-Os'], 'spirv-opt -Os'),
    ('04-inline', ['--inline-entry-points-exhaustive', '--eliminate-dead-functions'], 'helper functions inlined'),
    ('05-merge', ['--inline-entry-points-exhaustive', '--merge-return', '--merge-blocks', '--eliminate-dead-functions'], 'inlined, blocks merged'),
    ('06-ifconv', ['--inline-entry-points-exhaustive', '--eliminate-dead-functions', '--if-conversion', '--simplify-instructions', '--eliminate-dead-code-aggressive'], 'branches if-converted to selects'),
    ('07-relaxprec', ['--relax-float-ops', '--convert-relaxed-to-half'], 'mediump (16-bit) float math (precision changes)'),
    ('08-ssa', ['--inline-entry-points-exhaustive', '--ccp', '--simplify-instructions', '--redundancy-elimination', '--eliminate-dead-code-aggressive'], 'constant propagation + CSE'),
]
if base:
    for name, passes, note in opt:
        o = os.path.join(out, name + '.spv')
        r = tool('spirv-opt', '--target-env=vulkan1.3', *passes, base, '-o', o)
        if r.returncode: print(f'{name}: spirv-opt failed: {r.stderr.strip()[:200]}'); continue
        v = tool('spirv-val', '--target-env', 'vulkan1.3', o)
        if v.returncode: print(f'{name}: invalid, dropped'); os.unlink(o); continue
        print(f'{name}: {note}')

# 09: every GLSL.std.450 Fma as OpFMul + OpFAdd
b = bound(basetext); lines = []
ext = re.search(r'^\s*%(\d+) = OpExtInstImport "GLSL.std.450"', basetext, re.M).group(1)
for line in basetext.split('\n'):
    m = re.match(rf'^(\s*)%(\d+) = OpExtInst %(\d+) %{ext} Fma %(\d+) %(\d+) %(\d+)$', line)
    if m:
        ind, r, ty, a, bb, c = m.groups()
        lines.append(f'{ind}%{b} = OpFMul %{ty} %{a} %{bb}'); lines.append(f'{ind}%{r} = OpFAdd %{ty} %{b} %{c}'); b += 1
    else:
        lines.append(line)
assemble(set_bound('\n'.join(lines), b), '09-fmasplit', 'Fma as multiply + add')

# 10: no NoContraction decorations
assemble(re.sub(r'^\s*OpDecorate \S+ NoContraction\n', '', basetext, flags=re.M), '10-nocontraction-off', 'NoContraction decorations dropped')

# 11 (bisect): dynamic indexing of the immediate constant buffer replaced by element 0
t = basetext
icb = re.search(r'OpName %(\d+) "icb"', orig)
if icb:
    zero = re.search(r'^\s*%(\d+) = OpConstant %\d+ 0$', t, re.M)
    if zero:
        t2 = re.sub(rf'(OpAccessChain %\d+ %{icb.group(1)} %\d+) %\d+$', rf'\1 %{zero.group(1)}', t, flags=re.M)
        assemble(t2, '11-bisect-icbconst', '(bisect) constant-buffer array index made constant')

# 12 (bisect): Exp2/Log2 replaced by their argument
t = re.sub(rf'^(\s*%\d+) = OpExtInst (%\d+) %{ext} (Exp2|Log2) (%\d+)$', r'\1 = OpCopyObject \2 \4', basetext, flags=re.M)
assemble(t, '12-bisect-noexplog', '(bisect) Exp2/Log2 removed')

# 13 (bisect): every conditional branch takes its true side
t = re.sub(r'^(\s*)OpBranchConditional %\d+ (%\d+) (%\d+)$', r'\1OpBranchConditional %TRUE \2 \3', basetext, flags=re.M)
if '%TRUE' in t:
    booltype = re.search(r'^\s*%(\d+) = OpTypeBool$', t, re.M).group(1)
    b = bound(t)
    t = t.replace('%TRUE', f'%{b}')
    t = re.sub(rf'^(\s*%{booltype} = OpTypeBool)$', rf'\1\n%{b} = OpConstantTrue %{booltype}', t, flags=re.M)
    assemble(set_bound(t, b + 1), '13-bisect-truebranches', '(bisect) every branch condition constant true')
shutil.rmtree(work)
