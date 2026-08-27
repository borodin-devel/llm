"""Linux process-tree resource measurement and publication tests."""

from __future__ import annotations

import errno
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
import unittest
from unittest import mock

from saturated_tcp_benchmark import resources
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
    parent_pid: int = 1,
    start_time_ticks: int | None = None,
) -> None:
    process_directory = proc_root / str(process_id)
    process_directory.mkdir()
    status = "Name:\ttest\n"
    if rss_kb is not None:
        status += f"VmRSS:\t{rss_kb} kB\n"
    (process_directory / "status").write_text(status, encoding="ascii")
    stat_fields = ["S", str(parent_pid), *("0" for _ in range(17))]
    stat_fields.append(str(start_time_ticks if start_time_ticks is not None else process_id * 10))
    (process_directory / "stat").write_text(
        f"{process_id} (test process) {' '.join(stat_fields)}\n",
        encoding="ascii",
    )
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
            _write_process(
                proc_root,
                101,
                rss_kb=20,
                children_by_thread={101: "103"},
                parent_pid=100,
            )
            _write_process(
                proc_root,
                102,
                rss_kb=30,
                children_by_thread={102: "103"},
                parent_pid=100,
            )
            _write_process(proc_root, 103, rss_kb=40, parent_pid=101)

            self.assertEqual(process_tree_rss_bytes(100, proc_root), 194_560)

    def test_treats_enoent_and_esrch_as_vanished_at_each_per_pid_read(self) -> None:
        for operation in ("status", "task", "children"):
            with self.subTest(operation=operation), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(
                    proc_root,
                    100,
                    rss_kb=10,
                    children_by_thread={100: "101"},
                )
                _write_process(proc_root, 101, rss_kb=20, parent_pid=100)
                original_read_text = Path.read_text
                original_iterdir = Path.iterdir

                def failing_read_text(path, *args, **kwargs):
                    if operation == "status" and path == proc_root / "101/status":
                        raise ProcessLookupError(errno.ESRCH, "process vanished")
                    if operation == "children" and path == proc_root / "100/task/100/children":
                        raise ProcessLookupError(errno.ESRCH, "process vanished")
                    return original_read_text(path, *args, **kwargs)

                def failing_iterdir(path):
                    if operation == "task" and path == proc_root / "100/task":
                        raise ProcessLookupError(errno.ESRCH, "process vanished")
                    return original_iterdir(path)

                with (
                    mock.patch.object(Path, "read_text", new=failing_read_text),
                    mock.patch.object(Path, "iterdir", new=failing_iterdir),
                ):
                    self.assertEqual(process_tree_rss_bytes(100, proc_root), 10_240)

    def test_rejects_child_pid_reused_outside_current_parent_tree(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                100,
                rss_kb=10,
                children_by_thread={100: "101"},
            )
            _write_process(
                proc_root,
                101,
                rss_kb=50,
                parent_pid=999,
                start_time_ticks=2000,
            )

            self.assertEqual(process_tree_rss_bytes(100, proc_root), 10_240)

    def test_distinguishes_reused_pid_instances_by_start_time(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                100,
                rss_kb=10,
                children_by_thread={100: "102 101"},
            )
            _write_process(proc_root, 101, rss_kb=20, parent_pid=100)
            _write_process(
                proc_root,
                102,
                rss_kb=30,
                children_by_thread={102: "101"},
                parent_pid=100,
            )
            identities = {
                100: resources._ProcessIdentity(100, 1, 1000),
                102: resources._ProcessIdentity(102, 100, 1020),
            }
            child_identities = iter(
                (
                    resources._ProcessIdentity(101, 100, 1010),
                    resources._ProcessIdentity(101, 100, 1010),
                    resources._ProcessIdentity(101, 100, 1010),
                    resources._ProcessIdentity(101, 102, 2010),
                    resources._ProcessIdentity(101, 102, 2010),
                    resources._ProcessIdentity(101, 102, 2010),
                )
            )

            def changing_identity(process_id, root):
                if process_id == 101:
                    return next(child_identities)
                return identities[process_id]

            with mock.patch.object(
                resources,
                "_read_process_identity",
                side_effect=changing_identity,
            ):
                self.assertEqual(process_tree_rss_bytes(100, proc_root), 81_920)

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

    def test_fast_process_disappearance_does_not_fail_monitor_finish(self) -> None:
        process = subprocess.Popen(
            ["/bin/true"],
            start_new_session=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        monitor = ProcessTreeResourceMonitor(process.pid, detect_resource_capability())
        monitor.start()
        try:
            exit_code = process.wait(timeout=1.0)
            measurement = monitor.finish(exit_code)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=1.0)

        self.assertEqual(measurement.usage.exit_code, 0)
        self.assertGreaterEqual(measurement.usage.peak_rss_bytes, 0)

    def test_start_samples_before_background_thread_can_run(self) -> None:
        class DormantThread:
            def __init__(self, **kwargs) -> None:
                pass

            def start(self) -> None:
                pass

            def join(self) -> None:
                pass

        with TemporaryDirectory() as directory:
            proc_root = Path(directory) / "proc"
            proc_root.mkdir()
            _write_process(proc_root, 300, rss_kb=7)
            meminfo = proc_root / "meminfo"
            meminfo.write_text(
                "MemTotal: 1000 kB\nMemAvailable: 750 kB\n",
                encoding="ascii",
            )
            capability = resources.ResourceCapability(
                monitor_mode="linux_proc",
                proc_root=proc_root,
                meminfo_path=meminfo,
                initial_memory_snapshot=MemorySnapshot(1_024_000, 768_000),
                diagnostic="test",
            )
            monitor = ProcessTreeResourceMonitor(300, capability)
            with mock.patch.object(resources.threading, "Thread", DormantThread):
                monitor.start()
                measurement = monitor.finish(0)

        self.assertEqual(measurement.usage.peak_rss_bytes, 7_168)
        self.assertEqual(measurement.minimum_mem_available_bytes, 768_000)


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

    def test_refuses_temp_name_substitution_without_publishing_symlink(self) -> None:
        document = {"schema_version": 1, "peak_rss_bytes": 4096}
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            parent = Path(directory)
            output_path = parent / "resource_usage.json"
            outside = Path(outside_directory) / "outside.json"
            outside.write_text('{"sentinel":true}', encoding="ascii")
            real_link = os.link

            def substituting_link(source, destination, **kwargs):
                temporary = next(path for path in parent.iterdir() if path.name.endswith(".tmp"))
                temporary.unlink()
                temporary.symlink_to(outside)
                return real_link(source, destination, **kwargs)

            with (
                mock.patch.object(resources.os, "link", new=substituting_link),
                self.assertRaisesRegex(ResourceError, "temporary identity"),
            ):
                publish_json_exclusive(output_path, document)

            self.assertFalse(output_path.exists())
            self.assertFalse(output_path.is_symlink())
            self.assertEqual(outside.read_text(encoding="ascii"), '{"sentinel":true}')

    def test_rejects_replaced_parent_directory_identity(self) -> None:
        with TemporaryDirectory() as directory:
            original_parent = Path(directory) / "attempt"
            original_parent.mkdir()
            status = original_parent.lstat()
            displaced = Path(directory) / "displaced"
            original_parent.rename(displaced)
            original_parent.mkdir()

            with self.assertRaisesRegex(ResourceError, "parent.*identity|identity.*parent"):
                publish_json_exclusive(
                    original_parent / "resource_usage.json",
                    {"schema_version": 1},
                    expected_parent_identity=(status.st_dev, status.st_ino),
                )

            self.assertEqual(list(original_parent.iterdir()), [])

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
