# SPDX-FileCopyrightText: 2026 ProCam Contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Generates icons/procam.svg; render the PNG with
#   python3 tools/icon/make_icon.py icons/procam.svg
#   rsvg-convert -w 500 -h 500 icons/procam.svg -o icons/procam-sfos.png
import math, sys
C = 250.0
R = 143.0     # inner edge of the white lens ring (blade outer edge)
A = 50.0      # aperture opening (hexagon circumradius)

def pt(r, deg):
    a = math.radians(deg)
    return (C + r * math.cos(a), C + r * math.sin(a))

hexagon = [pt(A, i * 60 - 90) for i in range(6)]

def extend(p, q):
    # From q, continue along p->q until the circle of radius R.
    dx, dy = q[0] - p[0], q[1] - p[1]
    n = math.hypot(dx, dy); dx /= n; dy /= n
    fx, fy = q[0] - C, q[1] - C
    b = fx * dx + fy * dy
    c = fx * fx + fy * fy - R * R
    t = -b + math.sqrt(b * b - c)
    return (q[0] + t * dx, q[1] + t * dy)

ends = [extend(hexagon[i], hexagon[(i + 1) % 6]) for i in range(6)]
f = lambda p: f"{p[0]:.2f},{p[1]:.2f}"
blades = []
shades = ["#141a4a", "#1d2563"]
for i in range(6):
    v1, v2 = hexagon[(i + 1) % 6], hexagon[(i + 2) % 6]
    p1, p2 = ends[i], ends[(i + 1) % 6]
    d = f"M{f(v1)} L{f(p1)} A{R},{R} 0 0 1 {f(p2)} L{f(v2)} Z"
    blades.append(f'<path d="{d}" fill="{shades[i % 2]}"/>')
edges = "".join(f'<line x1="{hexagon[(i + 1) % 6][0]:.2f}" y1="{hexagon[(i + 1) % 6][1]:.2f}" '
                f'x2="{ends[i][0]:.2f}" y2="{ends[i][1]:.2f}"/>' for i in range(6))
opening = " ".join(f(p) for p in hexagon)

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="500" height="500" viewBox="0 0 500 500">
  <!-- ProCam app icon: HDR-sunset disc, lens ring, six-blade aperture. -->
  <defs>
    <linearGradient id="sky" x1="0.15" y1="0.05" x2="0.85" y2="0.95">
      <stop offset="0" stop-color="#ffb347"/>
      <stop offset="0.38" stop-color="#ff5e62"/>
      <stop offset="0.72" stop-color="#8a2be2"/>
      <stop offset="1" stop-color="#2b1a7a"/>
    </linearGradient>
    <radialGradient id="glass" cx="0.42" cy="0.38" r="0.7">
      <stop offset="0" stop-color="#e8fbff"/>
      <stop offset="0.45" stop-color="#5ee7ff"/>
      <stop offset="1" stop-color="#3a5bff"/>
    </radialGradient>
    <radialGradient id="shine" cx="0.3" cy="0.2" r="0.8">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.35"/>
      <stop offset="0.6" stop-color="#ffffff" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <circle cx="250" cy="250" r="240" fill="url(#sky)"/>
  <circle cx="250" cy="250" r="240" fill="url(#shine)"/>
  <circle cx="250" cy="250" r="168" fill="#0d1033" fill-opacity="0.35"/>
  <circle cx="250" cy="250" r="150" fill="#0b0f2e"/>
  <circle cx="250" cy="250" r="150" fill="none" stroke="#ffffff" stroke-width="16"/>
  {"".join(blades)}
  <g stroke="#ffffff" stroke-opacity="0.55" stroke-width="3" stroke-linecap="round">{edges}</g>
  <polygon points="{opening}" fill="url(#glass)"/>
  <polygon points="{opening}" fill="none" stroke="#ffffff" stroke-opacity="0.8" stroke-width="3" stroke-linejoin="round"/>
  <circle cx="232" cy="231" r="11" fill="#ffffff" fill-opacity="0.9"/>
  <ellipse cx="322" cy="168" rx="22" ry="11" transform="rotate(-40 322 168)" fill="#ffffff" fill-opacity="0.85"/>
</svg>
'''
open(sys.argv[1], "w").write(svg)
