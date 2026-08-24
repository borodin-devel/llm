#!/usr/bin/env python3

import io
import json
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from trace_stream import (
    TraceValidationError,
    find_first_window,
    find_high_load_window,
    open_trace_input,
    validate_stream,
    write_window,
)


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

WINDOW_DOCUMENT = {
    "traces": [
        {
            "agentId": 1,
            "agentType": "worker",
            "metadata": {"keep": True},
            "tasks": [
                {
                    "taskSequence": 1,
                    "arrivalOffsetMs": 100000.0,
                    "operations": [
                        {
                            "opId": 0,
                            "startOffsetMs": 100000.0,
                            "durationMs": 1000.0,
                            "uplinkBytes": 200,
                            "downlinkBytes": 300,
                            "depend": [],
                        },
                        {
                            "opId": 1,
                            "startOffsetMs": 101000.0,
                            "durationMs": 20.0,
                            "uplinkBytes": 0,
                            "downlinkBytes": 0,
                            "depend": [0, 99],
                        },
                        {
                            "opId": 2,
                            "startOffsetMs": 159500.0,
                            "durationMs": 1000.0,
                            "uplinkBytes": 900,
                            "downlinkBytes": 900,
                            "depend": [1],
                        },
                    ],
                }
            ],
        }
    ]
}


def operation(op_id, start_ms, duration_ms, uplink_bytes, downlink_bytes):
    return {
        "opId": op_id,
        "startOffsetMs": start_ms,
        "durationMs": duration_ms,
        "uplinkBytes": uplink_bytes,
        "downlinkBytes": downlink_bytes,
        "depend": [],
    }


def make_document(operations):
    return {
        "traces": [
            {
                "agentId": 1,
                "agentType": "worker",
                "tasks": [{"taskSequence": 1, "operations": operations}],
            }
        ]
    }


@contextmanager
def json_path(document):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "trace.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        yield path


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


class WindowSelectionTest(unittest.TestCase):
    def test_writes_first_active_minute_with_metadata_and_dependencies(self):
        with json_path(WINDOW_DOCUMENT) as source, tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "slice.json"

            window = find_first_window(source, 60000.0)
            summary = write_window(source, output, window)
            sliced = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(window.start_ms, 100000.0)
        self.assertEqual(window.end_ms, 160000.0)
        self.assertEqual(summary.network_bytes, 500)
        self.assertEqual(sliced["traces"][0]["metadata"], {"keep": True})
        task = sliced["traces"][0]["tasks"][0]
        self.assertEqual(task["arrivalOffsetMs"], 0.0)
        self.assertEqual([item["opId"] for item in task["operations"]], [0, 1])
        self.assertEqual(
            [item["startOffsetMs"] for item in task["operations"]], [0.0, 1000.0]
        )
        self.assertEqual(task["operations"][1]["depend"], [0])

    def test_first_window_skips_network_operation_longer_than_window(self):
        document = make_document(
            [
                operation(0, 50000.0, 70000.0, 900, 900),
                operation(1, 100000.0, 1000.0, 200, 300),
            ]
        )

        with json_path(document) as source:
            window = find_first_window(source, 60000.0)

        self.assertEqual(window.start_ms, 100000.0)

    def test_finds_exact_maximum_contained_bytes(self):
        document = make_document(
            [
                operation(0, 0.0, 1000.0, 10, 10),
                operation(1, 200000.0, 1000.0, 200, 300),
                operation(2, 700000.0, 1000.0, 300, 400),
                operation(3, 150000.0, 650000.0, 1000, 1000),
            ]
        )

        with json_path(document) as source:
            window = find_high_load_window(source, 600000.0)

        self.assertEqual(window.start_ms, 101000.0)
        self.assertEqual(window.end_ms, 701000.0)
        self.assertEqual(window.network_bytes, 1200)


if __name__ == "__main__":
    unittest.main()
