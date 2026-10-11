#!/usr/bin/env python3
"""Build the expected MIDI schedule for one Furrow timing fixture.

The schedule uses project beats, then rounds each beat to a sample.
An exact half sample rounds toward the even integer. Between two
tempo points BPM moves linearly in time, which is what REAPER 7.82 did
for a gradual tempo marker. This script does not read REAPER.
"""

import argparse
import math
from pathlib import Path


def read_records(path):
    records = []
    for raw in Path(path).read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            records.append(line.split())
    return records


def read_pattern(path):
    lanes = []
    header = {}
    for fields in read_records(path):
        key = fields[0]
        if key == "lane":
            lanes.append(
                {
                    "lane": int(fields[1]),
                    "bus": int(fields[3]),
                    "channel": int(fields[5]),
                    "steps": [int(value) for value in fields[7:]],
                }
            )
        elif key == "notes":
            header["notes"] = [int(value) for value in fields[1:]]
        else:
            header[key] = fields[1]
    lanes.sort(key=lambda item: item["lane"])
    return header, lanes


def read_case(path):
    case = {"points": []}
    in_points = False
    for fields in read_records(path):
        if fields[0] == "points":
            in_points = True
        elif in_points:
            case["points"].append((float(fields[0]), float(fields[1])))
        else:
            case[fields[0]] = fields[1]
    case["points"].sort()
    if len(case["points"]) < 1:
        raise ValueError("a tempo map needs at least one point")
    return case


def seconds_into(start_bpm, end_bpm, span, beats):
    """Seconds to cover `beats` of a `span`-beat segment whose BPM moves linearly in time."""
    if beats < 0 or span < 0:
        raise ValueError("tempo spans move forward")
    if span == 0:
        return 0.0
    duration = 120.0 * span / (start_bpm + end_bpm)
    slope = (end_bpm - start_bpm) / duration
    return 120.0 * beats / (start_bpm + math.sqrt(start_bpm**2 + 120.0 * slope * beats))


def seconds_at(beat, points):
    if beat < points[0][0]:
        raise ValueError("beat starts before the first tempo point")
    elapsed = 0.0
    for index, (start, bpm) in enumerate(points):
        if index + 1 == len(points):
            return elapsed + 60.0 * (beat - start) / bpm
        end, next_bpm = points[index + 1]
        if beat <= end:
            return elapsed + seconds_into(bpm, next_bpm, end - start, beat - start)
        elapsed += seconds_into(bpm, next_bpm, end - start, end - start)
    raise ValueError("beat is outside the tempo map")


def round_half_even(value):
    lower = math.floor(value)
    fraction = value - lower
    if fraction < 0.5 - 1e-9:
        return lower
    if fraction > 0.5 + 1e-9:
        return lower + 1
    return lower if lower % 2 == 0 else lower + 1


def schedule(pattern_path, case_path):
    header, lanes = read_pattern(pattern_path)
    case = read_case(case_path)
    sample_rate = int(case["sample_rate_hz"])
    step_beats = float(header["step_beats"])
    gate_beats = float(header["gate_steps"]) * step_beats
    velocity = int(header["velocity"])
    origin = float(case["origin_beat"])
    events = []
    for lane in lanes:
        note = header["notes"][lane["lane"]]
        for index, enabled in enumerate(lane["steps"]):
            if not enabled:
                continue
            for kind, beat in (
                ("on", origin + index * step_beats),
                ("off", origin + index * step_beats + gate_beats),
            ):
                sample = round_half_even(
                    seconds_at(beat, case["points"]) * sample_rate
                )
                order = 0 if kind == "off" else 1
                events.append(
                    (
                        sample,
                        order,
                        lane["lane"],
                        lane["bus"],
                        0,
                        lane["channel"],
                        note,
                        0 if kind == "off" else velocity,
                        kind,
                    )
                )
    events.sort()
    return [
        f"{sample} {order} {lane} {bus} {dest_bus} {channel} {note} {vel} {kind}"
        for sample, order, lane, bus, dest_bus, channel, note, vel, kind in events
    ]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pattern")
    parser.add_argument("case")
    args = parser.parse_args()
    print("\n".join(schedule(args.pattern, args.case)))


if __name__ == "__main__":
    main()
