#!/usr/bin/env python3
"""Maps the PC-sampling profiler's output ("prof <address> <samples>" lines,
see port/trap.c) to functions: profile.py doom.elf run.log"""
import collections
import re
import subprocess
import sys

elf, log = sys.argv[1], sys.argv[2]
text = open(log, errors="replace").read().replace("\r", "")
blocks = re.findall(r"prof begin (\d+) samples.*?\n(.*?)prof end", text, re.S)
total = sum(int(b[0]) for b in blocks)
hits = collections.Counter()
for _, body in blocks:
    for addr, n in re.findall(r"prof ([0-9a-f]{8}) (\d+)", body):
        hits[addr] += int(n)
addrs = list(hits)
out = subprocess.run(["riscv64-unknown-elf-addr2line", "-f", "-e", elf] + ["0x" + a for a in addrs],
                     capture_output=True, text=True).stdout.split("\n")
funcs = collections.Counter()
for i, a in enumerate(addrs):
    funcs[out[2 * i]] += hits[a]
print(f"{total} samples in {len(blocks)} reports (top 40 buckets each)")
for f, n in funcs.most_common(25):
    print(f"{100 * n / total:5.1f}%  {n:5d}  {f}")
