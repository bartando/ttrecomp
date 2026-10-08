#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize GPU-thread CPU samples (gpu_cpu_sample_file) by function.

    python3 ps5/tools/sample_report.py out/ps5-port/ab/tt-samples.bin \\
        out/ps5-port/game-title/llvm-pie.elf [top] [last_n_samples]

Each record is rip plus 8 stack words that looked like code addresses. "self"
counts the function at rip; "incl" counts every distinct function in the
record, so it over-counts where stale return addresses linger on the stack.
"""
import bisect
import collections
import struct
import subprocess
import sys

LOAD_BASE = 0x400000
NM = "/opt/homebrew/opt/llvm@21/bin/llvm-nm"
CXXFILT = "/opt/homebrew/opt/llvm@21/bin/llvm-cxxfilt"
STACK_WORDS = 8
RECORD = struct.Struct("<%dQ" % (1 + STACK_WORDS))


def symbols(elf):
    out = subprocess.run([NM, "--defined-only", "-n", elf], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def demangle(names):
    out = subprocess.run([CXXFILT], input="\n".join(names), capture_output=True, text=True,
                         check=True).stdout.splitlines()
    return dict(zip(names, out))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    # Optional: only the last N samples (the file is in time order).
    if len(sys.argv) > 4:
        data = data[-int(sys.argv[4]) * RECORD.size:]
    addrs, names = symbols(sys.argv[2])
    if not addrs:
        sys.exit("no code symbols")

    text_end = LOAD_BASE + addrs[-1] + 0x1000

    def name_of(address):
        if not LOAD_BASE <= address < text_end:
            return "[system]"
        i = bisect.bisect_right(addrs, address - LOAD_BASE) - 1
        return names[i] if i >= 0 else f"?{address:#x}"

    def leaf_of(record):
        # Time in system libraries (libkernel syscalls, libc.prx) is charged
        # to the first game function found on the stack.
        if LOAD_BASE <= record[0] < text_end:
            return name_of(record[0])
        callers = [a for a in record[1:] if LOAD_BASE <= a < text_end]
        return "[system] <- " + (name_of(callers[0]) if callers else "?")

    self_counts = collections.Counter()
    incl_counts = collections.Counter()
    total = 0
    for record in RECORD.iter_unpack(data[:len(data) - len(data) % RECORD.size]):
        if not record[0]:
            continue
        total += 1
        leaf = leaf_of(record)
        self_counts[leaf] += 1
        incl_counts.update({name_of(a) for a in record if a} - {"[system]"} | {leaf})
    if not total:
        sys.exit("no samples")
    plain = {n.split(" <- ")[-1] for n in set(self_counts) | set(incl_counts)}
    pretty = demangle(sorted(plain))
    pretty = {n: (n.split(" <- ")[0] + " <- " if " <- " in n else "") +
              pretty.get(n.split(" <- ")[-1], n.split(" <- ")[-1])
              for n in set(self_counts) | set(incl_counts)}
    print(f"{total} samples")
    # Optional: callers of functions whose name contains this text.
    focus = sys.argv[5] if len(sys.argv) > 5 else None
    if focus:
        chains = collections.Counter()
        for record in RECORD.iter_unpack(data[:len(data) - len(data) % RECORD.size]):
            frames = [name_of(a) for a in record if a]
            game = [n for n in frames if n != "[system]"]
            hit = next((i for i, n in enumerate(game) if focus in n), None)
            if hit is not None:
                chains[tuple(game[hit:hit + 5])] += 1
        flat = sorted({n for c in chains for n in c})
        pretty_names = dict(zip(flat, demangle(flat)))
        print(f"\n== callers of *{focus}*")
        for chain, count in chains.most_common(8):
            print(f"{100 * count / total:5.1f}%  " +
                  " <- ".join(pretty_names.get(n, n).split("(")[0][-60:] for n in chain))
    for title, counts in (("self", self_counts), ("incl", incl_counts)):
        print(f"\n== {title}")
        for name, count in counts.most_common(top):
            print(f"{100 * count / total:5.1f}%  {pretty.get(name, name)[:150]}")


if __name__ == "__main__":
    main()
