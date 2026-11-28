#!/usr/bin/env python3
"""Disassembles and classifies the "hottest JIT code" of a profile.txt written by HorizonProfiler.

    switch/tools/logs/jit_hot.py profile.txt                      # VU1 instruction mix over the whole file
    switch/tools/logs/jit_hot.py profile.txt --thread EE --mix
    switch/tools/logs/jit_hot.py profile.txt --windows 3          # disassemble the 3 hottest windows
    switch/tools/logs/jit_hot.py profile.txt --section 120-150s --windows 2

profile.txt lists, per thread, the hottest 256-byte windows of JIT code as raw A64 instruction words with the
share of samples per 32-byte line. This turns them back into instructions (aarch64-linux-gnu-objdump) and
counts categories: clamps (FMINNM/FMAXNM against q27/q28), accesses through x27 (cpuRegistersPack: VU/EE
registers) and x28 (vuRegistersPack in VU code), large-offset address building (MOVZ/MOVK into x16/x17),
float arithmetic, flag logic, shuffles, moves and branches. "weighted" uses the line's sample share.
"""
import argparse
import collections
import os
import re
import struct
import subprocess
import tempfile


def read_windows(path, thread, section):
    windows = []
    cur_thread = None
    in_hot = False
    in_section = section is None
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        m = re.match(r"=+ (.*?) \(", line)
        if m:
            in_section = section is None or m[1] == section
            cur_thread = None
            continue
        m = re.match(r"--- (\w+) thread", line)
        if m:
            cur_thread = m[1]
            in_hot = False
            continue
        if "hottest JIT code" in line:
            in_hot = True
            continue
        if not (in_section and in_hot and cur_thread == thread):
            continue
        m = re.match(r"\s+([\d.]+)\s+\[(\w+) JIT\]\+0x([0-9a-f]+)", line)
        if m:
            windows.append(dict(pct=float(m[1]), kind=m[2], base=int(m[3], 16), lines=[]))
            continue
        m = re.match(r"\s+\*?\s*\+0x([0-9a-f]+)\s+([\d.]+)%: (.*)", line)
        if m and windows:
            words = [int(w, 16) for w in m[3].split() if re.fullmatch(r"[0-9a-f]{8}", w)]
            windows[-1]["lines"].append((int(m[1], 16), float(m[2]), words))
    return windows


def disassemble(words, vma=0):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"".join(struct.pack("<I", w) for w in words))
        name = f.name
    try:
        out = subprocess.run(["aarch64-linux-gnu-objdump", "-D", "-b", "binary", "-m", "aarch64",
                              f"--adjust-vma=0x{vma:x}", name], capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(name)
    result = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3 and re.match(r"\s+[0-9a-f]+:$", parts[0]):
            result.append((int(parts[0].strip()[:-1], 16), parts[2].strip(), parts[3] if len(parts) > 3 else ""))
    return result


MEM = {"ldr", "str", "ldur", "stur", "ldp", "stp", "ldrh", "strh", "ldrsh", "ldrsw", "ldrb", "strb", "ld1", "st1",
       "ld1r"}
FP = {"fmul", "fadd", "fsub", "fmla", "fmls", "fdiv", "fsqrt", "fabs", "fneg", "fmax", "fmin", "fcvtzs", "scvtf",
      "ucvtf", "fmadd", "fmsub", "frecpe", "frsqrte"}
VLOGIC = {"cmlt", "fcmeq", "fcmlt", "fcmgt", "fcmge", "sli", "addv", "fmov", "umov", "ushr", "usra", "sshr", "xtn",
          "cmeq", "cmgt", "bif", "bit", "bsl", "and", "orn", "bic", "eor", "orr"}
SHUF = {"ins", "dup", "ext", "zip1", "zip2", "uzp1", "uzp2", "trn1", "trn2", "rev64", "tbl"}


def classify(op, args):
    first = args.split(",")[0].strip()
    if op in ("fminnm", "fmaxnm") and re.search(r"\b[vsq]2[78]\b", args):
        return "clamp"
    if op in MEM:
        if "[x27" in args:
            return "mem x27"
        if "[x28" in args:
            return "mem x28"
        return "mem other"
    if op in ("mov", "movk", "movz") and re.match(r"[xw]1[67]$", first):
        return "addr constant (x16/x17)"
    if op == "mov" and first.startswith("v"):
        return "vector mov"
    if op in FP:
        return "fp arith"
    if op in VLOGIC and first.startswith(("v", "d", "s")):
        return "vector logic / flags"
    if op in SHUF or (op == "mov" and first.startswith("v")):
        return "vector shuffle / insert"
    if op.startswith("b") or op in ("cbz", "cbnz", "tbz", "tbnz", "ret", "blr", "br"):
        return "branch"
    if op == ".inst":
        return "data / padding"
    return "general register ops"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--thread", default="VU1", help="EE, VU1 or GS (default VU1)")
    ap.add_argument("--section", help="only this report section, e.g. '120-150s' or 'Whole session'")
    ap.add_argument("--windows", type=int, default=0, help="disassemble the N hottest windows")
    ap.add_argument("--mix", action="store_true", help="print the instruction mix (default if --windows is 0)")
    args = ap.parse_args()

    windows = read_windows(args.profile, args.thread, args.section)
    if not windows:
        print("no hot JIT windows found for", args.thread)
        return

    if args.windows:
        for w in sorted(windows, key=lambda w: -w["pct"])[:args.windows]:
            print(f"==== [{w['kind']} JIT]+0x{w['base']:x}  {w['pct']:.1f}% of samples")
            for off, pct, words in w["lines"]:
                print(f"  -- +0x{off:x}  {pct:.1f}%")
                for addr, op, a in disassemble(words, off):
                    print(f"     {addr:8x}: {op:8s} {a}")

    if args.mix or not args.windows:
        count = collections.Counter()
        weight = collections.Counter()
        mnemonics = collections.Counter()
        for w in windows:
            for off, pct, words in w["lines"]:
                ins = disassemble(words, off)
                for _, op, a in ins:
                    c = classify(op, a)
                    count[c] += 1
                    weight[c] += pct / max(len(ins), 1)
                    mnemonics[op] += 1
        total = sum(count.values())
        wtotal = sum(weight.values()) or 1.0
        print(f"{args.thread}: {len(windows)} windows, {total} instructions")
        for c, n in count.most_common():
            print(f"  {c:26s} {n:6d} {100.0 * n / total:5.1f}%   weighted {100.0 * weight[c] / wtotal:5.1f}%")
        print("  top mnemonics:", ", ".join(f"{op} {n}" for op, n in mnemonics.most_common(20)))


if __name__ == "__main__":
    main()
