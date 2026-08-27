"""Linux process-tree resource measurement and publication tests."""

from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
import unittest

from saturated_tcp_benchmark.resources import (
    AttemptResourceUsage,
    MemorySnapshot,
    ProcessTreeResourceMonitor,
    ResourceError,
    detect_resource_capability,
    process_tree_rss_bytes,
    publish_json_exclusive,
    read_memory_snapshot,
)


def _write_process(
    proc_root: Path,
    process_id: int,
    *,
    rss_kb: int | None,
    children_by_thread: dict[int, str] | None = None,
) -> None:
    process_directory = proc_root / str(process_id)
    process_directory.mkdir()
    status = "Name:\ttest\n"
    if rss_kb is not None:
        status += f"VmRSS:\t{rss_kb} kB\n"
    (process_directory / "status").write_text(status, encoding="ascii")
    for thread_id, children in (children_by_thread or {process_id: ""}).items():
        task_directory = process_directory / "task" / str(thread_id)
        task_directory.mkdir(parents=True)
        (task_directory / "children").write_text(children, encoding="ascii")


def _process_group_exists(process_group_id: int) -> bool:
    try:
        os.killpg(process_group_id, 0)
    except ProcessLookupError:
        return False
    return True


class ProcParserTest(unittest.TestCase):
    """Catch unit, traversal, race, and diagnostic errors in proc parsing."""

    def test_reads_literal_memory_fields_in_binary_kilobytes(self) -> None:
        with TemporaryDirectory() as directory:
            meminfo = Path(directory) / "meminfo"
            meminfo.write_text(
                "MemTotal:       16384 kB\nMemFree: 99 kB\nMemAvailable: 4097 kB\n",
                encoding="ascii",
            )

            self.assertEqual(
                read_memory_snapshot(meminfo),
                MemorySnapshot(
                    mem_total_bytes=16_777_216,
                    mem_available_bytes=4_195_328,
                ),
            )

    def test_sums_each_recursive_descendant_once_and_tolerates_vanishing_pid(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                100,
                rss_kb=100,
                children_by_thread={100: "101 102 999", 105: "102"},
            )
            _write_process(proc_root, 101, rss_kb=20, children_by_thread={101: "103"})
            _write_process(proc_root, 102, rss_kb=30, children_by_thread={102: "103"})
            _write_process(proc_root, 103, rss_kb=40)

            self.assertEqual(process_tree_rss_bytes(100, proc_root), 194_560)

    def test_reports_missing_and_malformed_required_memory_fields(self) -> None:
        cases = (
            ("MemTotal: 10 kB\n", "MemAvailable"),
            ("MemTotal: many kB\nMemAvailable: 2 kB\n", "MemTotal"),
            ("MemTotal: 10 MB\nMemAvailable: 2 kB\n", "MemTotal"),
        )
        for contents, field in cases:
            with self.subTest(field=field, contents=contents), TemporaryDirectory() as directory:
                meminfo = Path(directory) / "meminfo"
                meminfo.write_text(contents, encoding="ascii")
                with self.assertRaisesRegex(ResourceError, field):
                    read_memory_snapshot(meminfo)

    def test_reports_existing_status_without_a_valid_vmrss(self) -> None:
        cases = ((None, "missing"), ("not-a-number", "malformed"), ("12 MB", "malformed"))
        for value, diagnostic in cases:
            with self.subTest(value=value), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(proc_root, 200, rss_kb=None)
                status_path = proc_root / "200/status"
                if value is not None:
                    status_path.write_text(
                        f"Name:\ttest\nVmRSS:\t{value}\n", encoding="ascii"
                    )
                with self.assertRaisesRegex(
                    ResourceError,
                    f"{diagnostic}.*VmRSS|VmRSS.*{diagnostic}",
                ):
                    process_tree_rss_bytes(200, proc_root)

    def test_missing_proc_capability_is_explicitly_sequential(self) -> None:
        with TemporaryDirectory() as directory:
            missing = Path(directory) / "missing"
            capability = detect_resource_capability(
                proc_root=missing,
                meminfo_path=missing / "meminfo",
            )

        self.assertTrue(capability.sequential_only)
        self.assertEqual(capability.monitor_mode, "sequential_fallback")
        self.assertIsNone(capability.initial_memory_snapshot)
        self.assertIn("unavailable", capability.diagnostic)


@unittest.skipUnless(Path("/proc/self/status").is_file(), "Linux /proc is required")
class LiveProcessMonitorTest(unittest.TestCase):
    """Exercise the real monitor against a bounded parent/child process tree."""

    def test_monitors_child_memory_and_leaves_no_process_group_member(self) -> None:
        with TemporaryDirectory() as directory:
            temporary = Path(directory)
            child_pid_path = temporary / "child.pid"
            child_program = (
                "import os,pathlib,time;"
                "allocation=bytearray(32*1024*1024);"
                f"pathlib.Path({str(child_pid_path)!r}).write_text(str(os.getpid()));"
                "time.sleep(0.45)"
            )
            parent_program = (
                "import subprocess,sys;"
                "allocation=bytearray(4*1024*1024);"
                f"child=subprocess.Popen([sys.executable,'-c',{child_program!r}]);"
                "raise SystemExit(child.wait())"
            )
            process = subprocess.Popen(
                [sys.executable, "-c", parent_program],
                start_new_session=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            monitor = ProcessTreeResourceMonitor(
                process.pid,
                detect_resource_capability(),
                sample_interval_ms=100,
            )
            monitor.start()
            try:
                deadline = time.monotonic() + 2.0
                while not child_pid_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(child_pid_path.exists(), "child did not start")
                self.assertGreater(
                    process_tree_rss_bytes(process.pid),
                    32 * 1024 * 1024,
                    "recursive measurement omitted the allocating child",
                )
                exit_code = process.wait(timeout=2.0)
                measurement = monitor.finish(exit_code)
            finally:
                if process.poll() is None or _process_group_exists(process.pid):
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=1.0)

            self.assertEqual(
                measurement.usage.sample_interval_ms,
                100,
            )
            self.assertGreater(measurement.usage.peak_rss_bytes, 32 * 1024 * 1024)
            self.assertGreater(measurement.usage.wall_time_seconds, 0.0)
            self.assertEqual(measurement.usage.exit_code, 0)
            self.assertGreater(measurement.minimum_mem_available_bytes, 0)
            self.assertGreater(measurement.minimum_mem_available_percent, 0.0)
            self.assertFalse(_process_group_exists(process.pid))


class ResourcePublicationTest(unittest.TestCase):
    """Protect deterministic, atomic, exclusive, no-follow resource JSON."""

    def test_publishes_ordered_json_once_without_temporary_files(self) -> None:
        document = {
            "schema_version": 1,
            "experiment_id": 7,
            "repetition_attempt": 2,
            "sample_interval_ms": 100,
            "peak_rss_bytes": 123456,
            "minimum_mem_available_bytes": 654321,
            "minimum_mem_available_percent": 37.5,
            "wall_time_seconds": 1.25,
            "exit_code": 0,
            "monitor_mode": "linux_proc",
        }
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "resource_usage.json"
            publish_json_exclusive(output_path, document)
            raw = output_path.read_text(encoding="utf-8")

            self.assertEqual(list(json.loads(raw)), list(document))
            self.assertEqual(json.loads(raw), document)
            self.assertEqual(list(Path(directory).iterdir()), [output_path])
            with self.assertRaises(FileExistsError):
                publish_json_exclusive(output_path, {"schema_version": 99})
            self.assertEqual(json.loads(output_path.read_text(encoding="utf-8")), document)

    def test_refuses_destination_symlink_without_touching_target(self) -> None:
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            outside = Path(outside_directory) / "outside.json"
            outside.write_text('{"sentinel":true}', encoding="ascii")
            output_path = Path(directory) / "resource_usage.json"
            output_path.symlink_to(outside)

            with self.assertRaises(FileExistsError):
                publish_json_exclusive(output_path, {"schema_version": 1})

            self.assertTrue(output_path.is_symlink())
            self.assertEqual(outside.read_text(encoding="ascii"), '{"sentinel":true}')

    def test_attempt_usage_contract_is_frozen_and_literal(self) -> None:
        self.assertEqual(
            AttemptResourceUsage(100, 2048, 0.5, -signal.SIGTERM),
            AttemptResourceUsage(
                sample_interval_ms=100,
                peak_rss_bytes=2048,
                wall_time_seconds=0.5,
                exit_code=-signal.SIGTERM,
            ),
        )


if __name__ == "__main__":
    unittest.main()
