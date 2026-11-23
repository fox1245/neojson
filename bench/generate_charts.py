#!/usr/bin/env python3
"""Generate publication-grade dark-slate SVG benchmark charts from bench/run.py JSON outputs.

Usage:
    python3 bench/generate_charts.py \
        --x86-64 path/to/x86_64_avx2.json \
        --arm64 path/to/arm64_neon.json \
        --out-dir assets
"""

import argparse
import html
import json
import statistics
from pathlib import Path

LIBS = [
    ("bench_neojson", "neojson", "#fb923c", "Ordered mutable DOM (C++17)"),
    ("bench_yyjson", "yyjson (C)", "#38bdf8", "C API baseline"),
    ("bench_simdjson", "simdjson (DOM)", "#a78bfa", "Read-only tape DOM"),
    ("bench_cppyyjson", "cpp-yyjson", "#2dd4bf", "Ordered mutable/read DOM"),
    ("bench_rapidjson", "RapidJSON", "#60a5fa", "In-place/mutable DOM (SSE2/NEON)"),
    ("bench_nlohmann_json", "nlohmann::json", "#94a3b8", "Unordered mutable DOM"),
    ("bench_nlohmann_ordered", "nlohmann::ordered", "#64748b", "Ordered mutable DOM"),
]
LIB_META = {key: (label, color, kind) for key, label, color, kind in LIBS}

FILE_NOTES = {
    "canada.json": "Dense IEEE-754 float coordinates",
    "citm_catalog.json": "Nested arrays, integers & maps",
    "gsoc-2018.json": "Long strings & deep records",
    "twitter.json": "Mixed timeline objects & tweets",
}

SANS = "-apple-system, BlinkMacSystemFont, 'Segoe UI', 'DejaVu Sans', Arial, sans-serif"
MONO = "'DejaVu Sans Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"


def load_stats(path: Path):
    data = json.loads(path.read_text())
    files = data["files"]
    total_bytes = sum(files.values())
    med = {}
    for row in data["rows"]:
        med[(row["workload"], row["file"], row["lib"])] = (
            statistics.median(row["ns"]),
            row["ratio"],
        )
    return data, files, total_bytes, med


def fmt_mb(n_bytes: int) -> str:
    if n_bytes >= 1_000_000:
        return f"{n_bytes / 1e6:.2f} MB"
    return f"{n_bytes / 1e3:.0f} KB"


def fmt_ms(ms: float) -> str:
    if ms >= 100.0:
        return f"{ms:.0f}ms"
    if ms >= 10.0:
        return f"{ms:.1f}ms"
    return f"{ms:.2f}ms"


def esc(s: str) -> str:
    return html.escape(str(s), quote=True)


def logo_badge(x: int, y: int) -> str:
    return (
        f'<g transform="translate({x},{y}) scale(0.140625)">'
        '<rect x="2" y="2" width="252" height="252" rx="54" fill="#1e293b" stroke="#475569" stroke-width="6"/>'
        '<g fill="none" stroke="#e2e8f0" stroke-width="14" stroke-linecap="round" stroke-linejoin="round">'
        '<path d="M100 60C76 60 76 60 76 84V106C76 120 68 128 54 128C68 128 76 136 76 150V172C76 196 76 196 100 196"/>'
        '<path d="M156 60C180 60 180 60 180 84V106C180 120 188 128 202 128C188 128 180 136 180 150V172C180 196 180 196 156 196"/>'
        '</g>'
        '<polygon points="138,84 106,136 126,136 116,174 152,120 132,120" fill="#fb923c"/>'
        "</g>"
    )


def aggregate_panel_data(files, total_bytes, med, workload: str):
    items = []
    for key, label, color, _ in LIBS:
        if workload == "lookup":
            entry = med.get(("lookup", "twitter.json", key))
            if entry is None:
                continue
            ns, _ = entry
            val = 1e6 / ns  # K lookups/s
            val_str = f"{val:,.1f} K/s ({ns / 1e3:.2f}\u00b5s)"
        else:
            vals = [med.get((workload, fname, key)) for fname in files]
            if any(v is None for v in vals):
                continue
            tot_ns = sum(v[0] for v in vals)
            val = (total_bytes / 1e6) / (tot_ns / 1e9)  # MB/s
            val_str = f"{val:,.0f} MB/s ({fmt_ms(tot_ns / 1e6)})"
        items.append((key, label, color, val, val_str))
    items.sort(key=lambda x: x[3], reverse=True)
    return items


def render_overview_panel(px: int, py: int, pw: int, ph: int, title: str, unit: str, items) -> list[str]:
    out = [
        f'<rect x="{px}" y="{py}" width="{pw}" height="{ph}" rx="12" fill="#1e293b" stroke="#334155" stroke-width="1"/>',
        f'<text x="{px + 16}" y="{py + 25}" fill="#f8fafc" font-family="{SANS}" font-size="13.5" font-weight="700">{esc(title)}</text>',
        f'<text x="{px + pw - 14}" y="{py + 25}" text-anchor="end" fill="#94a3b8" font-family="{SANS}" font-size="10.8" font-weight="600">{esc(unit)}</text>',
    ]
    max_val = max(it[3] for it in items) if items else 1.0
    label_x = px + 136
    bar_x = px + 144
    max_bar_w = 154
    val_x = px + pw - 14
    row_h = 27
    start_y = py + 43

    # Subtle background tracks behind every bar row.
    for idx in range(len(items)):
        ry = start_y + idx * row_h
        out.append(
            f'<rect x="{bar_x}" y="{ry}" width="{max_bar_w}" height="16" rx="4" fill="#0f172a" opacity="0.55"/>'
        )

    # Subtle vertical gridlines strictly within bar plot bounds (bar_x .. bar_x + max_bar_w).
    grid_y1 = start_y - 3
    grid_y2 = start_y + (len(items) - 1) * row_h + 19 if items else start_y + 16
    for frac in (0.25, 0.5, 0.75, 1.0):
        gx = bar_x + max_bar_w * frac
        out.append(
            f'<line x1="{gx:.1f}" y1="{grid_y1}" x2="{gx:.1f}" y2="{grid_y2}" '
            f'stroke="#334155" stroke-width="1" stroke-dasharray="2 3" opacity="0.65"/>'
        )

    for idx, (key, label, color, val, val_str) in enumerate(items):
        ry = start_y + idx * row_h
        is_hero = key == "bench_neojson"
        bw = max(3.0, (val / max_val) * max_bar_w)
        lbl_fill = "#ffffff" if is_hero else "#cbd5e1"
        lbl_weight = "700" if is_hero else "500"
        val_fill = "#fdba74" if is_hero else "#e2e8f0"
        val_weight = "700" if is_hero else "500"
        out.append(
            f'<text x="{label_x}" y="{ry + 12}" text-anchor="end" fill="{lbl_fill}" '
            f'font-family="{SANS}" font-size="11.5" font-weight="{lbl_weight}">{esc(label)}</text>'
        )
        out.append(
            f'<rect x="{bar_x}" y="{ry}" width="{bw:.1f}" height="16" rx="4" fill="{color}"'
            + (' stroke="#ffedd5" stroke-width="0.9"' if is_hero else "")
            + "/>"
        )
        out.append(
            f'<text x="{val_x}" y="{ry + 12}" text-anchor="end" fill="{val_fill}" '
            f'font-family="{MONO}" font-size="10.2" font-weight="{val_weight}">{esc(val_str)}</text>'
        )
    return out


def render_overview_svg(title: str, subtitle: str, files, total_bytes, med, out_path: Path, note_right: str):
    w, h = 960, 670
    parse_items = aggregate_panel_data(files, total_bytes, med, "parse")
    read_items = aggregate_panel_data(files, total_bytes, med, "read")
    write_items = aggregate_panel_data(files, total_bytes, med, "write")
    lookup_items = aggregate_panel_data(files, total_bytes, med, "lookup")

    by_lib = lambda items, key: next(x[3] for x in items if x[0] == key)
    parse_vs_ord = by_lib(parse_items, "bench_neojson") / by_lib(parse_items, "bench_nlohmann_ordered")
    parse_vs_yy = by_lib(parse_items, "bench_neojson") / by_lib(parse_items, "bench_yyjson")
    write_vs_yy = by_lib(write_items, "bench_neojson") / by_lib(write_items, "bench_yyjson")

    pill1 = f"Parse {parse_vs_ord:.1f}\u00d7 vs nlohmann::ordered"
    pill2 = f"Parse {parse_vs_yy:.2f}\u00d7 \u00b7 Write {write_vs_yy:.2f}\u00d7 vs yyjson (C)"

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" role="img" aria-label="{esc(title)}">',
        f'<rect x="1" y="1" width="{w - 2}" height="{h - 2}" rx="16" fill="#0f172a" stroke="#334155" stroke-width="2"/>',
        logo_badge(24, 22),
        f'<text x="72" y="42" fill="#f8fafc" font-family="{SANS}" font-size="18.5" font-weight="800">{esc(title)}</text>',
        f'<text x="72" y="64" fill="#94a3b8" font-family="{SANS}" font-size="11.5">{esc(subtitle)}</text>',
        f'<rect x="472" y="21" width="216" height="25" rx="12.5" fill="#431407" stroke="#fb923c" stroke-width="1"/>',
        f'<text x="580" y="37" text-anchor="middle" fill="#fed7aa" font-family="{SANS}" font-size="9.8" font-weight="700">{esc(pill1)}</text>',
        f'<rect x="698" y="21" width="238" height="25" rx="12.5" fill="#0c4a6e" stroke="#38bdf8" stroke-width="1"/>',
        f'<text x="817" y="37" text-anchor="middle" fill="#bae6fd" font-family="{SANS}" font-size="9.8" font-weight="700">{esc(pill2)}</text>',
    ]

    pw, ph = 448, 244
    lines += render_overview_panel(24, 88, pw, ph, "Parse (Aggregate, 4 Files)", "MB/s · Higher is faster", parse_items)
    lines += render_overview_panel(488, 88, pw, ph, "Parse + Full Traversal Read", "MB/s · Higher is faster", read_items)
    lines += render_overview_panel(24, 344, pw, ph, "Serialize / Write (Minified)", "MB/s · Higher is faster", write_items)
    lines += render_overview_panel(488, 344, pw, ph, "Field Lookup (twitter.json)", "K ops/s · Higher is faster", lookup_items)

    # 2-row Footer legend covering all 7 libraries cleanly on Row 1 and methodology on Row 2
    legend_entries = [
        (40, "#fb923c", "#fed7aa", "700", "neojson (ordered DOM)"),
        (206, "#38bdf8", "#cbd5e1", "500", "yyjson (C)"),
        (292, "#a78bfa", "#cbd5e1", "500", "simdjson (read-only)"),
        (434, "#2dd4bf", "#cbd5e1", "500", "cpp-yyjson"),
        (524, "#60a5fa", "#cbd5e1", "500", "RapidJSON"),
        (610, "#94a3b8", "#cbd5e1", "500", "nlohmann::json"),
        (724, "#64748b", "#cbd5e1", "500", "nlohmann::ordered"),
    ]
    lines.append('<rect x="24" y="600" width="912" height="54" rx="10" fill="#1e293b" stroke="#334155" stroke-width="1"/>')
    for cx, dot_fill, txt_fill, weight, name in legend_entries:
        lines.append(f'<circle cx="{cx}" cy="615.5" r="4.8" fill="{dot_fill}"/>')
        lines.append(
            f'<text x="{cx + 10}" y="619.5" fill="{txt_fill}" font-family="{SANS}" font-size="11" font-weight="{weight}">{esc(name)}</text>'
        )
    lines += [
        '<line x1="36" y1="629" x2="924" y2="629" stroke="#334155" stroke-width="1" opacity="0.75"/>',
        f'<text x="40" y="644.5" fill="#94a3b8" font-family="{SANS}" font-size="10.2">Single-thread in-memory buffers · 7.94 MB across 4 files · MB/s = 10^6 B/s</text>',
        f'<text x="920" y="644.5" text-anchor="end" fill="#94a3b8" font-family="{SANS}" font-size="10.2">{esc(note_right)}</text>',
        "</svg>",
    ]
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_per_file_svg(files, med, out_path: Path):
    w, h = 960, 764
    title = "Per-File Breakdown on x86-64 AVX2 — Parse, Write & Build"
    subtitle = "Intel Core i7-8750H (6C/12T, AVX2) · GCC 13.3.0 -O3 -DNDEBUG · Median across 7 rounds (MB/s = 10^6 B/s, higher is faster)"

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" role="img" aria-label="{esc(title)}">',
        f'<rect x="1" y="1" width="{w - 2}" height="{h - 2}" rx="16" fill="#0f172a" stroke="#334155" stroke-width="2"/>',
        logo_badge(24, 22),
        f'<text x="72" y="42" fill="#f8fafc" font-family="{SANS}" font-size="18.5" font-weight="800">{esc(title)}</text>',
        f'<text x="72" y="64" fill="#94a3b8" font-family="{SANS}" font-size="11.5">{esc(subtitle)}</text>',
    ]

    # Column header strip
    cols = [("parse", "Parse (MB/s)", 188), ("write", "Write Minified (MB/s)", 438), ("build", "Mutable DOM Build (MB/s)", 688)]
    lines.append(f'<rect x="24" y="84" width="912" height="28" rx="8" fill="#1e293b" stroke="#334155" stroke-width="1"/>')
    lines.append(f'<text x="38" y="102" fill="#e2e8f0" font-family="{SANS}" font-size="11.5" font-weight="700">Corpus File / Library</text>')
    for _, col_title, cx in cols:
        lines.append(f'<text x="{cx}" y="102" fill="#e2e8f0" font-family="{SANS}" font-size="11.5" font-weight="700">{esc(col_title)}</text>')

    row_y0 = 122
    row_h = 144
    row_gap = 10

    for f_idx, (fname, fbytes) in enumerate(files.items()):
        ry = row_y0 + f_idx * (row_h + row_gap)
        fnote = FILE_NOTES.get(fname, "")
        lines.append(f'<rect x="24" y="{ry}" width="912" height="{row_h}" rx="12" fill="#1e293b" stroke="#334155" stroke-width="1"/>')
        lines.append(f'<text x="38" y="{ry + 20}" fill="#f8fafc" font-family="{SANS}" font-size="12.5" font-weight="700">{esc(fname)}</text>')
        lines.append(f'<text x="176" y="{ry + 20}" fill="#94a3b8" font-family="{SANS}" font-size="11">{esc(fmt_mb(fbytes) + " · " + fnote)}</text>')
        lines.append(f'<line x1="36" y1="{ry + 27}" x2="924" y2="{ry + 27}" stroke="#334155" stroke-width="1"/>')

        # Compute per-(file, workload) max for scaling inside each column cleanly.
        col_max = {}
        for wl, _, _ in cols:
            vals = [
                (fbytes / 1e6) / (med[(wl, fname, k)][0] / 1e9)
                for k, _, _, _ in LIBS
                if (wl, fname, k) in med
            ]
            col_max[wl] = max(vals) if vals else 1.0

        bar_y0 = ry + 34
        bar_step = 15
        max_bw = 150.0
        for l_idx, (key, label, color, _) in enumerate(LIBS):
            by = bar_y0 + l_idx * bar_step
            is_hero = key == "bench_neojson"
            lbl_fill = "#ffffff" if is_hero else "#cbd5e1"
            lbl_weight = "700" if is_hero else "500"
            lines.append(
                f'<text x="174" y="{by + 9.5}" text-anchor="end" fill="{lbl_fill}" '
                f'font-family="{SANS}" font-size="10.8" font-weight="{lbl_weight}">{esc(label)}</text>'
            )
            for wl, _, cx in cols:
                entry = med.get((wl, fname, key))
                if entry is None:
                    lines.append(
                        f'<text x="{cx + 2}" y="{by + 9.5}" fill="#94a3b8" font-family="{SANS}" font-size="10" font-style="italic">N/A (read-only DOM)</text>'
                    )
                    continue
                mbps = (fbytes / 1e6) / (entry[0] / 1e9)
                bw = max(2.5, (mbps / col_max[wl]) * max_bw)
                val_fill = "#fdba74" if is_hero else "#e2e8f0"
                val_weight = "700" if is_hero else "500"
                lines.append(
                    f'<rect x="{cx}" y="{by + 0.5}" width="{bw:.1f}" height="10.5" rx="3" fill="{color}"'
                    + (' stroke="#ffedd5" stroke-width="0.7"' if is_hero else "")
                    + "/>"
                )
                lines.append(
                    f'<text x="{cx + bw + 6:.1f}" y="{by + 9.5}" fill="{val_fill}" '
                    f'font-family="{MONO}" font-size="9.8" font-weight="{val_weight}">{mbps:,.0f} MB/s</text>'
                )

    lines.append(
        f'<text x="480" y="749" text-anchor="middle" fill="#94a3b8" font-family="{SANS}" font-size="11">'
        "Build constructs an owned mutable DOM via each library's build/copy API (COW string refs in neojson); "
        "simdjson DOM is read-only and has no Build benchmark."
        "</text>"
    )
    lines.append("</svg>")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--x86-64", required=True, help="x86-64 AVX2 JSON path")
    ap.add_argument("--arm64", required=True, help="ARM64 NEON JSON path")
    ap.add_argument("--out-dir", default="assets", help="Output directory for SVG charts")
    opts = ap.parse_args()

    out_dir = Path(opts.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    _, x86_files, x86_bytes, x86_med = load_stats(Path(opts.x86_64))
    _, arm_files, arm_bytes, arm_med = load_stats(Path(opts.arm64))

    render_overview_svg(
        "JSON Throughput on x86-64 (AVX2)",
        f"Intel Core i7-8750H (6C/12T, AVX2) · GCC 13.3.0 -O3 -DNDEBUG · {x86_bytes / 1e6:.2f} MB across 4 files · Higher is faster",
        x86_files,
        x86_bytes,
        x86_med,
        out_dir / "bench_overview_x86_64.svg",
        "7 rounds × 7 batches · simdjson padded input",
    )
    render_per_file_svg(x86_files, x86_med, out_dir / "bench_per_file_x86_64.svg")
    render_overview_svg(
        "JSON Throughput on ARM64 (NEON)",
        "ARM Neoverse-N1 (4 vCPU, NEON) · GCC 13.3.0 -O3 -DNDEBUG · yyjson+COW path (no native ARM SIMD parser) · Higher is faster",
        arm_files,
        arm_bytes,
        arm_med,
        out_dir / "bench_arm64_neon.svg",
        "7 rounds × 7 batches · simdjson padded input",
    )
    print(f"Generated 3 SVG charts in {out_dir}")


if __name__ == "__main__":
    main()
