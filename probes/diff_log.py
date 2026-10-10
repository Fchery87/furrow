#!/usr/bin/env python3
"""Compare one captured receiver log with one expected schedule.

A line matches when the sample is within one of the expected sample and
the bus, destination bus, channel, note, velocity, and kind agree.
Each expected line and each captured line is used once. Extra captured
lines fail. This is the diff the first proof runs after REAPER writes
the log. Running it with no log only reports that the capture is absent.
"""

import argparse
import sys
from pathlib import Path


FIELDS = ("sample", "order", "lane", "src_bus", "dst_bus", "channel", "note", "velocity", "kind")


def load(path):
    rows = []
    for raw in Path(path).read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != len(FIELDS):
            raise ValueError(f"{path} has a line with {len(parts)} fields: {line}")
        row = dict(zip(FIELDS, parts))
        row["sample"] = int(row["sample"])
        rows.append(row)
    return rows


def compare(expected, captured, tolerance):
    problems = []
    used = set()
    for index, want in enumerate(expected):
        match = None
        for seen_index, seen in enumerate(captured):
            if seen_index in used:
                continue
            if abs(seen["sample"] - want["sample"]) > tolerance:
                continue
            if all(seen[key] == want[key] for key in FIELDS if key not in ("sample", "order", "lane")):
                match = seen_index
                break
        if match is None:
            problems.append(f"missing {want['sample']} {want['kind']} note {want['note']} ch {want['channel']}")
        else:
            used.add(match)
    for seen_index, seen in enumerate(captured):
        if seen_index not in used:
            problems.append(
                f"extra {seen['sample']} {seen['kind']} note {seen['note']} ch {seen['channel']}"
            )
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("expected")
    parser.add_argument("captured", nargs="?")
    parser.add_argument("--tolerance", type=int, default=1)
    args = parser.parse_args()
    if not args.captured:
        print("no captured log")
        return 2
    problems = compare(load(args.expected), load(args.captured), args.tolerance)
    if problems:
        print("\n".join(problems))
        return 1
    print(f"{len(load(args.expected))} events match within {args.tolerance} sample")
    return 0


if __name__ == "__main__":
    sys.exit(main())
