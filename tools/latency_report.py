#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Latency per stage, median and p90, from the latency logs (VKBRIDGE_LATLOG).

    tools/latency_report.py [--presses] [--json] [--comm NAME] LOG...

Give it any of these, in any order (the kind is told from each file's lines):
  vkbridge-latency.log        the bridge server's: F pid comm n queued server gpu_done committed shown flags
  droiddeck-latency.log       DroidDeck's: I (pad event -> ring), S (its p50/p99), V (compositor scenes; 8th field 1 = drawn early)
  droiddeck-latency-guest.log the guest's fake Steam Deck: K (ring event -> Deck report -> hidraw read)
  lagprobe.log                tools/lagprobe.exe's: F (each Present), P (input changed at a frame's sample)
All times are CLOCK_MONOTONIC ns except lagprobe's (Wine's QPC clock). Its frames are lined up with
the bridge's lines for its process (lagprobe frame j = the bridge's present j + k, k found from the
gaps' spread), and its clock put on CLOCK_MONOTONIC with the bridge log's C lines (Wine's QPC is the
raw clock: exact) when they fit, else by taking the smallest gap between its Present() call and the
bridge's app present as zero (then "Present() call -> bridge app present" only shows the spread).

Sections: the bridge's stages per process name; DroidDeck's compositor, and the bridge's frames
followed into it (exactly, by the wp_presentation time, when the game draws straight to DroidDeck;
by time, when nested gamescope is between them); the input path; and, with a lagprobe log, each
press from the Android event to the scene that showed the white frame. A stage is left out when no
line has it. The camera (the photon) is not in any log: camera median minus the software total
here is SurfaceFlinger plus the panel (stage 12 in the plan).
"""
import argparse
import bisect
import json
import sys


def pct(vals, q):
    if not vals:
        return None
    v = sorted(vals)
    if len(v) == 1:
        return v[0]
    pos = (len(v) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


class Stages:
    """Named lists of samples in ns, kept in insertion order."""

    def __init__(self):
        self.order, self.vals = [], {}

    def add(self, name, a, b):
        """b - a when both were seen (non-zero) and b is not before a by more than 1 ms."""
        if not a or not b:
            return
        d = b - a
        if d < -1000000:
            return
        if name not in self.vals:
            self.order.append(name)
            self.vals[name] = []
        self.vals[name].append(max(d, 0))

    def rows(self):
        out = []
        for n in self.order:
            v = self.vals[n]
            out.append({'stage': n, 'n': len(v), 'median_ms': pct(v, 0.5) / 1e6, 'p90_ms': pct(v, 0.9) / 1e6,
                        'min_ms': min(v) / 1e6, 'max_ms': max(v) / 1e6})
        return out


def table(title, rows, out):
    if not rows:
        return
    out.write('\n%s\n' % title)
    w = max(40, max(len(r['stage']) for r in rows) + 2)
    out.write('  %-*s %6s %10s %8s %8s %8s\n' % (w, 'stage', 'n', 'median ms', 'p90 ms', 'min', 'max'))
    for r in rows:
        out.write('  %-*s %6d %10.2f %8.2f %8.2f %8.2f\n' % (w, r['stage'], r['n'], r['median_ms'], r['p90_ms'], r['min_ms'], r['max_ms']))


def ints(parts):
    return [int(p) for p in parts]


def parse(paths):
    """Returns (bridge frames, I, S, K, V, lagprobe runs)."""
    frames, ins, summ, ks, vs, probes, clocks = [], [], [], [], [], [], []
    for path in paths:
        try:
            f = open(path, errors='replace')
        except OSError as e:
            sys.exit('latency_report: %s' % e)
        kind = None
        probe = None
        with f:
            for line in f:
                line = line.rstrip('\n')
                if line.startswith('#'):
                    if line.startswith('# lagprobe'):
                        kind = 'lagprobe'
                        probe = {'path': path, 'head': line[2:], 'F': {}, 'P': []}
                        probes.append(probe)
                    elif line.startswith('# vkbridge latency'):
                        kind = 'bridge'
                    continue
                p = line.split()
                if not p:
                    continue
                try:
                    if kind == 'lagprobe' and p[0] == 'F' and len(p) == 3:
                        probe['F'][int(p[1])] = int(p[2])
                    elif kind == 'lagprobe' and p[0] == 'P' and len(p) >= 4:
                        probe['P'].append({'frame': int(p[1]), 't': int(p[2]), 'down': int(p[3]), 'src': int(p[4]) if len(p) > 4 else 0})
                    elif p[0] == 'C' and len(p) == 3:
                        clocks.append((int(p[1]), int(p[2])))
                    elif p[0] == 'F' and len(p) == 10:
                        pid, comm = int(p[1]), p[2]
                        n, q, s, d, c, sh, fl = ints(p[3:10])
                        frames.append({'pid': pid, 'comm': comm, 'n': n, 'queued': q, 'server': s, 'gpu_done': d, 'committed': c,
                                       'shown': sh, 'flags': fl})
                    elif p[0] == 'I' and len(p) >= 7:
                        te, tr, seq = ints(p[1:4])
                        ins.append({'t_event': te, 't_ring': tr, 'seq': seq, 'kind': p[4], 'code': p[5], 'value': p[6]})
                    elif p[0] == 'S' and len(p) >= 5:
                        summ.append(ints(p[1:5]))
                    elif p[0] == 'K' and len(p) >= 5:
                        seq, tre, trep, trd = ints(p[1:5])
                        ks.append({'seq': seq, 't_ring_event': tre, 't_report': trep, 't_read': trd})
                    elif p[0] == 'V' and len(p) >= 7:
                        n, tv, tc, trn, tp, tf = ints(p[1:7])
                        early = int(p[7]) if len(p) >= 8 else 0  # DroidDeck's judder fix: drawn as the game's frame landed
                        vs.append({'n': n, 't_vsync': tv, 't_commit': tc, 't_render': trn, 't_present': tp, 't_feedback': tf,
                                   'early': early})
                except ValueError:
                    continue
    return frames, ins, summ, ks, vs, probes, sorted(clocks)


def bridge_stages(fr, st, prefix=''):
    st.add(prefix + 'app present -> server', fr['queued'], fr['server'])
    st.add(prefix + 'server -> GPU done', fr['server'], fr['gpu_done'])
    st.add(prefix + 'GPU done -> committed', fr['gpu_done'], fr['committed'])
    st.add(prefix + 'committed -> shown (compositor)', fr['committed'], fr['shown'])
    st.add(prefix + 'app present -> shown', fr['queued'], fr['shown'])


def by_process(frames):
    procs = {}
    for fr in frames:
        procs.setdefault((fr['pid'], fr['comm']), []).append(fr)
    for v in procs.values():
        v.sort(key=lambda x: x['n'])
    return procs


class Scenes:
    """DroidDeck's compositor scenes, for following a frame into them."""

    def __init__(self, vs):
        self.vs = sorted(vs, key=lambda v: v['t_render'])
        self.by_fb = {v['t_feedback']: v for v in self.vs if v['t_feedback']}
        self.commits = sorted((v['t_commit'], i) for i, v in enumerate(self.vs) if v['t_commit'])
        self.ckeys = [c for c, _ in self.commits]

    def exact(self, shown):
        return self.by_fb.get(shown) if shown else None

    def after_commit(self, t):
        """The first scene that drew a commit made at or after t (nested gamescope's, after the game's)."""
        if not t:
            return None
        i = bisect.bisect_left(self.ckeys, t)
        return self.vs[self.commits[i][1]] if i < len(self.commits) else None


def align_probe(probe, procs, clocks):
    """(pid key, frame offset k, clock offset ns, how) of the bridge process that made this lagprobe
    run: lagprobe frame j is the bridge's present n = j + k, QPC + offset = CLOCK_MONOTONIC.

    DXVK makes one vkQueuePresentKHR per Present(), so k is 0 unless it skipped or added some. With
    the raw clock (C lines) the k taken is the smallest from 0 outwards for which every bridge present
    comes after its Present() call; without, 0 unless another k lines the gaps up far more tightly
    (a frame queue of DXVK's makes k - 1 look tight too: the earlier frame goes out as the next is
    called, so 0 is preferred)."""
    fj = sorted(probe['F'])
    if not fj:
        return None
    cands = []
    for key, frs in procs.items():
        if not key[1].lower().startswith('lagprobe'):
            continue
        q = {fr['n']: fr['queued'] for fr in frs if fr['queued']}
        if not q:
            continue
        t0 = min(q.values())
        off = None
        if clocks:
            mono, raw = min(clocks, key=lambda c: abs(c[0] - t0))
            off = mono - raw
        for k in sorted(range(-12, 13), key=abs):
            d = [q[j + k] - probe['F'][j] for j in fj if j + k in q]
            if len(d) < max(10, len(fj) // 2):
                continue
            iqr = pct(d, 0.75) - pct(d, 0.25)
            exact = off is not None and min(d) - off > -500000 and pct(d, 0.5) - off < 200000000
            cands.append({'key': key, 'k': k, 'iqr': iqr, 'env': min(d), 'off': off, 'exact': exact, 'n': len(d)})
    if not cands:
        return None
    exact = [c for c in cands if c['exact']]
    if exact:
        c = exact[0]  # nearest 0 first, per process in order
        return c['key'], c['k'], c['off'], 'exact (the raw clock)'
    best = min(cands, key=lambda c: c['iqr'])
    zero = [c for c in cands if c['k'] == 0 and c['key'] == best['key']]
    c = zero[0] if zero and zero[0]['iqr'] < max(3 * best['iqr'], 2000000) else best
    return c['key'], c['k'], c['env'], 'smallest gap taken as zero'


def report(args):
    frames, ins, summ, ks, vs, probes, clocks = parse(args.logs)
    out = sys.stdout
    result = {}
    procs = by_process(frames)
    names = {}
    for (pid, comm), frs in procs.items():
        if args.comm and args.comm not in comm:
            continue
        names.setdefault(comm, []).extend(frs)
    for comm, frs in sorted(names.items()):
        st = Stages()
        for fr in frs:
            bridge_stages(fr, st)
        qs = {}
        for fr in frs:
            qs.setdefault(fr['pid'], []).append(fr['queued'])
        for pid, q in qs.items():
            q = sorted(x for x in q if x)
            for a, b in zip(q, q[1:]):
                st.add('frame interval (app presents)', a, b)
        flags = {'discarded': sum(1 for f in frs if f['flags'] & 4), 'no answer': sum(1 for f in frs if f['flags'] & 8),
                 'failed': sum(1 for f in frs if f['flags'] & 16)}
        title = 'bridge: %s (%d frames, %d process%s; %s)' % (comm, len(frs), len(qs), '' if len(qs) == 1 else 'es',
                                                             ', '.join('%s %d' % kv for kv in flags.items()))
        rows = st.rows()
        table(title, rows, out)
        result['bridge:' + comm] = {'frames': len(frs), 'flags': flags, 'stages': rows}

    scenes = Scenes(vs) if vs else None
    if vs:
        st = Stages()
        for v in vs:
            st.add('vsync tick -> render start', v['t_vsync'], v['t_render'])
            st.add('client commit -> render start', v['t_commit'], v['t_render'])
            st.add('render start -> present done', v['t_render'], v['t_present'])
            st.add('client commit -> present done', v['t_commit'], v['t_present'])
            kind = 'drawn early' if v['early'] else 'on the tick'
            st.add('client commit -> render start (%s)' % kind, v['t_commit'], v['t_render'])
        rows = st.rows()
        early = sum(1 for v in vs if v['early'])
        table('DroidDeck compositor (%d scenes, %d drawn as the game\'s frame landed)' % (len(vs), early), rows, out)
        result['compositor'] = {'scenes': len(vs), 'early': early, 'stages': rows}
        for comm, frs in sorted(names.items()):
            st = Stages()
            exact = nested = 0
            for fr in frs:
                v = scenes.exact(fr['shown'])
                if v:
                    exact += 1
                    st.add('game commit -> DroidDeck present done', fr['committed'], v['t_present'])
                    st.add('app present -> DroidDeck present done', fr['queued'], v['t_present'])
            if not exact:
                for fr in frs:
                    v = scenes.after_commit(fr['committed'])
                    if v and v['t_commit'] - fr['committed'] < 200000000:
                        nested += 1
                        st.add('game commit -> next DroidDeck commit (gamescope)', fr['committed'], v['t_commit'])
                        st.add('game commit -> its DroidDeck present done', fr['committed'], v['t_present'])
                        st.add('app present -> DroidDeck present done', fr['queued'], v['t_present'])
            rows = st.rows()
            how = 'by the wp_presentation time' if exact else 'by time, through nested gamescope'
            table('bridge -> DroidDeck: %s (%d frames followed %s)' % (comm, exact or nested, how), rows, out)
            result['followed:' + comm] = {'how': how, 'stages': rows}

    if ins or ks:
        st = Stages()
        kseq = {k['seq']: k for k in ks}
        for e in ins:
            st.add('Android event -> ring write (PadBridge)', e['t_event'], e['t_ring'])
        iseq = {e['seq']: e for e in ins}
        for k in ks:
            e = iseq.get(k['seq'])
            if e:  # the I line's ring time (the ring's own stamp is a millisecond wall clock)
                st.add('ring write -> Deck report built', e['t_ring'], k['t_report'])
            else:
                st.add('ring event (ms stamp) -> Deck report built', k['t_ring_event'], k['t_report'])
            st.add('Deck report built -> hidraw read (Steam)', k['t_report'], k['t_read'])
        for e in ins:
            k = kseq.get(e['seq'])
            if k:
                st.add('Android event -> hidraw read', e['t_event'], k['t_read'])
        rows = st.rows()
        table('input (%d Android events, %d guest reads)' % (len(ins), len(ks)), rows, out)
        result['input'] = {'stages': rows}
        if summ:
            out.write('  PadBridge 10 s windows: %d, p50 of p50s %.2f ms, worst p99 %.2f ms\n' % (
                len(summ), pct([s[2] for s in summ], 0.5) / 1e6, max(s[3] for s in summ) / 1e6))

    downs = sorted((e['t_event'], e) for e in ins if e['value'] not in ('0', '0.0'))
    dkeys = [t for t, _ in downs]
    kseq = {k['seq']: k for k in ks}
    for probe in probes:
        al = align_probe(probe, procs, clocks)
        presses = [p for p in probe['P'] if p['down']]
        if not al:
            out.write('\nlagprobe %s: %d presses; no bridge process lines up with its frames (no lagprobe F lines in the '
                      'bridge log?)\n' % (probe['path'], len(presses)))
            st = Stages()
            for p in presses:
                st.add('game sample -> Present() call', p['t'], probe['F'].get(p['frame']))
            table('lagprobe (its own clock only)', st.rows(), out)
            continue
        key, k, off, how = al
        frs = {fr['n']: fr for fr in procs[key]}
        st = Stages()
        rows_p = []
        for p in presses:
            fr = frs.get(p['frame'] + k)
            t_sample = p['t'] + off
            t_call = probe['F'].get(p['frame'])
            t_call = t_call + off if t_call else 0
            i = bisect.bisect_right(dkeys, t_sample) - 1
            ev = downs[i][1] if i >= 0 and t_sample - downs[i][0] < 1000000000 else None
            kr = kseq.get(ev['seq']) if ev else None
            v = scenes.exact(fr['shown']) if scenes and fr else None
            if scenes and fr and not v:
                v2 = scenes.after_commit(fr['committed'])
                v = v2 if v2 and v2['t_commit'] - fr['committed'] < 200000000 else None
            if ev:
                st.add('Android event -> ring write', ev['t_event'], ev['t_ring'])
            if kr:
                st.add('ring write -> hidraw read', ev['t_ring'], kr['t_read'])
                st.add('hidraw read -> game sample (Steam, XInput, frame start)', kr['t_read'], t_sample)
            elif ev:
                st.add('ring write -> game sample', ev['t_ring'], t_sample)
            st.add('game sample -> Present() call', t_sample, t_call)
            if fr:
                st.add('Present() call -> bridge app present (DXVK)', t_call, fr['queued'])
                bridge_stages(fr, st)
                st.add('game sample -> shown', t_sample, fr['shown'])
            if v:
                st.add('shown -> DroidDeck present done', fr['shown'] or fr['committed'], v['t_present'])
                st.add('game sample -> DroidDeck present done', t_sample, v['t_present'])
            if ev and fr:
                st.add('TOTAL Android event -> shown', ev['t_event'], fr['shown'])
            if ev and v:
                st.add('TOTAL Android event -> DroidDeck present done', ev['t_event'], v['t_present'])
            rows_p.append({'frame': p['frame'], 'n': fr['n'] if fr else None, 'src': p['src'],
                           'event': ev['t_event'] if ev else 0, 'sample': t_sample, 'present_call': t_call,
                           'queued': fr['queued'] if fr else 0, 'server': fr['server'] if fr else 0,
                           'gpu_done': fr['gpu_done'] if fr else 0, 'committed': fr['committed'] if fr else 0,
                           'shown': fr['shown'] if fr else 0,
                           'droiddeck_present': v['t_present'] if v else 0})
        rows = st.rows()
        table('lagprobe %s: %d presses; bridge process %d %s, frame n = lagprobe frame %+d, clock offset %.3f ms, %s' % (
            probe['path'], len(presses), key[0], key[1], k, off / 1e6, how), rows, out)
        result['lagprobe:' + probe['path']] = {'presses': len(presses), 'pid': key[0], 'frame_offset': k, 'clock': how, 'stages': rows}
        # Every frame, pressed or not: what a press would see after its sample. The frame queue
        # (DXVK's and ours) shows in "Present() call -> GPU done" against the frame interval.
        st = Stages()
        fj = sorted(probe['F'])
        for a, b in zip(fj, fj[1:]):
            if b == a + 1:
                st.add('frame interval (Present() calls)', probe['F'][a], probe['F'][b])
        for j in fj:
            fr = frs.get(j + k)
            if not fr:
                continue
            t_call = probe['F'][j] + off
            st.add('Present() call -> bridge app present (DXVK)', t_call, fr['queued'])
            st.add('Present() call -> GPU done', t_call, fr['gpu_done'])
            st.add('Present() call -> committed', t_call, fr['committed'])
            st.add('Present() call -> shown', t_call, fr['shown'])
        rows = st.rows()
        table('lagprobe %s: every frame (%d)' % (probe['path'], len(fj)), rows, out)
        result['lagprobe:' + probe['path']]['frames'] = rows
        if args.presses:
            out.write('  per press (ms from the Android event, or from the game sample when there is none):\n')
            for r in rows_p:
                base = r['event'] or r['sample']
                cols = [(x, r[x]) for x in ('event', 'sample', 'present_call', 'queued', 'server', 'gpu_done', 'committed', 'shown',
                                            'droiddeck_present')]
                out.write('    frame %d: ' % r['frame'] + '  '.join('%s %.1f' % (n, (t - base) / 1e6) for n, t in cols if t) + '\n')
    if not (frames or vs or ins or ks or probes):
        out.write('no latency lines found\n')
    if args.json:
        json.dump(result, open(args.json, 'w'), indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--presses', action='store_true', help='one line per lagprobe press')
    ap.add_argument('--comm', help='only bridge processes whose name has this')
    ap.add_argument('--json', help='also write the numbers to this file')
    report(ap.parse_args())


if __name__ == '__main__':
    main()
