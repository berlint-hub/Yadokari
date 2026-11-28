#!/usr/bin/env python3
"""Averages the [Perf] summaries of one or more emulog.txt files, so runs of different builds can be compared.

    switch/tools/logs/perf_summary.py old/emulog.txt new/emulog.txt
    switch/tools/logs/perf_summary.py --limiter VU1 --list emulog.txt

Prints, over the selected 5-second intervals (by default the ones whose limiter is VU1): speed, ms per frame of
the EE, VU1 and GS threads and the GPU, GS work per vsync, and the CPU clock when the log has it. Scenes differ
between runs, so also compare per-primitive costs (VU1 us per prim) and look at the individual intervals.
"""
import argparse
import re


def parse(path):
    intervals = []
    cur = None
    for line in open(path, errors="replace"):
        m = re.search(r"\[Perf\] (\d+)-(\d+)s(?: \(paused [\d.]+s\))?: speed (\d+)% \| ([\d.]+) fps \(game (\d+)\)"
                      r" \| frame avg ([\d.]+) max ([\d.]+) ms \| limiter (.*?)(?: \| CPU (.*))?$", line.rstrip())
        if m:
            cur = dict(t0=int(m[1]), t1=int(m[2]), speed=int(m[3]), fps=float(m[4]), game=int(m[5]),
                       frame_avg=float(m[6]), frame_max=float(m[7]), limiter=m[8], cpu=m[9] or "")
            intervals.append(cur)
            continue
        if cur is None:
            continue
        m = re.search(r"EE (\d+)% ([\d.]+)ms/f .*?VU1 (\d+)% ([\d.]+)ms/f \| GS (\d+)% ([\d.]+)ms/f"
                      r".*?(?:GPU ([\d.]+)ms/f)?$", line.rstrip())
        if m and "[Perf]   EE" in line:
            cur.update(ee_pct=int(m[1]), ee=float(m[2]), vu1_pct=int(m[3]), vu1=float(m[4]), gs_pct=int(m[5]),
                       gs=float(m[6]), gpu=float(m[7] or 0))
        m = re.search(r"GS per vsync: (\d+) draws (\d+) prims (\d+) draw calls ([\d.]+) passes ([\d.]+) barriers"
                      r" ([\d.]+) uploads", line)
        if m:
            cur.update(draws=int(m[1]), prims=int(m[2]), calls=int(m[3]), barriers=float(m[5]), uploads=float(m[6]))
    return [x for x in intervals if "vu1" in x and "prims" in x]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--limiter", default="VU1", help="only intervals whose limiter starts with this ('' = all)")
    ap.add_argument("--list", action="store_true", help="also print every selected interval")
    args = ap.parse_args()

    for path in args.logs:
        everything = parse(path)
        sel = [x for x in everything if x["limiter"].startswith(args.limiter)]
        print(f"{path}: {len(everything)} intervals, {len(sel)} with limiter '{args.limiter}'")
        if not sel:
            continue
        n = len(sel)
        avg = lambda k: sum(x[k] for x in sel) / n
        print(f"  speed {avg('speed'):.1f}% | EE {avg('ee'):.1f} VU1 {avg('vu1'):.1f} ({avg('vu1_pct'):.0f}%) "
              f"GS {avg('gs'):.1f} ({avg('gs_pct'):.0f}%) GPU {avg('gpu'):.1f} ms/f")
        print(f"  per vsync: {avg('prims'):.0f} prims, {avg('draws'):.0f} draws, {avg('calls'):.0f} draw calls, "
              f"{avg('barriers'):.0f} barriers, {avg('uploads'):.1f} uploads")
        vu1_s = sum(x["vu1"] * x["fps"] for x in sel)
        prims = sum(x["prims"] * x["fps"] for x in sel)
        if prims:
            print(f"  VU1 {1000.0 * vu1_s / prims:.3f} us per prim")
        clocks = sorted({x["cpu"] for x in sel if x["cpu"]})
        if clocks:
            print(f"  CPU clock: {', '.join(clocks)}")
        if args.list:
            for x in sel:
                print(f"    {x['t0']}-{x['t1']}s speed {x['speed']}% VU1 {x['vu1']} GS {x['gs']} ms/f "
                      f"{x['prims']} prims {x['calls']} calls | {x['limiter']} {x['cpu']}")


if __name__ == "__main__":
    main()
