#!/usr/bin/env python3
"""host/profiles/mali-*.txt from players' vkbridge-server.log files (MaliBridge "Send logs" uploads).
    gen_mali_profiles.py <out dir> <log dir or .zip>...
Reads every vkbridge-server*.log (plain or inside a .zip) under the given paths and takes, per server
start on an Arm GPU (vendor 0x13b5), the self-test's "GPU:" line (name, Vulkan version), the driver
release (rNNpN, from the probe/errata lines), the "features:" lines (Vulkan 1.0 core features) and the
"  all:" lines (every device extension the driver lists). A start whose "all:" list is shorter than its
"N device extensions in total" line is skipped (a cut-off log).
Writes one file per driver family and mali-common.txt (what EVERY start has: the strictest profile):
    api 1.x.y            the lowest Vulkan version in the family
    ext VK_...           an extension every start in the family lists
    feature <name> 0|1   1 only when every start reports it
    # source ...         the log codes behind it (one line per GPU + driver)
host/mali_profile.sh turns a profile into the host server's switches."""
import os, re, sys, zipfile, collections

FAMILIES = [  # name, first and last driver release (rNN), what it is
    ("r18-r19", 18, 19, "Bifrost job-manager drivers (Mali-G52/G71/G76), Vulkan 1.1.97-1.1.108"),
    ("r21-r26", 21, 26, "older job-manager drivers (Mali-G57/G72/G78), Vulkan 1.1.117-1.1.131"),
    ("r32-r34", 32, 34, "r32p1-r34p0: Valhall job manager and the first CSF (G57/G77/G78/G610/G710), Vulkan 1.1"),
    ("r38-r40", 38, 40, "r38p0-r40p0: G57/G68/G72/G76/G78/G610 and the G715 on r38p1 (Vulkan 1.3 on most, 1.1 on r38p0 and one G610 r38p1)"),
    ("r44", 44, 44, "r44p1: CSF Mali-G615/G720 (Galaxy Tab S10, A-series), Vulkan 1.3.247"),
    ("r49", 49, 49, "r49p1: Mali-G925 (Tab S11), G720 MC7/MC8, G68 MC4, G52 MC2, Vulkan 1.3.278"),
    ("r54", 54, 54, "r54p1-r54p3: Mali-G1-Ultra (D9500), G715 (Pixel), G57/G78 on Android 16, Vulkan 1.3-1.4"),
    ("r56", 56, 56, "r56p0: Mali-G2-Ultra-NX (XRing O3), Vulkan 1.4"),
]

def parse(text, src):
    recs, cur = [], None
    for line in text.splitlines():
        m = re.search(r'vkbridge-server I GPU: (.+?) \(vendor (0x[0-9a-f]+) device (0x[0-9a-f]+), Vulkan ([\d.]+), driver (0x[0-9a-f]+)\)', line)
        if m:
            cur = dict(src=src, gpu=m.group(1), vendor=m.group(2), vk=m.group(4), all={}, feats={}, drv=None, total=None, in_feats=True)
            recs.append(cur)
            continue
        if cur is None:
            continue
        m = re.search(r'vkbridge-server I features: (.*)$', line)
        if m and cur['in_feats']:
            for kv in m.group(1).split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    cur['feats'][k] = int(v)
            continue
        m = re.search(r'vkbridge-server I   all: (.*)$', line)
        if m:
            cur['in_feats'] = False
            for kv in m.group(1).split():
                cur['all'][kv.rpartition(':')[0]] = 1
            continue
        m = re.search(r'I (\d+) device extensions in total', line)
        if m and cur['total'] is None:
            cur['total'] = int(m.group(1))
            continue
        if cur['drv'] is None:
            m = re.search(r'v1\.(r\d+p\d+)', line) or re.search(r'driver (r\d+p\d+)', line)
            if m:
                cur['drv'] = m.group(1)
    return recs

def code_of(src):
    m = re.search(r'(gh-logs-[^/]+)/([^/!]+)', src)
    return f"{m.group(2)} ({m.group(1)})" if m else os.path.basename(src)

def main():
    out, paths = sys.argv[1], sys.argv[2:]
    recs = []
    for root in paths:
        walk = [(os.path.dirname(root), [], [os.path.basename(root)])] if os.path.isfile(root) else os.walk(root)
        for dp, _, fn in walk:
            for f in fn:
                p = os.path.join(dp, f)
                if re.match(r'vkbridge-server.*\.log$', f):
                    recs += parse(open(p, errors='replace').read(), p)
                elif f.endswith('.zip'):
                    try:
                        z = zipfile.ZipFile(p)
                    except zipfile.BadZipFile:
                        continue
                    for n in z.namelist():
                        if re.search(r'vkbridge-server[^/]*\.log$', n):
                            recs += parse(z.read(n).decode(errors='replace'), p + '!' + n)
    fams = collections.defaultdict(list)
    skipped = collections.Counter()
    for r in recs:
        if r['vendor'] != '0x13b5':
            continue
        if not r['drv']:
            skipped['no driver release in the log'] += 1
            continue
        if not r['all'] or (r['total'] and len(r['all']) < r['total']) or len(r['feats']) < 30:
            skipped['cut-off extension or feature list'] += 1
            continue
        rel = int(re.match(r'r(\d+)', r['drv']).group(1))
        fam = next((f for f in FAMILIES if f[1] <= rel <= f[2]), None)
        if not fam:
            skipped[f'driver {r["drv"]} in no family'] += 1
            continue
        fams[fam[0]].append(r)
    for k, v in skipped.items():
        print(f"skipped {v} start(s): {k}", file=sys.stderr)
    def write(name, desc, rs):
        exts = set.intersection(*(set(r['all']) for r in rs))
        feats = sorted(set.union(*(set(r['feats']) for r in rs)))
        api = min((tuple(int(x) for x in r['vk'].split('.')) for r in rs))
        srcs = collections.defaultdict(set)
        for r in rs:
            srcs[(r['gpu'], r['drv'], r['vk'])].add(code_of(r['src']))
        with open(os.path.join(out, f"mali-{name}.txt"), 'w') as f:
            f.write(f"# Mali profile {name}: {desc}\n")
            f.write(f"# generated by gen_mali_profiles.py from {len(rs)} server start(s) in players' logs; do not edit by hand\n")
            for (g, d, vk), codes in sorted(srcs.items(), key=lambda kv: (kv[0][1], kv[0][0])):
                f.write(f"# source {g}, {d}, Vulkan {vk}: {', '.join(sorted(codes))}\n")
            f.write("api %d.%d.%d\n" % api)
            for e in sorted(exts):
                f.write(f"ext VK_{e}\n")
            for k in feats:
                f.write(f"feature {k} {int(all(r['feats'].get(k, 0) for r in rs))}\n")
        print(f"mali-{name}.txt: {len(rs)} starts, {len(srcs)} GPU+driver, {len(exts)} extensions, Vulkan {'.'.join(map(str, api))}")
    allr = []
    for name, lo, hi, desc in FAMILIES:
        if fams[name]:
            write(name, desc, fams[name])
            allr += fams[name]
    write("common", "what every Mali driver in the players' logs has (the strictest profile, the default of the host suite)", allr)

if __name__ == '__main__':
    main()
