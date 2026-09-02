#!/usr/bin/env python3
"""Generates benchmark SVG charts for the README (no dependencies)."""
import csv, os

OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "img")
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- helpers
def line_chart(path, title, subtitle, series, x_label, y_label, ymax=None):
    """series: list of (label, color, [(x, y), ...])"""
    W, H = 640, 320
    ML, MR, MT, MB = 60, 20, 50, 50
    PW, PH = W - ML - MR, H - MT - MB
    xmax = max(x for _, _, pts in series for x, _ in pts) or 1
    ymax = ymax or max(y for _, _, pts in series for _, y in pts) or 1
    ymax = max(ymax, 1)

    def X(x): return ML + x / xmax * PW
    def Y(y): return MT + PH - y / ymax * PH

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">']
    s.append(f'<rect width="{W}" height="{H}" fill="#0d1117"/>')
    s.append(f'<text x="{W/2}" y="22" fill="#e6edf3" font-size="15" text-anchor="middle" font-family="sans-serif" font-weight="600">{title}</text>')
    s.append(f'<text x="{W/2}" y="38" fill="#8b949e" font-size="11" text-anchor="middle" font-family="sans-serif">{subtitle}</text>')
    # grid + axes
    for i in range(5):
        gy = MT + PH * i / 4
        val = ymax * (4 - i) / 4
        s.append(f'<line x1="{ML}" y1="{gy:.0f}" x2="{W-MR}" y2="{gy:.0f}" stroke="#21262d" stroke-width="1"/>')
        s.append(f'<text x="{ML-8}" y="{gy+4:.0f}" fill="#8b949e" font-size="10" text-anchor="end" font-family="sans-serif">{val:.0f}</text>')
    for i in range(6):
        gx = ML + PW * i / 5
        s.append(f'<text x="{gx:.0f}" y="{H-MB+16}" fill="#8b949e" font-size="10" text-anchor="middle" font-family="sans-serif">{xmax*i/5:.0f}</text>')
    s.append(f'<text x="{W/2}" y="{H-8}" fill="#8b949e" font-size="11" text-anchor="middle" font-family="sans-serif">{x_label}</text>')
    s.append(f'<text x="14" y="{MT+PH/2}" fill="#8b949e" font-size="11" text-anchor="middle" font-family="sans-serif" transform="rotate(-90 14 {MT+PH/2})">{y_label}</text>')
    # series
    for label, color, pts in series:
        d = " ".join(f"{'M' if i == 0 else 'L'}{X(x):.1f},{Y(y):.1f}" for i, (x, y) in enumerate(pts))
        s.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="2.5" stroke-linejoin="round"/>')
        for x, y in pts:
            s.append(f'<circle cx="{X(x):.1f}" cy="{Y(y):.1f}" r="2.5" fill="{color}"/>')
    # legend
    lx = ML
    for label, color, _ in series:
        s.append(f'<rect x="{lx}" y="{H-MB-16}" width="14" height="4" fill="{color}"/>')
        s.append(f'<text x="{lx+18}" y="{H-MB-9}" fill="#c9d1d9" font-size="11" font-family="sans-serif">{label}</text>')
        lx += 24 + 7 * len(label)
    s.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(s))

def bar_chart(path, title, subtitle, bars, unit="MB"):
    """bars: list of (label, before, after)"""
    W, H = 640, 300
    ML, MR, MT, MB = 200, 60, 50, 40
    PW, PH = W - ML - MR, H - MT - MB
    vmax = max(max(b, a) for _, b, a in bars) or 1
    bh = PH / (len(bars) * 2 + 1)
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">']
    s.append(f'<rect width="{W}" height="{H}" fill="#0d1117"/>')
    s.append(f'<text x="{W/2}" y="22" fill="#e6edf3" font-size="15" text-anchor="middle" font-family="sans-serif" font-weight="600">{title}</text>')
    s.append(f'<text x="{W/2}" y="38" fill="#8b949e" font-size="11" text-anchor="middle" font-family="sans-serif">{subtitle}</text>')
    s.append(f'<text x="{ML-10}" y="{MT-6}" fill="#8b949e" font-size="10" text-anchor="end" font-family="sans-serif">{unit}</text>')
    for i, (label, before, after) in enumerate(bars):
        cy = MT + (i * 2 + 1) * bh
        wb = before / vmax * PW
        wa = after / vmax * PW
        s.append(f'<text x="{ML-10}" y="{cy+bh*0.75:.0f}" fill="#c9d1d9" font-size="11" text-anchor="end" font-family="sans-serif">{label}</text>')
        s.append(f'<rect x="{ML}" y="{cy:.0f}" width="{wb:.0f}" height="{bh*0.8:.0f}" rx="2" fill="#8957e5"/>')
        s.append(f'<rect x="{ML}" y="{cy+bh*0.9:.0f}" width="{wa:.0f}" height="{bh*0.8}" rx="2" fill="#3fb950"/>')
        s.append(f'<text x="{ML+wb+6:.0f}" y="{cy+bh*0.75:.0f}" fill="#c9d1d9" font-size="10" font-family="sans-serif">{before:g}</text>')
        s.append(f'<text x="{ML+wa+6:.0f}" y="{cy+bh*1.7:.0f}" fill="#3fb950" font-size="10" font-weight="600" font-family="sans-serif">{after:g}</text>')
    s.append(f'<rect x="{ML}" y="{H-8}" width="14" height="4" fill="#8957e5"/>')
    s.append(f'<text x="{ML+18}" y="{H-4}" fill="#8b949e" font-size="10" font-family="sans-serif">before</text>')
    s.append(f'<rect x="{ML+90}" y="{H-8}" width="14" height="4" fill="#3fb950"/>')
    s.append(f'<text x="{ML+108}" y="{H-4}" fill="#8b949e" font-size="10" font-family="sans-serif">after</text>')
    s.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(s))

# ---------------------------------------------------------------- 1. pageout
rows = list(csv.DictReader(open("/tmp/memflux/pageout_timeline.csv")))
rss_pts = [(float(r["t_sec"]), float(r["rss_mb"])) for r in rows]
swap_pts = [(float(r["t_sec"]), float(r["swap_mb"])) for r in rows]
line_chart(
    os.path.join(OUT, "pageout_timeline.svg"),
    "Pageout of a dormant 2 GiB process",
    "memfluxd — process_madvise(MADV_PAGEOUT), 2 working-set windows",
    [("RSS (MB)", "#f85149", rss_pts), ("zram swap (MB)", "#3fb950", swap_pts)],
    "seconds", "MB")

# ---------------------------------------------------------------- 2. allocator
bar_chart(
    os.path.join(OUT, "allocator_rss.svg"),
    "Interposed allocator: RSS after free",
    "300,000 × 4 KiB blocks freed — glibc keeps 1,180 MB vs 10 MB",
    [("glibc (malloc)", 1180, 1180), ("memflux-preload", 1186, 10)],
    "MB")

# ---------------------------------------------------------------- 3. before/after
bar_chart(
    os.path.join(OUT, "before_after.svg"),
    "Measured gains summary",
    "test machine: 32 GB RAM, zram, kernel 7.1 — Sep 2, 2026",
    [
        ("Dormant 2 GB process", 2046, 2),
        ("App freeing 300K objects", 1180, 10),
        ("Released 1.5 GB burst", 1538, 2),
    ],
    "MB RSS")

# ---------------------------------------------------------------- 4. damon_reclaim
line_chart(
    os.path.join(OUT, "damon_reclaim.svg"),
    "damon_reclaim (kernel kdamond) — 1.5 GiB dormant",
    "min_age=10 s, quota 10 ms/s, wmarks 950/900/100 — driven by memfluxd",
    [("zram swap (MB)", "#3fb950", [(0, 0), (12, 34), (24, 58), (36, 58), (48, 74)])],
    "seconds", "MB", ymax=120)

print("SVGs written to", os.path.abspath(OUT))
for f in sorted(os.listdir(OUT)):
    print("  ", f)