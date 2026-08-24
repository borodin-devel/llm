#!/usr/bin/env python3

import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from trace_stream import (
    TraceValidationError,
    _WindowEventStore,
    find_first_window,
    find_high_load_window,
    open_trace_input,
    validate_stream,
    write_window,
)


VALID_DOCUMENT = {
    "metadata": {"source": "fixture", "generator": "unit-test"},
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
    "metadata": {"source": "window-fixture", "generator": "unit-test"},
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

    def test_rejects_values_outside_cpp_integer_range(self):
        oversized_agent = json.loads(json.dumps(VALID_DOCUMENT))
        oversized_agent["traces"][0]["agentId"] = 2**31
        with self.assertRaisesRegex(TraceValidationError, "agentId"):
            validate_stream(self.make_stream(oversized_agent))

        for byte_field in ("uplinkBytes", "downlinkBytes"):
            with self.subTest(byte_field=byte_field):
                oversized_bytes = json.loads(json.dumps(VALID_DOCUMENT))
                oversized_bytes["traces"][0]["tasks"][0]["operations"][0][byte_field] = 2**31
                with self.assertRaisesRegex(TraceValidationError, byte_field):
                    validate_stream(self.make_stream(oversized_bytes))

    def test_rejects_nonfinite_derived_operation_end(self):
        document = json.loads(json.dumps(VALID_DOCUMENT))
        operation_value = document["traces"][0]["tasks"][0]["operations"][0]
        operation_value["startOffsetMs"] = 1e308
        operation_value["durationMs"] = 1e308

        with self.assertRaisesRegex(TraceValidationError, "operation end"):
            validate_stream(self.make_stream(document))

    def test_rar_stream_does_not_deadlock_on_large_stderr(self):
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            trace_path = directory_path / "trace.json"
            trace_path.write_text(json.dumps(VALID_DOCUMENT), encoding="utf-8")
            archive_path = directory_path / "input.rar"
            archive_path.touch()

            fake_unrar = directory_path / "unrar"
            fake_unrar.write_text(
                "#!/usr/bin/env python3\n"
                "import os, pathlib, sys\n"
                "if sys.argv[1] == 'lb':\n"
                "    print('trace.json')\n"
                "else:\n"
                "    sys.stderr.write('x' * 262144)\n"
                "    sys.stderr.flush()\n"
                "    sys.stdout.write(pathlib.Path(os.environ['FAKE_RAR_JSON']).read_text())\n",
                encoding="utf-8",
            )
            fake_unrar.chmod(0o755)

            environment = {
                **os.environ,
                "FAKE_RAR_JSON": str(trace_path),
                "PATH": f"{directory}{os.pathsep}{os.environ['PATH']}",
                "PYTHONPATH": str(SCRIPT_DIR),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
            process = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    "from pathlib import Path; "
                    "from trace_stream import validate_path; "
                    "print(validate_path(Path('input.rar')).trace_count)",
                ],
                cwd=directory,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            try:
                stdout, stderr = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
                self.fail("RAR validation deadlocked while unrar stderr filled")

            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(stdout.strip(), "1")


class WindowSelectionTest(unittest.TestCase):
    def test_window_event_store_aggregates_boundaries_and_cleans_up(self):
        with _WindowEventStore() as events:
            database_path = events.path
            events.add_interval(0.0, 10.0, 20)
            events.add_interval(5.0, 15.0, 30)
            events.add_interval(10.0, 10.0, 5)

            self.assertEqual(events.find_best(), (10.0, 55))
            self.assertTrue(database_path.is_file())

        self.assertFalse(database_path.exists())

    def test_writes_first_active_minute_with_metadata_and_dependencies(self):
        with json_path(WINDOW_DOCUMENT) as source, tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "slice.json"

            window = find_first_window(source, 60000.0)
            summary = write_window(source, output, window)
            sliced = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(window.start_ms, 100000.0)
        self.assertEqual(window.end_ms, 160000.0)
        self.assertEqual(summary.network_bytes, 500)
        self.assertEqual(
            sliced["metadata"], {"source": "window-fixture", "generator": "unit-test"}
        )
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


class TraceCliTest(unittest.TestCase):
    def test_cli_writes_first_slice_atomically(self):
        with json_path(WINDOW_DOCUMENT) as source, tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "slice.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT_DIR / "find_window.py"),
                    "slice-first",
                    str(source),
                    str(output),
                    "--window-seconds",
                    "60",
                ],
                check=False,
                capture_output=True,
                text=True,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(output.is_file())
            self.assertIn("window_start_ms=100000.000", completed.stdout)
            self.assertEqual([path.name for path in Path(directory).iterdir()], ["slice.json"])

    def test_validate_accepts_stdin(self):
        completed = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "find_window.py"), "validate", "-"],
            input=json.dumps(VALID_DOCUMENT),
            capture_output=True,
            text=True,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("network_bytes=120", completed.stdout)

    def test_two_pass_command_rejects_stdin(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(SCRIPT_DIR / "find_window.py"),
                "find-window",
                "-",
                "unused.json",
            ],
            input=json.dumps(WINDOW_DOCUMENT),
            capture_output=True,
            text=True,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("requires a reopenable JSON or RAR path", completed.stderr)


if __name__ == "__main__":
    unittest.main()
