"""Simulates how the plugin interpolates V between Skate ticks (StageC.cpp Follow) against the runtime's worker
(lib.rs Job::Step accumulator), at the 60 fps the 0.17.0 run showed, and alternatives keyed to the runtime's
simulation clock: read after posting the step ('clock') or before it ('clock-pre', the proposed fix). Prints the frame-to-frame step of V's sent position at 8.4 m/s (14 cm per tick)."""
import random

P = 1 / 60          # Skate tick period
SPEED = 8.4         # m/s
random.seed(1)

def run(scheme, frames=6000, fps=60.0, jitter_ms=1.0, cost_ms=(1.0, 3.0), read_ms=(0.2, 1.5)):
    t = 0.0
    acc = 0.0
    tick = 0
    worker_free = 0.0
    published = []          # (time published, tick, acc after the step that produced it, steps processed)
    posted = []             # (time posted, dt)
    steps_done = 0
    # plugin state for the current scheme
    prev = cur = None; cur_at = 0.0
    hist = []               # (tick, pos) samples seen, for the sim-clock scheme
    out = []
    last_t = 0.0
    for k in range(frames):
        t = k / fps + random.uniform(-jitter_ms, jitter_ms) / 1000
        dt = max(0.0, t - last_t) if k else 1 / fps
        last_t = t
        posted.append((t, dt))
        # the worker: processes this step's ticks after it is free, each tick costing cost_ms
        start = max(t, worker_free)
        acc = min(acc + min(dt, 0.25), 0.25)
        n = 0
        while acc >= P:
            acc -= P
            tick += 1
            n += 1
            start += random.uniform(*cost_ms) / 1000
            published.append((start, tick, acc, k + 1))
        if n == 0:
            published.append((start + 0.0001, tick, acc, k + 1))
        worker_free = start
        # the plugin reads a little after posting
        # 'clock-pre' reads the runtime at the start of the plugin's update, before posting this frame's step
        read = (t - 0.00001) if scheme == 'clock-pre' else t + random.uniform(*read_ms) / 1000
        seen = [p for p in published if p[0] <= read]
        if not seen:
            continue
        _, ltick, lacc, lsteps = seen[-1]
        pos = lambda tk: tk * P * SPEED
        if scheme == 'noticed':   # 0.17.0: alpha from when the plugin noticed the tick
            if cur is None: prev = cur = ltick; cur_at = read
            elif ltick != cur: prev, cur, cur_at = cur, ltick, read
            a = min(max((read - cur_at) / P, 0.0), 1.0)
            x = pos(prev) + (pos(cur) - pos(prev)) * a
        else:                     # runtime clock: tick + (acc + dt of steps posted but not yet processed) / P, one tick behind
            pending = sum(d for (_, d) in (posted[lsteps:-1] if scheme == 'clock-pre' else posted[lsteps:]))
            f = ltick + (lacc + pending) / P - 1.0
            if not hist or hist[-1][0] != ltick: hist.append((ltick, pos(ltick)))
            hist[:] = hist[-4:]
            lo = max((h for h in hist if h[0] <= f), default=hist[0], key=lambda h: h[0])
            hi = min((h for h in hist if h[0] >= f), default=hist[-1], key=lambda h: h[0])
            a = 0.0 if hi[0] == lo[0] else (f - lo[0]) / (hi[0] - lo[0])
            x = lo[1] + (hi[1] - lo[1]) * a
        out.append(x)
    steps = [b - a for a, b in zip(out, out[1:])][60:]
    per_frame = SPEED / fps
    big = sum(1 for s in steps if abs(s - per_frame) > 0.5 * per_frame)
    stalls = sum(1 for s in steps if s < 0.25 * per_frame)
    rms = (sum((s - per_frame) ** 2 for s in steps) / len(steps)) ** 0.5
    return f"{scheme:8}: per-frame step should be {100*per_frame:.1f} cm; off by >50% in {100*big/len(steps):.1f}% of frames " \
           f"(stalls {100*stalls/len(steps):.1f}%); max step {100*max(steps):.1f} cm; rms error {100*rms:.1f} cm"

for scheme in ('noticed', 'clock', 'clock-pre'):
    print(run(scheme))
print('at 120 fps:')
for scheme in ('noticed', 'clock', 'clock-pre'):
    print(run(scheme, fps=120.0))
