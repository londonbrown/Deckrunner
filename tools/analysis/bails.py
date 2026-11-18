import json, re, sys, bisect

trace, runlog, t_from = sys.argv[1], sys.argv[2], float(sys.argv[3])

rows = [json.loads(l) for l in open(trace) if l.strip()]

# time <-> tick anchors from the runtime log (trick lines carry both)
anchors, installs, events = [], [], []
for l in open(runlog, encoding='utf-8', errors='replace'):
    m = re.match(r'\[(\d+\.\d+)\] (.*)', l)
    if not m: continue
    t, msg = float(m.group(1)), m.group(2)
    if t < t_from: continue
    mt = re.match(r'tick (\d+): trick (\S+)', msg)
    if mt: anchors.append((t, int(mt.group(1)), mt.group(2)))
    if msg.startswith('collision installed'): installs.append(t)
    if msg.startswith(('session', 'parked', 'fell', 're-centre')): events.append((t, msg[:90]))

ticks_at = [a[0] for a in anchors]
def tick_of(t):
    i = bisect.bisect_left(ticks_at, t)
    if i == 0: a = anchors[0]
    elif i >= len(anchors): a = anchors[-1]
    else:
        a = anchors[i - 1] if t - anchors[i - 1][0] < anchors[i][0] - t else anchors[i]
    return a[1] + (t - a[0]) * 60.0
install_ticks = sorted(tick_of(t) for t in installs)

def nearest_install(tk):
    i = bisect.bisect_left(install_ticks, tk)
    c = [install_ticks[j] for j in (i - 1, i) if 0 <= j < len(install_ticks)]
    return min(c, key=lambda x: abs(x - tk)) if c else None

def last_trick(tk):
    best = None
    for t, k, name in anchors:
        if k <= tk: best = (k, name)
    return best

for i in range(1, len(rows)):
    if rows[i]['state'].startswith('Wipeout') and not rows[i - 1]['state'].startswith('Wipeout'):
        tk = rows[i]['tick']
        hist = []
        for r in rows[max(0, i - 15):i + 1]:
            s = r['state']
            if not hist or hist[-1][0] != s: hist.append([s, 1])
            else: hist[-1][1] += 1
        p = rows[i - 1]
        ni = nearest_install(tk)
        lt = last_trick(tk)
        dz = [round(rows[j]['pos'][2] - rows[j - 1]['pos'][2], 2) for j in range(max(1, i - 4), i + 1)]
        print(f"tick {tk:6}: from {p['state']:15} at {p['speed']:5.1f} m/s; dz/6 ticks {dz}; last trick {lt}; "
              f"nearest install {'' if ni is None else f'{ni - tk:+.0f} ticks'}")
        print('    ' + ' > '.join(f'{s}x{n}' for s, n in hist))
print('events:', *events, sep='\n  ')
