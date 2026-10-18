#!/usr/bin/env python3
"""Compare an observed receiver trace with an independent expected schedule."""

import argparse
import sys
from pathlib import Path


EXPECTED_FIELDS = ("sample", "order", "receiver", "lane", "bus", "channel", "note", "velocity", "kind")
CAPTURE_FIELDS = ("sample", "sequence", "receiver", "lane", "bus", "channel", "note", "velocity", "kind")
LEGACY_CAPTURE_FIELDS = ("sample", "order", "lane", "src_bus", "dst_bus", "channel", "note", "velocity", "kind")
IDENTITY_FIELDS = ("receiver", "lane", "bus", "channel", "note", "velocity", "kind")
ORDER_RANK = {"off": 0, "cc": 1, "on": 2}


def load(path):
    rows = []
    fields = None
    for raw in Path(path).read_text().splitlines():
        line = raw.strip()
        if line.startswith("#"):
            names = tuple(line[1:].strip().split())
            if names in (EXPECTED_FIELDS, CAPTURE_FIELDS, LEGACY_CAPTURE_FIELDS):
                fields = names
            continue
        if not line:
            continue
        parts = line.split()
        if fields is None or len(parts) != len(fields):
            raise ValueError(f"{path} has an unsupported or malformed timing row: {line}")
        row = dict(zip(fields, parts))
        for key in ("sample", "order", "sequence", "receiver", "lane", "bus", "src_bus", "dst_bus", "channel", "note", "velocity"):
            if key in row:
                row[key] = int(row[key])
        row["format"] = "legacy" if fields == LEGACY_CAPTURE_FIELDS else (
            "capture" if fields == CAPTURE_FIELDS else "expected"
        )
        rows.append(row)
    if not rows:
        raise ValueError(f"{path} contains no timing events")
    return rows


def _describe(event):
    return (
        f"sample {event['sample']} {event['kind']} lane {event['lane']} "
        f"receiver {event['receiver']} bus {event['bus']} ch {event['channel']} "
        f"note {event['note']} velocity {event['velocity']}"
    )


def compare(expected, captured, tolerance):
    if tolerance not in (0, 1):
        raise ValueError("the documented timing tolerance is at most one sample")
    if any(row.get("format") == "legacy" for row in captured):
        return ["unverifiable: legacy capture discarded observed sequence and receiver identity"]

    problems = []
    matched = {}
    used = set()

    # Match events by complete observable identity first; sample tolerance is checked
    # separately so routing mistakes do not masquerade as timing errors.
    for want in expected:
        candidates = [
            (index, seen) for index, seen in enumerate(captured)
            if index not in used and all(seen[key] == want[key] for key in IDENTITY_FIELDS)
        ]
        if not candidates:
            same_event_type = [seen for seen in captured if seen["note"] == want["note"] and seen["kind"] == want["kind"]]
            if same_event_type:
                seen = same_event_type[0]
                changed = [key for key in IDENTITY_FIELDS if seen[key] != want[key]]
                category = "routing" if set(changed) & {"receiver", "lane", "bus", "channel"} else "event"
                problems.append(
                    f"{category} mismatch in {', '.join(changed)}: expected {_describe(want)}; "
                    f"observed {_describe(seen)}"
                )
            else:
                problems.append(f"missing event: expected {_describe(want)}")
            continue

        in_tolerance = [(index, seen) for index, seen in candidates if abs(seen["sample"] - want["sample"]) <= tolerance]
        if not in_tolerance:
            index, seen = min(candidates, key=lambda pair: abs(pair[1]["sample"] - want["sample"]))
            problems.append(
                f"timing mismatch: expected {_describe(want)}; observed {_describe(seen)} "
                f"(tolerance {tolerance} sample)"
            )
            used.add(index)
            continue

        index, seen = min(in_tolerance, key=lambda pair: (abs(pair[1]["sample"] - want["sample"]), pair[1]["sequence"]))
        used.add(index)
        matched[id(want)] = seen

    for index, seen in enumerate(captured):
        if index not in used:
            problems.append(f"extra event: observed {_describe(seen)}")

    # The spec defines only the event-class order at a shared offset: note-off,
    # controllers, note-on. It does not define ordering within a class.
    by_receiver = {}
    for want in expected:
        seen = matched.get(id(want))
        if seen is not None:
            by_receiver.setdefault(want["receiver"], []).append((want, seen))
    for receiver, pairs in by_receiver.items():
        for index, (left_want, left_seen) in enumerate(pairs):
            for right_want, right_seen in pairs[index + 1:]:
                if left_seen["sample"] != right_seen["sample"]:
                    continue
                left_rank = ORDER_RANK[left_want["kind"]]
                right_rank = ORDER_RANK[right_want["kind"]]
                if left_rank == right_rank:
                    continue
                earlier_seen, later_seen = (left_seen, right_seen) if left_rank < right_rank else (right_seen, left_seen)
                if earlier_seen["sequence"] >= later_seen["sequence"]:
                    earlier_want, later_want = (left_want, right_want) if left_rank < right_rank else (right_want, left_want)
                    problems.append(
                        f"order mismatch at receiver {receiver}: expected {_describe(earlier_want)} "
                        f"before {_describe(later_want)}; observed {_describe(later_seen)} "
                        f"at sequence {later_seen['sequence']} before "
                        f"{_describe(earlier_seen)} at sequence {earlier_seen['sequence']}"
                    )

    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("expected")
    parser.add_argument("captured", nargs="?")
    parser.add_argument("--tolerance", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    if not args.captured:
        print("no captured log")
        return 2
    try:
        expected = load(args.expected)
        captured = load(args.captured)
    except (OSError, ValueError) as error:
        print(f"invalid timing evidence: {error}")
        return 2
    if captured[0]["format"] == "legacy":
        print(f"{Path(args.expected).stem}: unverifiable legacy capture; rerun with the sequenced receiver logger")
        return 2
    if expected[0]["format"] != "expected" or captured[0]["format"] != "capture":
        print("expected schedule and sequenced receiver capture formats are required")
        return 2
    problems = compare(expected, captured, args.tolerance)
    if problems:
        print("\n".join(f"{Path(args.expected).stem}: {problem}" for problem in problems))
        return 1
    print(f"{len(expected)} events match within {args.tolerance} sample; routing and documented event order agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
