"""Deckrunner's logo: original geometric letterforms (no font needed), neon on a rainy night skyline, mirrored on a wet
street. One scene laid out for any size.

    python media/make_logo.py            writes deckrunner-logo.svg (1600x640), deckrunner-header.svg (1300x372) and
                                         deckrunner-image.svg (1920x1080) beside this script
"""
import random
from pathlib import Path

GLYPHS = {
    'D': 'M0,0 H44 L64,20 V80 L44,100 H0 Z M14,14 V86 H38 L50,74 V26 L38,14 Z',
    'E': 'M0,0 H64 V14 H14 V43 H54 V57 H14 V86 H64 V100 H0 Z',
    'C': 'M20,0 H64 V14 H26 L14,26 V74 L26,86 H64 V100 H20 L0,80 V20 Z',
    'K': 'M0,0 H14 V100 H0 Z M14,43 L48,0 H66 L31,50 L66,100 H48 L14,57 Z',
    'R': 'M0,0 H48 L64,16 V42 L52,54 L66,100 H51 L38,58 H14 V100 H0 Z M14,14 V44 H42 L50,36 V22 L42,14 Z',
    'U': 'M0,0 H14 V78 L22,86 H42 L50,78 V0 H64 V86 L50,100 H14 L0,86 Z',
    'N': 'M0,0 H16 L50,70 V0 H64 V100 H48 L14,30 V100 H0 Z',
}
WORD, ADV, GAP = 'DECKRUNNER', 64, 24
WORD_UNITS = len(WORD) * ADV + (len(WORD) - 1) * GAP  # 856 x 100


def render(W, H, horizon_frac, word_frac=0.78):
    rnd = random.Random(2077)
    horizon = round(H * horizon_frac)
    scale = min(word_frac * W / WORD_UNITS, 0.32 * H / 100)
    s = scale / 1.45  # the original 1600x640 layout's unit
    hL = 100 * scale
    sub_y = 104 * s  # subtitle baseline below the letters
    y0 = horizon - 18 * s - (hL + sub_y)
    x0 = (W - WORD_UNITS * scale) / 2
    f = H / 640  # skyline height factor
    mid = 560 * W / 1600  # the low middle of the skyline, under the board and subtitle
    letters = '\n'.join(f'      <use href="#g{c}" x="{i * (ADV + GAP)}"/>' for i, c in enumerate(WORD))

    towers, x = [], -10
    while x < W + 10:
        w = rnd.randint(28, 90) * max(1, f * 0.8)
        m = abs(x + w / 2 - W / 2) < mid
        h = rnd.uniform(4, 14) * s if m else rnd.randint(70, 250) * f  # the middle stays under the subtitle
        towers.append(f'M{x:.0f},{horizon} V{horizon - h:.0f} H{x + w:.0f} V{horizon} Z')
        if not m and rnd.random() < 0.3:
            c = x + w / 2
            towers.append(f'M{c - 2:.0f},{horizon - h:.0f} V{horizon - h - rnd.randint(20, 60) * f:.0f} H{c + 2:.0f} V{horizon - h:.0f} Z')
        x += w + rnd.randint(-6, 10)
    area = W * H / (1600 * 640)
    windows = ''.join(f'<rect x="{rnd.randint(0, W)}" y="{rnd.randint(int(horizon - 230 * f), horizon - 8)}" width="3" height="2"/>'
                      for _ in range(int(260 * area)))
    rain = []
    for _ in range(int(220 * area)):
        rx, ry, l = rnd.randint(-100, W), rnd.randint(-40, H), rnd.randint(18, 60) * max(1, f * 0.8)
        rain.append(f'M{rx},{ry} l{l * 0.22:.1f},{l:.0f}')
    board_y = y0 + hL + 46 * s
    defs = '\n'.join(f'    <path id="g{c}" d="{d}" fill-rule="evenodd"/>' for c, d in GLYPHS.items())
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-label="Deckrunner">
  <title>Deckrunner</title>
  <defs>
{defs}
    <linearGradient id="sky" x1="0" y1="0" x2="0" y2="{horizon}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#05060c"/>
      <stop offset="0.68" stop-color="#140a1c"/>
      <stop offset="0.92" stop-color="#3a1220"/>
      <stop offset="1" stop-color="#7a2a1a"/>
    </linearGradient>
    <radialGradient id="haze" cx="{W / 2}" cy="{horizon}" r="{max(W, H) * 0.6:.0f}" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#ff7a2e" stop-opacity="0.45"/>
      <stop offset="0.5" stop-color="#b8263a" stop-opacity="0.18"/>
      <stop offset="1" stop-color="#000" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="neon" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="100">
      <stop offset="0" stop-color="#fff2e8"/>
      <stop offset="0.35" stop-color="#ff9a6a"/>
      <stop offset="0.75" stop-color="#ff3b3b"/>
      <stop offset="1" stop-color="#c0123a"/>
    </linearGradient>
    <linearGradient id="street" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#0c0a12"/>
      <stop offset="1" stop-color="#030306"/>
    </linearGradient>
    <linearGradient id="fade" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#fff" stop-opacity="0.5"/>
      <stop offset="1" stop-color="#fff" stop-opacity="0"/>
    </linearGradient>
    <mask id="reflect"><rect x="0" y="{horizon}" width="{W}" height="{H - horizon}" fill="url(#fade)"/></mask>
    <filter id="glow" x="-20%" y="-60%" width="140%" height="220%">
      <feGaussianBlur in="SourceGraphic" stdDeviation="3" result="b1"/>
      <feGaussianBlur in="SourceGraphic" stdDeviation="14" result="b2"/>
      <feColorMatrix in="b2" type="matrix" values="1 0 0 0 0.2  0 0.3 0 0 0  0 0 0.3 0 0.05  0 0 0 1.4 0" result="red"/>
      <feMerge><feMergeNode in="red"/><feMergeNode in="b1"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="soft"><feGaussianBlur stdDeviation="{2.2 * s:.2f}"/></filter>
    <pattern id="scan" width="4" height="4" patternUnits="userSpaceOnUse">
      <rect width="4" height="1" fill="#000" opacity="0.35"/>
    </pattern>
  </defs>

  <rect width="{W}" height="{H}" fill="#06070d"/>
  <rect width="{W}" height="{horizon}" fill="url(#sky)"/>
  <rect width="{W}" height="{H}" fill="url(#haze)"/>
  <path fill="#07070e" d="{' '.join(towers)}"/>
  <g fill="#ffb36b" opacity="0.55">{windows}</g>
  <rect x="0" y="{horizon}" width="{W}" height="{H - horizon}" fill="url(#street)"/>
  <rect x="0" y="{horizon - 1}" width="{W}" height="2" fill="#ff5a3a" opacity="0.8" filter="url(#soft)"/>

  <g id="word" transform="translate({x0:.1f},{y0:.1f}) scale({scale:.4f})" fill="url(#neon)" filter="url(#glow)">
{letters}
  </g>
  <g transform="translate({W / 2:.1f},{board_y:.1f}) scale({s:.4f})" fill="none" stroke="#ff4d4d" stroke-width="5" stroke-linecap="round" filter="url(#glow)">
    <path d="M-230,-6 Q-262,-6 -270,-26 M-230,-6 H230 M230,-6 Q262,-6 270,-26"/>
    <circle cx="-150" cy="14" r="11"/>
    <circle cx="150" cy="14" r="11"/>
  </g>
  <text x="{W / 2:.1f}" y="{y0 + hL + sub_y:.1f}" text-anchor="middle" fill="#ffb08a" opacity="0.85"
        font-family="Consolas, 'Courier New', monospace" font-size="{22 * s:.1f}" letter-spacing="{14 * s:.1f}">SKATE 3 // NIGHT CITY</text>

  <g mask="url(#reflect)" opacity="0.6">
    <g transform="translate(0,{horizon + 12 * s + y0 + hL:.1f}) scale(1,-1)">
      <use href="#word" filter="url(#soft)"/>
    </g>
  </g>
  <path stroke="#9fb4ff" stroke-width="1" opacity="0.16" d="{' '.join(rain)}"/>
  <rect width="{W}" height="{H}" fill="url(#scan)"/>
</svg>
'''


if __name__ == '__main__':
    here = Path(__file__).resolve().parent
    for name, (w, h, hz) in {'deckrunner-logo': (1600, 640, 470 / 640), 'deckrunner-header': (1300, 372, 0.80),
                             'deckrunner-image': (1920, 1080, 0.70)}.items():
        (here / f'{name}.svg').write_text(render(w, h, hz), encoding='utf-8', newline='\n')
        print(f'{name}.svg {w}x{h}')
