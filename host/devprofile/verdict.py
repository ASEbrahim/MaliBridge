#!/usr/bin/env python3
"""The verdict of one devprofile_check.sh run directory, or a markdown summary of all of them.
    verdict.py <run dir>            text verdict (also written as <run dir>/verdict.json)
    verdict.py --summary <out dir>  markdown table over every run dir in it
"""
import glob, json, os, re, sys

DXVK_ORDER = ["1.10.3", "2.6.2", "2.7.1", "proton"]


def read(p):
    try:
        with open(p, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def caps(profile_json):
    """(properties, features, extensions) of the first profile's device capabilities"""
    try:
        d = json.loads(profile_json)
    except ValueError:
        return {}, {}, {}
    c = d.get("capabilities") or {}
    merged_p, merged_f, merged_e = {}, {}, {}
    for v in c.values():
        merged_p.update(v.get("properties", {}))
        merged_f.update(v.get("features", {}))
        merged_e.update(v.get("extensions", {}))
    return merged_p, merged_f, merged_e


def api_str(v):
    if isinstance(v, str): return v
    if not isinstance(v, int): return "?"
    return f"{(v >> 22) & 0x7f}.{(v >> 12) & 0x3ff}.{v & 0xfff}"


def feat(f, *names):
    for s in f.values():
        for n in names:
            if n in s: return bool(s[n])
    return None


def droiddeck_pick(api, int64, push):
    """ComponentsManager.bridgeDxvk (DroidDeck rc/mali-v0.5.2)"""
    try:
        ma, mi = (int(x) for x in api.split(".")[:2])
    except ValueError:
        return None
    if (ma, mi) < (1, 1): return None
    if (ma, mi) < (1, 3): return "1.10.3"
    if int64 is False or (push if push is not None else 256) < 256: return "2.6.2"
    return "proton"


def dxvk_result(d):
    logs = "".join(read(p) for p in sorted(glob.glob(os.path.join(d, "*.log"))) if not p.endswith("check.log"))
    out = read(os.path.join(d, "check.log"))
    version = (re.search(r"DXVK: (v\S+)", logs) or [None, "?"])[1]
    skips = []
    for m in re.finditer(r"(Skipping[^\n]*)", logs):
        s = m.group(1).strip()
        if s not in skips: skips.append(s)
    created = bool(re.search(r"^PASS D3D11 device", out, re.M))
    m = re.search(r"^(OK|FAILED): (\d+) of (\d+) checks passed", out, re.M)
    fl = (re.search(r"Using feature level (D3D_FEATURE_LEVEL_\w+)", logs) or [None, None])[1]
    fails = [l[5:] for l in out.splitlines() if l.startswith("FAIL ")]
    errs = []
    for l in logs.splitlines():
        if l.startswith("err:") and l not in errs: errs.append(l[5:].strip())
    return {"dxvk": version, "accepted": created, "skips": skips, "feature_level": fl,
            "checks": f"{m.group(2)}/{m.group(3)}" if m else None, "check_fails": fails[:5], "errors": errs[:4]}


def verdict(run):
    r = {"name": os.path.basename(run.rstrip("/"))}
    prof_files = glob.glob(os.path.join(run, "profile", "*.json"))
    pp, pf, pe = caps(read(prof_files[0])) if prof_files else ({}, {}, {})
    props = pp.get("VkPhysicalDeviceProperties", {})
    drv = pp.get("VkPhysicalDeviceDriverPropertiesKHR") or pp.get("VkPhysicalDeviceDriverProperties") or pp.get("VkPhysicalDeviceVulkan12Properties") or {}
    r["gpu"] = props.get("deviceName", "?")
    r["driver_info"] = drv.get("driverInfo", "?")
    r["driver_api"] = api_str(props.get("apiVersion"))
    r["memory_imitation"] = read(os.path.join(run, "memory-imitation.txt")).strip() or "?"
    try:
        heaps = json.loads(read(prof_files[0])).get("x-malibridge", {}).get("memory", {}).get("memoryHeaps") or []
        r["heap0_mb"] = heaps[0]["size"] >> 20 if heaps else None
    except (ValueError, IndexError, KeyError):
        r["heap0_mb"] = None
    log = read(os.path.join(run, "server.log"))
    m = re.search(r"GPU: (.*?) \(vendor (0x[0-9a-f]+)", log)
    r["server_gpu"] = m.group(1) if m else None
    r["identity_ok"] = bool(m) and m.group(1) == r["gpu"] and m.group(2) == "0x13b5"
    r["listening"] = "listening on" in log
    smoke = read(os.path.join(run, "smoke.log"))
    m = re.search(r"^(OK|FAILED): (\d+) of (\d+) checks passed", smoke, re.M)
    r["smoke"] = f"{m.group(2)}/{m.group(3)}" if m else None
    r["smoke_fails"] = [l[5:] for l in smoke.splitlines() if l.startswith("FAIL ")][:6]
    mc = read(os.path.join(run, "memclass.log"))
    r["memclass"] = None if not mc else (mc.strip().splitlines() or ["?"])[-2 if mc.strip().splitlines()[-1].startswith("exit") and len(mc.strip().splitlines()) > 1 else -1]
    r["memclass_ok"] = None if not mc else "exit 0" in mc and "gets no client type" not in mc
    r["memclass_fails"] = [l.strip() for l in mc.splitlines() if "gets no client type" in l or l.startswith("FAIL")][:4]
    r["bridge_starts"] = r["listening"] and bool(m) and m.group(1) == "OK" and r["memclass_ok"] is not False
    errs = []
    for l in log.splitlines():
        mm = re.match(r"\S+ vkbridge-server E (.*)", l)
        if mm and not mm.group(1).startswith("  requested") and mm.group(1) not in errs: errs.append(mm.group(1))
    r["server_errors"] = errs[:8]
    m = re.search(r": memory sharing = (\S+), missing (0x[0-9a-f]+), emulating (0x[0-9a-f]+)", log)
    r["missing"], r["emulating"] = (m.group(2), m.group(3)) if m else (None, None)
    adj = []
    pats = [r"robustness2: the driver.s has .*", r"errata: fp16_hide: shaderFloat16 and the 16-bit float controls hidden.*",
            r"errata: device \d+, .*", r"promotion: .*", r"Vulkan promotion.*", r"promot(ed|ing) .*",
            r"vertex attribute divisor: .*", r"BC emulation: (transcoding|decoding|formats).*", r"probe: \S+\s+(fail|hang|crash)\S*.*"]
    for l in log.splitlines():
        body = l.split(" vkbridge-server ", 1)[-1][2:]
        for p in pats:
            if re.match(p, body) and body not in adj and not (body.startswith("errata: device") and any(a.startswith("errata: device") for a in adj)):
                adj.append(body)
    r["adjustments"] = adj[:14]
    clamp = sorted({l.split("devclamp: ", 1)[1].split(" (the desktop")[0] for l in read(os.path.join(run, "devclamp.log")).splitlines()
                    if "taken off (the desktop" in l})
    r["host_gaps"] = clamp
    # what the bridge presents (vulkaninfo through the client ICD)
    cp, cf, ce = caps(read(os.path.join(run, "presented.json")))
    cprops = cp.get("VkPhysicalDeviceProperties", {})
    r["presented_api"] = api_str(cprops.get("apiVersion")) if cprops else None
    lim = cprops.get("limits", {})
    av = read(os.path.join(run, "cache", "apps-vulkan"))
    r["apps_vulkan"] = av.strip().replace("\n", "; ") if av else None
    r["presented"] = {
        "shaderInt64": feat(cf, "shaderInt64"),
        "robustBufferAccess2": feat(cf, "robustBufferAccess2"),
        "robustImageAccess2": feat(cf, "robustImageAccess2"),
        "nullDescriptor": feat(cf, "nullDescriptor"),
        "storageBuffer16BitAccess": feat(cf, "storageBuffer16BitAccess"),
        "shaderFloat16": feat(cf, "shaderFloat16"),
        "textureCompressionBC": feat(cf, "textureCompressionBC"),
        "maxPushConstantsSize": lim.get("maxPushConstantsSize"),
        "extensions": len(ce),
    } if cprops else None
    if r["presented"]:
        r["droiddeck_pick"] = droiddeck_pick(r["presented_api"] or "0.0", r["presented"]["shaderInt64"], r["presented"]["maxPushConstantsSize"])
    else:
        r["droiddeck_pick"] = None
    z = read(os.path.join(run, "zink.log"))
    m = re.search(r"OpenGL core profile renderer: (.*)", z)
    ver = re.search(r"OpenGL core profile version: (\S+)", z)
    es = re.search(r"OpenGL ES profile version: OpenGL ES (\S+)", z)
    warn = re.search(r"doesn't support base Zink requirements: (.*)", z)
    if not z: r["zink"] = None
    elif m and m.group(1).startswith("zink"):
        r["zink"] = {"ok": True, "gl": ver.group(1) if ver else "?", "es": es.group(1) if es else "?", "missing": warn.group(1).strip() if warn else ""}
    else:
        r["zink"] = {"ok": False, "renderer": m.group(1) if m else None, "missing": warn.group(1).strip() if warn else ""}
    g = read(os.path.join(run, "gamescope.log"))
    if not g: r["gamescope"] = None
    else:
        sel = re.search(r"selecting physical device '([^']*)'", g)
        fps = re.findall(r"frames in [\d.]+ seconds = ([\d.]+) FPS", g)
        rend = re.search(r"GL_RENDERER\s*=\s*(.*)", g)
        err = [re.sub(r"\x1b\[[0-9;]*m", "", l).strip() for l in g.splitlines()
               if re.search(r"Error|Assertion|failed to find|Aborted|failed to load driver", l) and "pipewire" not in l and "pw." not in l]
        r["gamescope"] = {"device": sel.group(1) if sel else None, "fps": fps[-1] if fps else None,
                          "renderer": rend.group(1).strip() if rend else None, "errors": err[:4]}
    r["dxvk"] = {}
    for d in sorted(glob.glob(os.path.join(run, "dxvk-*"))):
        r["dxvk"][os.path.basename(d)[5:]] = dxvk_result(d)
    return r


def text(r):
    L = [f"{r['name']}: {r['gpu']} ({r['driver_info']}), driver Vulkan {r['driver_api']}"]
    if not r["identity_ok"]:
        L.append(f"  WARNING: the server saw {r['server_gpu']!r}, not the profile's GPU (identity not imitated)")
    L.append(f"  bridge starts: {'YES' if r['bridge_starts'] else 'NO'} (server {'listening' if r['listening'] else 'did not listen'}, vkbridge-smoke {r['smoke'] or 'not run'}, memclass_check {'ok' if r['memclass_ok'] else 'not run' if r['memclass_ok'] is None else 'FAILED'})")
    for f in r["smoke_fails"]: L.append(f"    smoke FAIL {f}")
    if r["memclass"] is not None: L.append(f"    memclass_check: {r['memclass']}")
    for f in r["memclass_fails"]: L.append(f"    memclass {f}")
    for e in r["server_errors"]: L.append(f"    server E {e}")
    L.append(f"  presented: Vulkan {r['presented_api']}, {r['presented']}")
    if r["apps_vulkan"]: L.append(f"  apps-vulkan: {r['apps_vulkan']}")
    L.append(f"  missing {r['missing']}, emulating {r['emulating']}; memory table imitated: {r['memory_imitation']}, heap 0 {r['heap0_mb']} MB")
    for a in r["adjustments"]: L.append(f"    {a}")
    if r["host_gaps"]: L.append(f"  not imitated (the desktop driver lacks them, taken off at vkCreateDevice): {', '.join(r['host_gaps'])}")
    z = r.get("zink")
    if z:
        L.append(f"  Zink (Steam's UI): " + (f"yes, GL {z['gl']}, GLES {z['es']}" if z["ok"] else f"NO (renderer {z['renderer']})")
                 + (f"; lacks Zink's base requirements: {z['missing']}" if z["missing"] else ""))
    g = r.get("gamescope")
    if g:
        ok = g["device"] and g["fps"] and (g["renderer"] or "").startswith("zink")
        L.append(f"  gamescope + Zink glxgears: " + ("yes, " + g["fps"] + " fps" if ok else "NO") +
                 f" (device {g['device']!r}, renderer {g['renderer']!r})")
        for e in g["errors"]: L.append(f"    {e}")
    for v, x in r["dxvk"].items():
        pick = " <- DroidDeck's pick" if v == r["droiddeck_pick"] else ""
        if x["accepted"]:
            L.append(f"  DXVK {v} ({x['dxvk']}): ACCEPTED, {x['feature_level']}, d3d11_check {x['checks']}{pick}")
            for f in x["check_fails"]: L.append(f"    FAIL {f}")
        else:
            why = "; ".join(x["skips"]) or "; ".join(x["errors"]) or "?"
            L.append(f"  DXVK {v} ({x['dxvk']}): REFUSED: {why}{pick}")
    return "\n".join(L)


def cell(x):
    if not x: return "-"
    if x["accepted"]:
        return "yes" + ("" if not x["checks"] or x["checks"].split("/")[0] == x["checks"].split("/")[1] else f" (checks {x['checks']})")
    why = "; ".join(s.replace("Skipping: ", "").replace("Skipping ", "") for s in x["skips"]) or (x["errors"][0] if x["errors"] else "?")
    return "**no**: " + why


def zcell(z):
    if not z: return "-"
    if z["ok"]: return f"GL {z['gl']} / ES {z['es']}"
    return f"**no**: {(z['renderer'] or '?').split(' (')[0]}"


def gcell(g):
    if not g: return "-"
    if g["device"] and g["fps"] and (g["renderer"] or "").startswith("zink"): return f"yes ({float(g['fps']):.0f} fps)"
    if not g["device"]: return "**no**: " + (g["errors"][0] if g["errors"] else "no device")
    return "**gamescope yes, Zink no**" + (f": {g['errors'][0]}" if g["errors"] else "")


def summary(out):
    rows = []
    for vf in sorted(glob.glob(os.path.join(out, "*", "verdict.json"))):
        rows.append(json.load(open(vf)))
    vs = [v for v in DXVK_ORDER if any(v in r["dxvk"] for r in rows)]
    H = ["device", "GPU, driver", "Vulkan driver -> presented", "bridge starts", "Zink (Steam UI)", "gamescope + Zink"] + [f"DXVK {v if v != 'proton' else 'Proton'}" for v in vs] + ["DroidDeck picks", "not imitated on the host"]
    L = ["| " + " | ".join(H) + " |", "|" + "---|" * len(H)]
    for r in rows:
        drv = re.search(r"r\d+p\d+", r["driver_info"] or "")
        why = r["smoke_fails"][:1] or r["memclass_fails"][:1] or r["server_errors"][:1]
        start = "yes" if r["bridge_starts"] else "**no**" + (f": {why[0]}" if why else "")
        if not r["identity_ok"]: start += " (identity NOT imitated)"
        L.append("| " + " | ".join([r["name"], f"{r['gpu']} {drv.group(0) if drv else ''}", f"{r['driver_api']} -> {r['presented_api']}", start, zcell(r.get("zink")), gcell(r.get("gamescope"))]
                                  + [cell(r["dxvk"].get(v)) for v in vs]
                                  + [r["droiddeck_pick"] or "-", ", ".join(g.split(" ", 1)[1] for g in r["host_gaps"]) or "-"]) + " |")
    return "\n".join(L) + "\n"


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--summary":
        sys.stdout.write(summary(sys.argv[2]))
        return
    run = sys.argv[1]
    r = verdict(run)
    json.dump(r, open(os.path.join(run, "verdict.json"), "w"), indent=1)
    print(text(r))


if __name__ == "__main__":
    main()
