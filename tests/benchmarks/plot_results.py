#!/usr/bin/env python3
"""
plot_results.py — Thesis-quality plots from benchmark results.

Usage: python3 plot_results.py [results.csv]

Generates 6 publication-quality figures:
  1. Cycles vs Matrix Size (grouped bar)
  2. Speedup over Single-Core (line)
  3. Cycles per FLOP (bar)
  4. D-cache Miss Rate (bar)
  5. Pipeline Stall Breakdown (stacked bar)
  6. MXU vs Vanilla Rocket Regression (bar)
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
    'font.size': 11,
    'axes.titlesize': 13,
    'axes.labelsize': 12,
    'legend.fontsize': 10,
    'figure.dpi': 150,
    'savefig.dpi': 300,
    'axes.grid': True,
    'grid.alpha': 0.3,
})

COLORS = {
    'gemm_single': '#888888',
    'gemm_mimd':   '#4A90D9',
    'gemm_simd':   '#50C878',
    'gemm_csr':    '#F5A623',
}
LABELS = {
    'gemm_single': 'Single-Core',
    'gemm_mimd':   'MIMD (4-core)',
    'gemm_simd':   'SIMD (4-core)',
    'gemm_csr':    'CSR Systolic (4-core)',
}

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

def plot_cycles(data, sizes, outdir):
    """Fig 1: Cycles vs Matrix Size — grouped bar chart."""
    fig, ax = plt.subplots(figsize=(8, 5))
    benchmarks = ['gemm_single', 'gemm_mimd', 'gemm_simd', 'gemm_csr']
    x = np.arange(len(sizes))
    width = 0.2

    for i, bench in enumerate(benchmarks):
        vals = get_vals(data, bench, sizes, field='cycles')
        vals = [int(v) for v in vals]
        bars = ax.bar(x + i * width, vals, width, label=LABELS[bench],
                      color=COLORS[bench], edgecolor='white', linewidth=0.5)
        for bar, v in zip(bars, vals):
            if v > 0:
                ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                        f'{v}', ha='center', va='bottom', fontsize=7)

    ax.set_xlabel('Matrix Size (N×N)')
    ax.set_ylabel('Cycles')
    ax.set_title('GEMM Execution Cycles by Mode')
    ax.set_xticks(x + 1.5 * width)
    ax.set_xticklabels([str(s) for s in sizes])
    ax.set_yscale('log')
    ax.legend()
    fig.savefig(os.path.join(outdir, 'fig1_cycles.png'))
    fig.savefig(os.path.join(outdir, 'fig1_cycles.pdf'))
    plt.close(fig)
    print("  Fig 1: Cycles vs Matrix Size")

def plot_normalized_time(data, sizes, outdir):
    """Fig 2: Normalized Execution Time (relative to MIMD baseline)."""
    fig, ax = plt.subplots(figsize=(7, 5))
    benchmarks = ['gemm_mimd', 'gemm_simd', 'gemm_csr']

    mimd_cycles = get_vals(data, 'gemm_mimd', sizes, field='cycles')
    mimd_cycles = [int(v) if int(v) > 0 else 1 for v in mimd_cycles]

    for bench in benchmarks:
        vals = get_vals(data, bench, sizes, field='cycles')
        vals = [int(v) if int(v) > 0 else 1 for v in vals]
        # Normalized time = cycles / mimd_cycles
        norm_time = [v / m for v, m in zip(vals, mimd_cycles)]
        ax.plot(sizes, norm_time, 'o-', label=LABELS[bench],
                color=COLORS[bench], linewidth=2, markersize=8)

    ax.axhline(y=1, color='gray', linestyle=':', alpha=0.5, label='MIMD Baseline')
    ax.set_ylabel('Execution Time (Normalized to MIMD)')
    ax.set_title('Normalized Execution Time (Lower is Better)')
    ax.set_xticks(sizes)
    ax.legend()
    fig.savefig(os.path.join(outdir, 'fig2_normalized_time.png'))
    fig.savefig(os.path.join(outdir, 'fig2_normalized_time.pdf'))
    plt.close(fig)
    print("  Fig 2: Normalized Time vs MIMD")

def plot_cycles_per_flop(data, sizes, outdir):
    """Fig 3: Cycles per FLOP — compute utilization."""
    fig, ax = plt.subplots(figsize=(8, 5))
    benchmarks = ['gemm_single', 'gemm_mimd', 'gemm_simd', 'gemm_csr']
    x = np.arange(len(sizes))
    width = 0.2

    for i, bench in enumerate(benchmarks):
        vals = get_vals(data, bench, sizes, field='cycles')
        cpf = [int(v) / (2.0 * s**3) if int(v) > 0 else 0 for v, s in zip(vals, sizes)]
        ax.bar(x + i * width, cpf, width, label=LABELS[bench],
               color=COLORS[bench], edgecolor='white', linewidth=0.5)

    ax.set_xlabel('Matrix Size (N×N)')
    ax.set_ylabel('Cycles / FLOP')
    ax.set_title('Compute Utilization (lower = better)')
    ax.set_xticks(x + 1.5 * width)
    ax.set_xticklabels([str(s) for s in sizes])
    ax.legend()
    fig.savefig(os.path.join(outdir, 'fig3_cycles_per_flop.png'))
    fig.savefig(os.path.join(outdir, 'fig3_cycles_per_flop.pdf'))
    plt.close(fig)
    print("  Fig 3: Cycles per FLOP")

def plot_dcache_miss(data, sizes, outdir):
    """Fig 4: D-cache miss count per mode."""
    fig, ax = plt.subplots(figsize=(8, 5))
    benchmarks = ['gemm_single', 'gemm_mimd', 'gemm_simd', 'gemm_csr']
    x = np.arange(len(sizes))
    width = 0.2

    for i, bench in enumerate(benchmarks):
        vals = get_vals(data, bench, sizes, field='dcache_miss')
        vals = [int(v) for v in vals]
        ax.bar(x + i * width, vals, width, label=LABELS[bench],
               color=COLORS[bench], edgecolor='white', linewidth=0.5)

    ax.set_xlabel('Matrix Size (N×N)')
    ax.set_ylabel('D-cache Misses')
    ax.set_title('D-cache Miss Count by Mode')
    ax.set_xticks(x + 1.5 * width)
    ax.set_xticklabels([str(s) for s in sizes])
    ax.legend()
    fig.savefig(os.path.join(outdir, 'fig4_dcache_miss.png'))
    fig.savefig(os.path.join(outdir, 'fig4_dcache_miss.pdf'))
    plt.close(fig)
    print("  Fig 4: D-cache Miss Count")

def plot_stall_breakdown(data, sizes, outdir):
    """Fig 5: Pipeline stall breakdown — stacked bar per mode."""
    fig, axes = plt.subplots(1, len(sizes), figsize=(4 * len(sizes), 5), sharey=False)
    if len(sizes) == 1:
        axes = [axes]
    benchmarks = ['gemm_single', 'gemm_mimd', 'gemm_simd', 'gemm_csr']
    stall_fields = ['load_use', 'dcache_blocked', 'csr_interlock', 'systolic_stall']
    stall_colors = ['#E74C3C', '#3498DB', '#F39C12', '#9B59B6']
    stall_labels = ['Load-Use', 'D$ Blocked', 'CSR Interlock', 'SIMD Stall']

    for ax, sz in zip(axes, sizes):
        x = np.arange(len(benchmarks))
        bottom = np.zeros(len(benchmarks))
        for field, color, label in zip(stall_fields, stall_colors, stall_labels):
            vals = [int(data.get((b, sz, 'mxu'), {}).get(field, 0)) for b in benchmarks]
            ax.bar(x, vals, 0.6, bottom=bottom, color=color, label=label,
                   edgecolor='white', linewidth=0.5)
            bottom += np.array(vals)

        ax.set_title(f'N={sz}')
        ax.set_xticks(x)
        ax.set_xticklabels(['Single', 'MIMD', 'SIMD', 'CSR'], rotation=45, ha='right')

    for ax in axes:
        ax.set_ylabel('Stall Cycles')
    axes[-1].legend(loc='upper right')
    fig.suptitle('Pipeline Stall Breakdown')
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig5_stall_breakdown.png'))
    fig.savefig(os.path.join(outdir, 'fig5_stall_breakdown.pdf'))
    plt.close(fig)
    print("  Fig 5: Pipeline Stall Breakdown")

def plot_regression(data, sizes, outdir):
    """Fig 6: MXU vs Vanilla Rocket — MIMD regression test."""
    fig, ax = plt.subplots(figsize=(7, 5))
    x = np.arange(len(sizes))
    width = 0.35

    mxu_vals = [int(data.get(('gemm_mimd', s, 'mxu'), {}).get('cycles', 0)) for s in sizes]
    van_vals = [int(data.get(('gemm_mimd', s, 'vanilla'), {}).get('cycles', 0)) for s in sizes]

    ax.bar(x - width/2, mxu_vals, width, label='MIMD on MXU Rocket',
           color=COLORS['gemm_mimd'], edgecolor='white')
    ax.bar(x + width/2, van_vals, width, label='MIMD on Vanilla Rocket',
           color='#E74C3C', edgecolor='white')

    for xi, (m, v) in enumerate(zip(mxu_vals, van_vals)):
        if m > 0 and v > 0:
            pct = (m - v) / v * 100
            ax.text(xi, max(m, v) * 1.05, f'{pct:+.1f}%', ha='center', fontsize=9)

    ax.set_xlabel('Matrix Size (N×N)')
    ax.set_ylabel('Cycles')
    ax.set_title('MIMD Regression: MXU vs Vanilla Rocket')
    ax.set_xticks(x)
    ax.set_xticklabels([str(s) for s in sizes])
    ax.legend()
    fig.savefig(os.path.join(outdir, 'fig6_regression.png'))
    fig.savefig(os.path.join(outdir, 'fig6_regression.pdf'))
    plt.close(fig)
    print("  Fig 6: MXU vs Vanilla Regression")

def print_summary_table(data, sizes):
    """Print a summary table to terminal."""
    print("\n" + "="*90)
    print(f"{'Benchmark':<20} {'N':>4} {'Cycles':>8} {'Instret':>8} {'D$Miss':>7} {'SysStl':>7} {'LoadUse':>8} {'CSRStl':>7} {'FP-FMA':>7}")
    print("-"*90)
    for bench in ['gemm_single', 'gemm_mimd', 'gemm_simd', 'gemm_csr']:
        for s in sizes:
            d = data.get((bench, s, 'mxu'), {})
            if d:
                print(f"{LABELS.get(bench, bench):<20} {s:>4} {int(d.get('cycles',0)):>8} "
                      f"{int(d.get('instret',0)):>8} {int(d.get('dcache_miss',0)):>7} "
                      f"{int(d.get('systolic_stall',0)):>7} "
                      f"{int(d.get('load_use',0)):>8} {int(d.get('csr_interlock',0)):>7} "
                      f"{int(d.get('fp_muladd',0)):>7}")
    print("="*90)

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
    plot_normalized_time(data, sizes, outdir)
    plot_cycles_per_flop(data, sizes, outdir)
    plot_dcache_miss(data, sizes, outdir)
    plot_stall_breakdown(data, sizes, outdir)
    plot_regression(data, sizes, outdir)

    print_summary_table(data, sizes)
    print(f"\nAll plots saved to {outdir}/")

if __name__ == '__main__':
    main()
