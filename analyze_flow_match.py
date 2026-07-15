#!/usr/bin/env python3
"""Compare MID debug flow records with collector flow information."""

import argparse
import json
import socket
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class FlowKey:
    src_ip: int
    src_port: int
    dst_ip: int
    dst_port: int
    protocol: int


@dataclass(frozen=True)
class Totals:
    byte_count: int
    packet_count: int


@dataclass
class ComparisonResult:
    missing_flows: list
    byte_mismatches: list
    packet_mismatches: list

    @property
    def matches(self):
        return not (self.missing_flows or self.byte_mismatches or self.packet_mismatches)


def load_json_lines(path):
    with Path(path).open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if line.strip():
                try:
                    yield json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {error.msg}") from error


def _key(flow, protocol_field, network_byte_order):
    convert_ip = socket.ntohl if network_byte_order else int
    return FlowKey(
        convert_ip(flow["src_ip"]),
        int(flow["src_port"]),
        convert_ip(flow["dst_ip"]),
        int(flow["dst_port"]),
        int(flow[protocol_field]),
    )


def aggregate_output(path):
    totals = defaultdict(lambda: [0, 0])
    for record in load_json_lines(path):
        # `devnum` identifies a batch, but aggregation intentionally combines
        # all configured devices for comparison with the shared collector view.
        for flow in record.get("flowMap", []):
            key = _key(flow, "protocol", network_byte_order=True)
            agents = flow.get("agent", [])
            if not agents:
                continue
            totals[key][0] += sum(int(agent["byte_cnt"]) * int(agent["sampling_rate"]) for agent in agents) // len(agents)
            totals[key][1] += sum(int(agent["packet_cnt"]) * int(agent["sampling_rate"]) for agent in agents) // len(agents)
    return {key: Totals(*value) for key, value in totals.items()}


def aggregate_ground_truth(path):
    totals = defaultdict(lambda: [0, 0])
    for record in load_json_lines(path):
        for flow in record.get("flowinfo", []):
            key = _key(flow, "protocol_id", network_byte_order=False)
            totals[key][0] += int(flow["estimated_flow_sending_rate_bps_in_the_last_sec"]) // 8
            totals[key][1] += int(flow["estimated_packet_rate_in_the_last_sec"])
    return {key: Totals(*value) for key, value in totals.items()}


def _within_tolerance(actual, expected, tolerance):
    return actual == 0 if expected == 0 else abs(actual - expected) <= expected * tolerance


def compare_totals(output, ground, tolerance=0.2):
    missing = sorted(key for key in output if key not in ground)
    byte_mismatches = []
    packet_mismatches = []
    for key, actual in output.items():
        expected = ground.get(key)
        if expected is None:
            continue
        if not _within_tolerance(actual.byte_count, expected.byte_count, tolerance):
            byte_mismatches.append(key)
        if not _within_tolerance(actual.packet_count, expected.packet_count, tolerance):
            packet_mismatches.append(key)
    return ComparisonResult(missing, byte_mismatches, packet_mismatches)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("ground")
    parser.add_argument("--tolerance", type=float, default=0.2)
    args = parser.parse_args(argv)
    try:
        result = compare_totals(
            aggregate_output(args.output), aggregate_ground_truth(args.ground), args.tolerance
        )
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"input error: {error}", file=sys.stderr)
        return 2
    if result.matches:
        print("flow totals match")
        return 0
    print("flow totals do not match")
    return 1


if __name__ == "__main__":
    sys.exit(main())
