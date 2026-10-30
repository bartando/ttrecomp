#!/usr/bin/env python3
"""Rank guest functions by how hot they are between two profiler snapshots.

    python3 tools/rank_hot_functions.py out/profile/run/t00060.tsv out/profile/run/t00180.tsv > hot.tsv

Snapshots come from tools/gen_function_profile.py. Score is calls/s * size, so
a big function called a lot outranks a tiny one called slightly more. Names,
sizes and lift status come from the tt-decomp checkout (TT_DECOMP, default
~/PhpstormProjects/tt-decomp): `spec` means a lift-check spec exists for it
(run lift-check to see if it passes), `lifted` only that a marked definition
exists in src/, which can still be a placeholder.
"""
import os
import re
import sys
from pathlib import Path


def read_snapshot(path):
    counts = {}
    for line in Path(path).read_text().splitlines():
        address, count = line.split("\t")
        counts[int(address, 16)] = int(count)
    return counts


def seconds_of(path):
    return int(re.search(r"t(\d+)\.tsv$", str(path)).group(1))


def decomp_symbols(decomp):
    symbols = {}
    pattern = re.compile(r"^(\S+) = \.text:0x([0-9A-Fa-f]{8}); // type:function size:0x([0-9A-Fa-f]+)")
    for line in (decomp / "config/434C4803/symbols.txt").read_text().splitlines():
        match = pattern.match(line)
        if match:
            symbols[int(match.group(2), 16)] = (match.group(1), int(match.group(3), 16))
    return symbols


def decomp_lifted(decomp):
    # tt-decomp's progress counter knows a marked definition from a declaration.
    sys.path.insert(0, str(decomp / "tools"))
    from progress import lifted
    return lifted(decomp)


def decomp_specs(decomp):
    specs = set()
    for path in (decomp / "tools/lift-check/specs").glob("*.py"):
        match = re.search(r"^ADDRESS\s*=\s*0x([0-9A-Fa-f]+)", path.read_text(), re.M)
        if match:
            specs.add(int(match.group(1), 16))
    return specs


def status(address, specs, lifted):
    if address in specs:
        return "spec"
    return "lifted" if address in lifted else "todo"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    start, end = sys.argv[1:]
    window = seconds_of(end) - seconds_of(start)
    before, after = read_snapshot(start), read_snapshot(end)
    decomp = Path(os.environ.get("TT_DECOMP", Path.home() / "PhpstormProjects/tt-decomp"))
    symbols, lifted, specs = decomp_symbols(decomp), decomp_lifted(decomp), decomp_specs(decomp)

    rows = []
    for address, count in after.items():
        calls = count - before.get(address, 0)
        if calls <= 0:
            continue
        name, size = symbols.get(address, (f"sub_{address:08X}", 0))
        rate = calls / window
        rows.append((rate * size, address, name, rate, size, status(address, specs, lifted)))
    rows.sort(reverse=True)

    print("rank\taddress\tname\tcalls_per_s\tsize\tscore\tstatus")
    for rank, (score, address, name, rate, size, state) in enumerate(rows, 1):
        print(f"{rank}\t{address:08X}\t{name}\t{rate:.1f}\t{size}\t{score:.0f}\t{state}")
    todo = sum(1 for row in rows if row[5] == "todo")
    print(f"{len(rows)} functions ran in the {window}s window, {todo} not lifted", file=sys.stderr)


if __name__ == "__main__":
    main()
