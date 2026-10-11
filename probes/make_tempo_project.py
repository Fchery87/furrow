#!/usr/bin/env python3
"""Write a REAPER project for one tempo case from a saved probe project.

The source project must already hold the probe tracks. This replaces its
tempo envelope with the case's points. A point that starts a ramp gets
shape 0, which REAPER 7.82 played as linear in time. Every other point gets
shape 1, which holds its tempo until the next point. A step is two case
points at one beat. The held tempo already ends there, so the project gets
only the second point.
"""

import argparse
from pathlib import Path

import schedule


def tempo_points(points):
    lines = []
    for index, (beat, bpm) in enumerate(points):
        following = points[index + 1] if index + 1 < len(points) else None
        if following is not None and following[0] == beat:
            continue
        ramps = following is not None and following[0] > beat and following[1] != bpm
        seconds = schedule.seconds_at(beat, points[: index + 1])
        lines.append(f"    PT {seconds:.12f} {bpm:.10f} {0 if ramps else 1}")
    return lines


def rewrite(source_text, points):
    out = []
    in_envelope = False
    for line in source_text.split("\r\n"):
        if line.startswith("  TEMPO "):
            parts = line.split()
            line = f"  TEMPO {points[0][1]:g} {' '.join(parts[2:])}"
        if line == "  <TEMPOENVEX":
            in_envelope = True
        elif in_envelope and line.startswith("    PT "):
            continue
        elif in_envelope and line.startswith("    ACT "):
            line = "    ACT 1 -1"
        elif in_envelope and line == "  >":
            out.extend(tempo_points(points))
            in_envelope = False
        out.append(line)
    return "\r\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("case")
    parser.add_argument("source_project")
    parser.add_argument("output_project")
    args = parser.parse_args()
    points = schedule.read_case(args.case)["points"]
    text = Path(args.source_project).read_bytes().decode("utf-8")
    Path(args.output_project).write_bytes(rewrite(text, points).encode("utf-8"))


if __name__ == "__main__":
    main()
