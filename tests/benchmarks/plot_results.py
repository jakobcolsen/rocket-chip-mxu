#!/usr/bin/env python3
"""
plot_results.py — Thesis-quality plots from benchmark results.

Usage: python3 plot_results.py [results.csv]

Generates 5 publication-quality, grayscale-safe figures:
  1. Cycles vs Matrix Size (line)
  2. MFLOP/s Throughput @ 100 MHz (line)
  3. D-cache Miss Count (line)
  4. Pipeline Stall Breakdown (stacked bar + hatching)
  5. MXU vs Vanilla Rocket Regression (grouped bar + hatching)

All figures use distinct markers, line styles, and hatching patterns
to ensure readability for monochromatic color-blind viewers.
"""

import sys
import csv
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

# ── Style ────────────────────────────────────────────────────────────
plt.rcParams.update({
    'font.family': 'serif',
    'font.size': 12,
    'axes.titlesize': 15,
    'axes.labelsize': 13,
    'legend.fontsize': 11,
    'xtick.labelsize': 11,
    'ytick.labelsize': 11,
    'figure.dpi': 150,
    'savefig.dpi': 300,
    'axes.grid': True,
    'grid.alpha': 0.3,
})

# ── Grayscale-safe style definitions ─────────────────────────────────
# Each benchmark gets a unique (color, marker, linestyle, hatch) tuple
# so the plots are fully readable in pure grayscale.
SERIES = {
    'gemm_single': {
        'label': 'Single-Core',
        'color': '#888888',       # gray
        'marker': 'o',            # circle
        'linestyle': '-',         # solid
        'hatch': '',              # solid fill
    },
    'gemm_mimd': {
        'label': 'MIMD (4-core)',
        'color': '#4A90D9',       # blue
        'marker': 's',            # square
        'linestyle': '--',        # dashed
        'hatch': '//',
    },
    'gemm_simd': {
        'label': 'SIMD (4-core)',
        'color': '#50C878',       # green
        'marker': '^',            # triangle
        'linestyle': ':',         # dotted
        'hatch': '\\\\',
    },
    'gemm_csr': {
        'label': 'CSR Systolic (4-core)',
        'color': '#F5A623',       # orange
        'marker': 'D',            # diamond
        'linestyle': '-.',        # dash-dot
        'hatch': 'xx',
    },
}

BENCH_ORDER = ['gemm_single', 'gemm_mimd', 'gemm_simd', 'gemm_csr']

def load_csv(path):
    """Load results.csv into dict keyed by (benchmark, N, simulator)."""
    data = defaultdict(dict)
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = (row['benchmark'], int(row['N']), row['simulator'])
            data[key] = {k: int(v) if v.isdigit() else v for k, v in row.items()}
    return data

def get_vals(data, bench, sizes, sim='mxu', field='cycles'):
    """Extract a field for a benchmark across sizes."""
    return [data.get((bench, s, sim), {}).get(field, 0) for s in sizes]


# ── Accessible annotation helper ─────────────────────────────────────

# Short prefixes so labels are identifiable without color (accessibility)
TAG = {
    'gemm_single': 'SC',
    'gemm_mimd':   'MIMD',
    'gemm_simd':   'SIMD',
    'gemm_csr':    'CSR',
}


def _annotate_clusters(ax, sizes, bench_vals, is_log=False, y_cap=None,
                       fmt=lambda v: f'{v:,}'):
    """Smart annotation: cluster close points and stack labels above them.

    At each x-position, points within a proximity threshold are grouped.
    Each group's labels are stacked above the topmost data point in
    legend order (SC -> MI -> SI -> CSR, top to bottom) so the plot is
    fully readable in monochromatic grayscale.

    For points that exceed y_cap (linear-scale plots only), an arrow
    annotation is placed near the top of the chart.
    """
    label_h = 16  # approx label height in points (scaled for publication)

    for xi, x in enumerate(sizes):
        # Collect (bench, value) at this x
        points = []
        for bench in BENCH_ORDER:
            v = bench_vals.get(bench, [0] * len(sizes))[xi]
            if v > 0:
                points.append((bench, v))

        if not points:
            continue

        # Sort ascending by value
        points.sort(key=lambda p: p[1])

        # Group points that are "close" on the visible scale
        if is_log:
            def closeness(a, b):
                return np.log10(b) - np.log10(a) < 0.15
        else:
            visible_range = y_cap if y_cap else (points[-1][1] or 1)
            def closeness(a, b):
                return (b - a) / visible_range < 0.10

        groups = [[points[0]]]
        for i in range(1, len(points)):
            if closeness(groups[-1][-1][1], points[i][1]):
                groups[-1].append(points[i])
            else:
                groups.append([points[i]])

        # Annotate each group
        for group in groups:
            top_val = group[-1][1]  # highest value in cluster

            # Sort by value descending: highest value label at top of stack
            group.sort(key=lambda p: -p[1])
            n = len(group)

            for i, (bench, v) in enumerate(group):
                text = f'{TAG[bench]}: {fmt(v)}'

                # Handle off-chart points (linear scale with y_cap)
                if y_cap and v > y_cap:
                    ax.annotate(f'{TAG[bench]}: {fmt(v)} \u2191',
                                (x, y_cap * 0.95),
                                ha='center', fontsize=9,
                                color='black', fontweight='bold',
                                bbox=dict(boxstyle='round,pad=0.15',
                                          facecolor='white', alpha=0.85,
                                          edgecolor='gray', linewidth=0.5))
                    continue

                # Stack: legend-order top-to-bottom, index 0 = highest offset
                offset_y = 8 + (n - 1 - i) * label_h
                anchor = min(top_val, y_cap) if y_cap else top_val
                ax.annotate(text, (x, anchor),
                            textcoords='offset points',
                            xytext=(0, offset_y), ha='center', fontsize=9,
                            color='black', fontweight='bold',
                            bbox=dict(boxstyle='round,pad=0.15',
                                      facecolor='white', alpha=0.85,
                                      edgecolor='gray', linewidth=0.5))


# ── Fig 1: Cycles vs Matrix Size (line graph) ───────────────────────

def plot_cycles(data, sizes, outdir):
    """Fig 1: Cycles vs Matrix Size — line graph with markers."""
    fig, ax = plt.subplots(figsize=(10, 6))

    bench_vals = {}
    for bench in BENCH_ORDER:
        s = SERIES[bench]
        vals = [int(v) for v in get_vals(data, bench, sizes, field='cycles')]
        bench_vals[bench] = vals
        ax.plot(sizes, vals, marker=s['marker'], linestyle=s['linestyle'],
                color=s['color'], label=s['label'], linewidth=2, markersize=8)

    _annotate_clusters(ax, sizes, bench_vals, is_log=True)

    ax.set_xlabel('Matrix Size (N\u00d7N)')
    ax.set_ylabel('Cycles (log scale)')
    ax.set_title('SGEMM Execution Cycles by Mode (lower is better)')
    ax.set_xticks(sizes)
    ax.set_yscale('log')
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig1_cycles.png'))
    fig.savefig(os.path.join(outdir, 'fig1_cycles.pdf'))
    plt.close(fig)
    print("  Fig 1: Cycles vs Matrix Size")


# ── Fig 2: MFLOP/s Throughput (line graph) ───────────────────────────

def plot_mflops(data, sizes, outdir):
    """Fig 2: MFLOP/s throughput assuming 100 MHz clock."""
    CLOCK_HZ = 100e6  # 100 MHz (verified via Vivado implementation)
    fig, ax = plt.subplots(figsize=(10, 6))

    bench_vals = {}
    for bench in BENCH_ORDER:
        s = SERIES[bench]
        vals = get_vals(data, bench, sizes, field='cycles')
        mflops = [(2.0 * sz**3) / (int(v) / CLOCK_HZ) / 1e6 if int(v) > 0 else 0
                  for v, sz in zip(vals, sizes)]
        bench_vals[bench] = mflops
        ax.plot(sizes, mflops, marker=s['marker'], linestyle=s['linestyle'],
                color=s['color'], label=s['label'], linewidth=2, markersize=8)

    _annotate_clusters(ax, sizes, bench_vals, is_log=False,
                        fmt=lambda v: f'{round(v):,}')

    ax.set_xlabel('Matrix Size (N\u00d7N)')
    ax.set_ylabel('MFLOP/s')
    ax.set_title('Compute Throughput @ 100 MHz (higher is better)')
    ax.set_xticks(sizes)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig2_mflops.png'))
    fig.savefig(os.path.join(outdir, 'fig2_mflops.pdf'))
    plt.close(fig)
    print("  Fig 2: MFLOP/s Throughput")


# ── Fig 3: D-cache Miss Count (line graph) ──────────────────────────

def plot_dcache_miss(data, sizes, outdir):
    """Fig 3: D-cache miss count per mode."""
    fig, ax = plt.subplots(figsize=(10, 6))

    # Gather values and compute y-cap from multi-core series
    multicore_max = 0
    bench_vals = {}
    for bench in BENCH_ORDER:
        vals = [int(v) for v in get_vals(data, bench, sizes, field='dcache_miss')]
        bench_vals[bench] = vals
        if bench != 'gemm_single':
            multicore_max = max(multicore_max, max(vals))
    y_cap = multicore_max * 1.4

    for bench in BENCH_ORDER:
        s = SERIES[bench]
        ax.plot(sizes, bench_vals[bench], marker=s['marker'],
                linestyle=s['linestyle'], color=s['color'], label=s['label'],
                linewidth=2, markersize=8, clip_on=True)

    _annotate_clusters(ax, sizes, bench_vals, is_log=False, y_cap=y_cap)

    ax.set_xlabel('Matrix Size (N\u00d7N)')
    ax.set_ylabel('D-cache Misses')
    ax.set_title('D-cache Miss Count by Mode (lower is better)')
    ax.set_xticks(sizes)
    ax.set_ylim(bottom=-0.02 * y_cap, top=y_cap)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig3_dcache_miss.png'))
    fig.savefig(os.path.join(outdir, 'fig3_dcache_miss.pdf'))
    plt.close(fig)
    print("  Fig 3: D-cache Miss Count")


# ── Fig 4: Pipeline Stall Breakdown (stacked bar + hatching) ────────

def plot_stall_breakdown(data, sizes, outdir):
    """Fig 4: Pipeline stall breakdown — stacked bar per mode."""
    fig, axes = plt.subplots(1, len(sizes), figsize=(3 * len(sizes), 6), sharey=False)
    if len(sizes) == 1:
        axes = [axes]

    stall_fields = ['load_use', 'dcache_blocked', 'csr_interlock', 'systolic_stall']
    # Grayscale-safe: distinct luminance + hatching per stall category
    stall_styles = [
        {'color': '#E74C3C', 'hatch': '',     'label': 'Load-Use'},
        {'color': '#3498DB', 'hatch': '//',   'label': 'D$ Blocked'},
        {'color': '#F39C12', 'hatch': '\\\\', 'label': 'CSR Interlock'},
        {'color': '#9B59B6', 'hatch': 'xx',   'label': 'SIMD Stall'},
    ]

    for ax, sz in zip(axes, sizes):
        x = np.arange(len(BENCH_ORDER))
        bottom = np.zeros(len(BENCH_ORDER))
        for field, style in zip(stall_fields, stall_styles):
            vals = [int(data.get((b, sz, 'mxu'), {}).get(field, 0)) for b in BENCH_ORDER]
            ax.bar(x, vals, 0.6, bottom=bottom, color=style['color'],
                   hatch=style['hatch'], label=style['label'],
                   edgecolor='white', linewidth=0.5)
            bottom += np.array(vals)

        # Total stall count above each bar
        for xi, total in enumerate(bottom):
            if total > 0:
                ax.text(xi, total, f'{int(total)}', ha='center', va='bottom', fontsize=9)

        ax.set_title(f'N={sz}')
        ax.set_xticks(x)
        ax.set_xticklabels(['Single', 'MIMD', 'SIMD', 'CSR'], rotation=45, ha='right')

    for ax in axes:
        ax.set_ylabel('Stall Cycles')
    # Grab handles from first axes only (avoid duplicates)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', ncol=4, fontsize=10,
              bbox_to_anchor=(0.5, 0.93), frameon=True)
    fig.suptitle('Pipeline Stall Breakdown (lower is better)', fontsize=15, y=0.98)
    fig.tight_layout(rect=[0, 0, 1, 0.88])
    fig.savefig(os.path.join(outdir, 'fig4_stall_breakdown.png'))
    fig.savefig(os.path.join(outdir, 'fig4_stall_breakdown.pdf'))
    plt.close(fig)
    print("  Fig 4: Pipeline Stall Breakdown")


# ── Fig 5: MXU vs Vanilla Regression (grouped bar + hatching) ───────

def plot_regression(data, sizes, outdir):
    """Fig 5: MXU vs Vanilla Rocket — MIMD regression test."""
    fig, ax = plt.subplots(figsize=(8, 5.5))
    x = np.arange(len(sizes))
    width = 0.35

    mxu_vals = [int(data.get(('gemm_mimd', s, 'mxu'), {}).get('cycles', 0)) for s in sizes]
    van_vals = [int(data.get(('gemm_mimd', s, 'vanilla'), {}).get('cycles', 0)) for s in sizes]

    mxu_bars = ax.bar(x - width/2, mxu_vals, width, label='MIMD SGEMM on MXU Rocket',
                      color='#4A90D9', edgecolor='black', linewidth=0.5)
    van_bars = ax.bar(x + width/2, van_vals, width, label='MIMD SGEMM on Baseline Rocket',
                      color='#E74C3C', hatch='//', edgecolor='black', linewidth=0.5)

    # Value labels on each bar -- single centered label when values are equal
    for mbar, vbar, m, v in zip(mxu_bars, van_bars, mxu_vals, van_vals):
        if m > 0 and v > 0 and m == v:
            cx = (mbar.get_x() + mbar.get_width() + vbar.get_x()) / 2
            ax.text(cx, m, f'{m:,}', ha='center', va='bottom', fontsize=10)
        else:
            if m > 0:
                ax.text(mbar.get_x() + mbar.get_width()/2, mbar.get_height(),
                        f'{m:,}', ha='center', va='bottom', fontsize=10)
            if v > 0:
                ax.text(vbar.get_x() + vbar.get_width()/2, vbar.get_height(),
                        f'{v:,}', ha='center', va='bottom', fontsize=10)

    # Percentage difference annotation — placed above both bars
    for xi, (m, v) in enumerate(zip(mxu_vals, van_vals)):
        if m > 0 and v > 0:
            pct = (m - v) / v * 100
            top = max(m, v)
            if abs(pct) < 0.05:
                label = '0% $\Delta$'  # delta symbol via mathtext
                fc = '#d4edda'  # light green
            else:
                label = f'{pct:+.1f}%'
                fc = '#f8d7da' if pct > 0 else '#d4edda'
            ax.annotate(label, (xi, top),
                        textcoords='offset points', xytext=(0, 18),
                        ha='center', fontsize=11, fontweight='bold',
                        bbox=dict(boxstyle='round,pad=0.2', facecolor=fc,
                                  edgecolor='gray', alpha=0.9))

    ax.set_xlabel('Matrix Size (N\u00d7N)', fontsize=13)
    ax.set_ylabel('Cycles', fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels([str(s) for s in sizes], fontsize=11)
    ax.tick_params(axis='y', labelsize=11)
    ax.legend(fontsize=10)
    # Add headroom for annotations above tallest bar
    ymax = max(max(mxu_vals), max(van_vals))
    ax.set_ylim(top=ymax * 1.15)
    ax.set_title('MIMD Regression: MXU vs Baseline Rocket (lower is better)', fontsize=15)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig5_regression.png'))
    fig.savefig(os.path.join(outdir, 'fig5_regression.pdf'))
    plt.close(fig)
    print("  Fig 5: MXU vs Vanilla Regression")


# ── Summary Table ────────────────────────────────────────────────────

def print_summary_table(data, sizes):
    """Print a summary table to terminal."""
    print("\n" + "="*90)
    print(f"{'Benchmark':<20} {'N':>4} {'Cycles':>8} {'Instret':>8} {'D$Miss':>7} {'SysStl':>7} {'LoadUse':>8} {'CSRStl':>7} {'FP-FMA':>7}")
    print("-"*90)
    for bench in BENCH_ORDER:
        for s in sizes:
            d = data.get((bench, s, 'mxu'), {})
            if d:
                print(f"{SERIES[bench]['label']:<20} {s:>4} {int(d.get('cycles',0)):>8} "
                      f"{int(d.get('instret',0)):>8} {int(d.get('dcache_miss',0)):>7} "
                      f"{int(d.get('systolic_stall',0)):>7} "
                      f"{int(d.get('load_use',0)):>8} {int(d.get('csr_interlock',0)):>7} "
                      f"{int(d.get('fp_muladd',0)):>7}")
    print("="*90)


# ── Main ─────────────────────────────────────────────────────────────

def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else 'results.csv'
    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found. Run run_all_benchmarks.sh first.")
        sys.exit(1)

    outdir = os.path.join(os.path.dirname(csv_path), 'plots')
    os.makedirs(outdir, exist_ok=True)

    data = load_csv(csv_path)
    sizes = sorted(set(int(d['N']) for d in data.values() if isinstance(d.get('N'), (int, str)) and str(d.get('N','')).isdigit()))
    if not sizes:
        sizes = [4, 8, 16]

    print(f"Generating plots from {csv_path} → {outdir}/")

    plot_cycles(data, sizes, outdir)
    plot_mflops(data, sizes, outdir)
    plot_dcache_miss(data, sizes, outdir)
    plot_stall_breakdown(data, sizes, outdir)
    plot_regression(data, sizes, outdir)

    print_summary_table(data, sizes)
    print(f"\nAll plots saved to {outdir}/")

if __name__ == '__main__':
    main()
