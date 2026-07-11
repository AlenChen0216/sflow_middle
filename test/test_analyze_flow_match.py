import json
import socket
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))

import analyze_flow_match as analyzer  # noqa: E402


def output_flow(agents=None):
    if agents is None:
        agents = [
            {
                "aip": 1,
                "index": 10,
                "in_if": 2,
                "out_if": 0,
                "byte_cnt": 10,
                "packet_cnt": 2,
                "sampling_rate": 100,
            },
            {
                "aip": 2,
                "index": 20,
                "in_if": 3,
                "out_if": 0,
                "byte_cnt": 14,
                "packet_cnt": 4,
                "sampling_rate": 100,
            },
        ]
    return {
        "src_ip": 0xC0A8020B,
        "src_port": 5204,
        "dst_ip": 0xC0A80298,
        "dst_port": 44505,
        "protocol": 6,
        "agent": agents,
    }


def ground_flow(byte_total=1200, packet_total=300):
    return {
        "src_ip": socket.ntohl(0xC0A8020B),
        "src_port": 5204,
        "dst_ip": socket.ntohl(0xC0A80298),
        "dst_port": 44505,
        "protocol_id": 6,
        "estimated_flow_sending_rate_bps_in_the_last_sec": byte_total * 8,
        "estimated_packet_rate_in_the_last_sec": packet_total,
    }


class FlowMatchTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name)

    def write_jsonl(self, name, records, blank_line=False):
        path = self.root / name
        with path.open("w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record) + "\n")
                if blank_line:
                    handle.write("\n")
        return path

    def test_sample_files_are_valid_json_lines(self):
        output_records = list(
            analyzer.load_json_lines(PROJECT_ROOT / "sample_output.json")
        )
        ground_records = list(
            analyzer.load_json_lines(PROJECT_ROOT / "sample_flowinfo.json")
        )
        self.assertEqual(len(output_records), 2)
        self.assertEqual(len(ground_records), 2)

    def test_ip_conversion_sampling_and_agent_average(self):
        output_path = self.write_jsonl(
            "output.json", [{"flowMap": [output_flow()]}], blank_line=True
        )
        ground_path = self.write_jsonl(
            "ground.json", [{"flowinfo": [ground_flow()]}]
        )

        output = analyzer.aggregate_output(output_path)
        ground = analyzer.aggregate_ground_truth(ground_path)
        result = analyzer.compare_totals(output, ground, tolerance=0)

        key = next(iter(output))
        self.assertEqual(key.src_ip, socket.ntohl(0xC0A8020B))
        self.assertEqual(output[key], analyzer.Totals(1200, 300))
        self.assertTrue(result.matches)

    def test_agents_are_accumulated_before_averaging(self):
        first = output_flow()
        second = output_flow()
        output_path = self.write_jsonl(
            "output.json", [{"flowMap": [first]}, {"flowMap": [second]}]
        )
        ground_path = self.write_jsonl(
            "ground.json", [{"flowinfo": [ground_flow(2400, 600)]}]
        )

        result = analyzer.compare_totals(
            analyzer.aggregate_output(output_path),
            analyzer.aggregate_ground_truth(ground_path),
            tolerance=0,
        )
        self.assertTrue(result.matches)

    def test_tolerance_boundary_and_mismatch(self):
        key = analyzer.FlowKey(1, 2, 3, 4, 6)
        ground = {key: analyzer.Totals(100, 100)}
        at_boundary = {key: analyzer.Totals(120, 80)}
        outside = {key: analyzer.Totals(121, 79)}

        self.assertTrue(analyzer.compare_totals(at_boundary, ground, 0.20).matches)
        result = analyzer.compare_totals(outside, ground, 0.20)
        self.assertEqual(len(result.byte_mismatches), 1)
        self.assertEqual(len(result.packet_mismatches), 1)

    def test_missing_flow_and_extra_ground_flow(self):
        output_key = analyzer.FlowKey(1, 2, 3, 4, 6)
        other_key = analyzer.FlowKey(5, 6, 7, 8, 17)
        totals = analyzer.Totals(10, 10)

        missing = analyzer.compare_totals(
            {output_key: totals}, {other_key: totals}, 0.20
        )
        self.assertEqual(missing.missing_flows, [output_key])
        self.assertFalse(missing.matches)

        extra = analyzer.compare_totals(
            {output_key: totals}, {output_key: totals, other_key: totals}, 0.20
        )
        self.assertTrue(extra.matches)

    def test_zero_ground_total_requires_exact_zero(self):
        key = analyzer.FlowKey(1, 2, 3, 4, 6)
        ground = {key: analyzer.Totals(0, 0)}
        self.assertTrue(
            analyzer.compare_totals({key: analyzer.Totals(0, 0)}, ground, 1).matches
        )
        self.assertFalse(
            analyzer.compare_totals({key: analyzer.Totals(1, 0)}, ground, 1).matches
        )

    def test_ground_uses_last_second_packet_field(self):
        flow = ground_flow(packet_total=321)
        flow["estimated_packet_rate_in_the_proceeding_1sec_timeslot"] = 999999
        path = self.write_jsonl("ground.json", [{"flowinfo": [flow]}])

        totals = next(iter(analyzer.aggregate_ground_truth(path).values()))
        self.assertEqual(totals.packet_count, 321)

    def test_malformed_json_returns_input_error_exit_code(self):
        output_path = self.root / "bad.json"
        output_path.write_text("{not-json}\n", encoding="utf-8")
        ground_path = self.write_jsonl(
            "ground.json", [{"flowinfo": [ground_flow()]}]
        )
        with redirect_stderr(StringIO()):
            status = analyzer.main([str(output_path), str(ground_path)])
        self.assertEqual(status, 2)

    def test_cli_match_and_mismatch_exit_codes(self):
        output_path = self.write_jsonl(
            "output.json", [{"flowMap": [output_flow()]}]
        )
        matching_ground = self.write_jsonl(
            "matching.json", [{"flowinfo": [ground_flow()]}]
        )
        mismatching_ground = self.write_jsonl(
            "mismatching.json", [{"flowinfo": [ground_flow(byte_total=1)]}]
        )

        with redirect_stdout(StringIO()):
            match_status = analyzer.main(
                [str(output_path), str(matching_ground), "--tolerance", "0"]
            )
            mismatch_status = analyzer.main(
                [str(output_path), str(mismatching_ground), "--tolerance", "0"]
            )
        self.assertEqual(match_status, 0)
        self.assertEqual(mismatch_status, 1)


if __name__ == "__main__":
    unittest.main()
