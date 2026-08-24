#!/usr/bin/env python3

import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from trace_stream import TraceValidationError, open_trace_input, validate_stream


VALID_DOCUMENT = {
    "traces": [
        {
            "agentId": 7,
            "agentType": "worker",
            "hostId": 17,
            "tasks": [
                {
                    "taskSequence": 3,
                    "operations": [
                        {
                            "opId": 0,
                            "startOffsetMs": 1000.0,
                            "durationMs": 200.5,
                            "uplinkBytes": 80,
                            "downlinkBytes": 40,
                            "depend": [],
                        },
                        {
                            "opId": 1,
                            "startOffsetMs": 1200.5,
                            "durationMs": 10.0,
                            "uplinkBytes": 0,
                            "downlinkBytes": 0,
                            "depend": [0],
                        },
                    ],
                }
            ],
        }
    ]
}


class TraceValidationTest(unittest.TestCase):
    def make_stream(self, document=VALID_DOCUMENT):
        return io.BytesIO(json.dumps(document).encode("utf-8"))

    def test_summarizes_valid_trace(self):
        summary = validate_stream(self.make_stream())

        self.assertEqual(summary.trace_count, 1)
        self.assertEqual(summary.operation_count, 2)
        self.assertEqual(summary.network_operation_count, 1)
        self.assertEqual(summary.total_network_bytes, 120)
        self.assertEqual(summary.earliest_network_start_ms, 1000.0)
        self.assertEqual(summary.maximum_operation_end_ms, 1210.5)

    def test_rejects_missing_duration(self):
        document = json.loads(json.dumps(VALID_DOCUMENT))
        del document["traces"][0]["tasks"][0]["operations"][0]["durationMs"]

        with self.assertRaisesRegex(TraceValidationError, "durationMs"):
            validate_stream(self.make_stream(document))

    def test_regular_input_can_be_reopened(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.json"
            path.write_text(json.dumps(VALID_DOCUMENT), encoding="utf-8")

            with open_trace_input(path) as first:
                first_summary = validate_stream(first)
            with open_trace_input(path) as second:
                second_summary = validate_stream(second)

            self.assertEqual(first_summary, second_summary)


if __name__ == "__main__":
    unittest.main()
