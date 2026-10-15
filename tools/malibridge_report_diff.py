#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compares two MaliBridge session reports (malibridge-report.json) or two startup-probe device
profiles (<cache dir>/probe-*.json): S10+ against S11 Ultra, or a build before and after a change.

    tools/malibridge_report_diff.py A.json B.json [--regressions]

Prints what differs - device and driver, probe results (with their failed sub-cases) and the
decisions they caused, each driver erratum's decision in either mode, issues only in one of them,
and per game (matched by Steam key or process name): frame rate, compile time, device losses, long
GPU waits, memory, log classes only in one. Numbers inside messages (counts,
times, addresses) are ignored when matching issues and log lines. --regressions: exit 1 when B has
an issue A does not, or a probe that passed in A does not pass in B.
"""
import json
import re
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def norm(text):
    """A message without its numbers, so the same problem matches across runs."""
    text = re.sub(r"0x[0-9a-fA-F]+", "#", text)
    text = re.sub(r"\d+(\.\d+)?", "#", text)
    return re.sub(r"\s+", " ", text).strip()


def probe_results(profile):
    return {r["name"]: r for r in profile.get("results", [])}


def diff_probes(a, b, out, label=""):
    """a, b: probe profiles (the file's or one embedded in a report). Returns regressions."""
    regress = 0
    if a.get("gpu") != b.get("gpu") or a.get("driver") != b.get("driver"):
        out.append(f"{label}GPU/driver: {a.get('gpu')} [{a.get('driver')}]  ->  {b.get('gpu')} [{b.get('driver')}]")
    for k in ("probe_version", "emu_before", "emu_after", "mode"):
        if a.get(k) != b.get(k):
            out.append(f"{label}{k}: {a.get(k)} -> {b.get(k)}")
    ra, rb = probe_results(a), probe_results(b)
    for name in list(ra) + [n for n in rb if n not in ra]:
        x, y = ra.get(name), rb.get(name)
        if not x or not y:
            out.append(f"{label}probe {name}: only in {'A' if x else 'B'} ({(x or y)['status']})")
            continue
        changes = []
        if x["status"] != y["status"]:
            changes.append(f"{x['status']} -> {y['status']}")
            if x["status"] == "pass" and y["status"] not in ("pass", "skip"):
                regress += 1
        for k in ("emu", "flags", "failed"):
            if x.get(k) != y.get(k):
                changes.append(f"{k} {x.get(k)} -> {y.get(k)}")
        mx, my = x.get("ms", 0), y.get("ms", 0)
        if max(mx, my) > 50 and (min(mx, my) == 0 or max(mx, my) / max(min(mx, my), 0.1) > 2):
            changes.append(f"{mx:.0f} ms -> {my:.0f} ms")
        if changes:
            out.append(f"{label}probe {name}: " + ", ".join(changes))
            if x["detail"] != y["detail"]:
                out.append(f"    A: {x['detail']}")
                out.append(f"    B: {y['detail']}")
    # the driver errata a report shows per profile: each one's decision in either mode
    ea = {e["name"]: e for e in a.get("errata", [])}
    eb = {e["name"]: e for e in b.get("errata", [])}
    for name in list(ea) + [n for n in eb if n not in ea]:
        x, y = ea.get(name), eb.get(name)
        if not x or not y:
            out.append(f"{label}erratum {name}: only in {'A' if x else 'B'}")
            continue
        for m in ("safe", "performance"):
            if x[m]["on"] != y[m]["on"]:
                out.append(f"{label}erratum {name}, {m} mode: {'on' if x[m]['on'] else 'off'} -> {'on' if y[m]['on'] else 'off'}"
                           f" ({y[m]['why']})")
    return regress


def proc_key(p):
    return p.get("key") or p.get("comm") or p.get("name")


def num(p, k):
    v = p.get(k)
    return v if isinstance(v, (int, float)) else 0


def diff_reports(a, b, out):
    regress = 0
    for k in ("model", "soc", "android", "kernel", "mem_total_mb"):
        if a["device"].get(k) != b["device"].get(k):
            out.append(f"device {k}: {a['device'].get(k)} -> {b['device'].get(k)}")
    for k in ("build", "protocol"):
        if a["bridge"].get(k) != b["bridge"].get(k):
            out.append(f"bridge {k}: {a['bridge'].get(k)} -> {b['bridge'].get(k)}")
    if a["bridge"].get("env_overrides") != b["bridge"].get("env_overrides"):
        out.append(f"env overrides: '{a['bridge'].get('env_overrides')}' -> '{b['bridge'].get('env_overrides')}'")
    ga, gb = a.get("gpus", []), b.get("gpus", [])
    for i in range(max(len(ga), len(gb))):
        x = ga[i] if i < len(ga) else {}
        y = gb[i] if i < len(gb) else {}
        for k in ("name", "api", "memory_sharing", "missing", "emulating"):
            if x.get(k) != y.get(k):
                out.append(f"GPU {i} {k}: {x.get(k)} -> {y.get(k)}")
    pa, pb = a.get("probe", []), b.get("probe", [])
    for i in range(min(len(pa), len(pb))):
        regress += diff_probes(pa[i], pb[i], out, f"[GPU {i}] " if max(len(pa), len(pb)) > 1 else "")
    if len(pa) != len(pb):
        out.append(f"probe profiles: {len(pa)} -> {len(pb)}")
    ia = {norm(t): t for t in a.get("issues", [])}
    ib = {norm(t): t for t in b.get("issues", [])}
    out.append(f"issues: {a.get('issue_count', len(ia))} -> {b.get('issue_count', len(ib))}")
    for k, t in ia.items():
        if k not in ib:
            out.append(f"  only in A: {t}")
    for k, t in ib.items():
        if k not in ia:
            out.append(f"  only in B: {t}")
            regress += 1
    procs_a = {proc_key(p): p for p in a.get("processes", [])}
    procs_b = {proc_key(p): p for p in b.get("processes", [])}
    for key in list(procs_a) + [k for k in procs_b if k not in procs_a]:
        x, y = procs_a.get(key), procs_b.get(key)
        if not x or not y:
            out.append(f"process {key}: only in {'A' if x else 'B'}")
            continue
        lines = []
        for k, fmt in (("fps_p50", "{:.1f}"), ("t_first_frame", "{:.1f} s"), ("compiles", "{}"), ("compile_ms", "{:.0f} ms"),
                       ("compile_max_ms", "{:.0f} ms"), ("compile_failed", "{}"), ("device_lost", "{}"), ("long_waits", "{}"),
                       ("gpu_mem_peak_mb", "{} MB"), ("mem_available_min_mb", "{} MB"), ("cpu_cap_min_pct", "{}%"), ("thermal_max", "{}")):
            vx, vy = num(x, k), num(y, k)
            if vx == vy:
                continue
            big = abs(vx - vy) > 0.1 * max(abs(vx), abs(vy), 1)
            if k in ("compile_failed", "device_lost", "long_waits", "thermal_max") or big:
                lines.append(f"{k} {fmt.format(vx)} -> {fmt.format(vy)}")
        if x.get("engine") != y.get("engine"):
            lines.append(f"engine {x.get('engine')} -> {y.get('engine')}")
        la = {norm(c["text"]): c for c in x.get("log", []) if c["level"] != "info"}
        lb = {norm(c["text"]): c for c in y.get("log", []) if c["level"] != "info"}
        only_a = [c for k, c in la.items() if k not in lb]
        only_b = [c for k, c in lb.items() if k not in la]
        if lines or only_a or only_b:
            out.append(f"process {key}: " + ("; ".join(lines) if lines else "log differs"))
            for c in only_a:
                out.append(f"    only in A ({c['level']} x{c['count']}): {c['text']}")
            for c in only_b:
                out.append(f"    only in B ({c['level']} x{c['count']}): {c['text']}")
    return regress


def main(argv):
    args = [x for x in argv[1:] if not x.startswith("--")]
    if len(args) != 2:
        print(__doc__.strip())
        return 2
    a, b = load(args[0]), load(args[1])
    out = [f"A: {args[0]}", f"B: {args[1]}"]
    if "malibridge_report" in a and "malibridge_report" in b:
        regress = diff_reports(a, b, out)
    elif "malibridge_probe" in a and "malibridge_probe" in b:
        regress = diff_probes(a, b, out)
    else:
        print("both files must be session reports (malibridge-report.json) or both probe profiles (probe-*.json)")
        return 2
    if len(out) == 2:
        out.append("no differences")
    print("\n".join(out))
    if "--regressions" in argv and regress:
        print(f"{regress} regression(s)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
