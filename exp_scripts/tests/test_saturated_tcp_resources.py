"""Linux process-tree resource measurement and publication tests."""

from __future__ import annotations

import errno
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from tempfile import TemporaryDirectory
import threading
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
    status_state: str | None = None,
    stat_state: str = "S",
) -> None:
    process_directory = proc_root / str(process_id)
    process_directory.mkdir()
    status = "Name:\ttest\n"
    if status_state is not None:
        status += f"State:\t{status_state}\n"
    if rss_kb is not None:
        status += f"VmRSS:\t{rss_kb} kB\n"
    (process_directory / "status").write_text(status, encoding="ascii")
    stat_fields = [stat_state, str(parent_pid), *("0" for _ in range(17))]
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


def _cleanup_real_process(
    process: subprocess.Popen,
    monitor: ProcessTreeResourceMonitor | None,
) -> None:
    if process.poll() is None or _process_group_exists(process.pid):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.wait(timeout=1.0)
    if monitor is not None:
        try:
            monitor.finish(process.returncode)
        except ResourceError:
            pass


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

    def test_retries_one_transient_live_status_without_vmrss(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(proc_root, 200, rss_kb=7, stat_state="R")
            status_path = proc_root / "200/status"
            original_read_text = Path.read_text
            status_reads = 0

            def transient_status(path, *args, **kwargs):
                nonlocal status_reads
                if path == status_path:
                    status_reads += 1
                    if status_reads == 1:
                        return "Name:\ttest\nState:\tR (running)\n"
                return original_read_text(path, *args, **kwargs)

            with mock.patch.object(Path, "read_text", new=transient_status):
                self.assertEqual(
                    resources._read_process_rss_bytes(200, proc_root),
                    7 * 1024,
                )
            self.assertEqual(status_reads, 2)

    def test_does_not_retry_missing_vmrss_without_explicit_running_state(self) -> None:
        cases = (
            ("Name:\ttest\nState:\tS (sleeping)\n", "sleeping"),
            ("Name:\ttest\nState:\tReady\n", "non-code-running-prefix"),
            ("Name:\ttest\nState:\tRR (malformed)\n", "malformed-state"),
            ("Name:\ttest\n", "missing-state"),
        )
        for first_status, case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(proc_root, 200, rss_kb=7)
                status_path = proc_root / "200/status"
                original_read_text = Path.read_text
                status_reads = 0

                def non_running_status(path, *args, **kwargs):
                    nonlocal status_reads
                    if path == status_path:
                        status_reads += 1
                        if status_reads == 1:
                            return first_status
                    return original_read_text(path, *args, **kwargs)

                with (
                    mock.patch.object(Path, "read_text", new=non_running_status),
                    self.assertRaisesRegex(ResourceError, "missing.*VmRSS|VmRSS.*missing"),
                ):
                    resources._read_process_rss_bytes(200, proc_root)
                self.assertEqual(status_reads, 1)

    def test_stale_running_status_uses_current_stat_zombie(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                200,
                rss_kb=None,
                status_state="R (running)",
                stat_state="Z",
            )
            self.assertIsNone(resources._read_process_rss_bytes(200, proc_root))

    def test_absent_rss_checks_current_stat_exit_before_status_classification(self) -> None:
        cases = (
            ("X (dead)", "S", True, "dead-status-stat-gone"),
            ("D (disk sleep)", "Z", False, "transition-status-stat-zombie"),
            (None, "Z", False, "missing-status-state-stat-zombie"),
            ("RR (malformed)", "Z", False, "malformed-status-state-stat-zombie"),
            ("S (sleeping)", "X", False, "sleeping-status-stat-dead"),
        )
        for status_state, stat_state, remove_stat, case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(
                    proc_root,
                    200,
                    rss_kb=None,
                    status_state=status_state,
                    stat_state=stat_state,
                )
                if remove_stat:
                    (proc_root / "200/stat").unlink()
                self.assertIsNone(
                    resources._read_process_rss_bytes(200, proc_root)
                )

    def test_absent_rss_invalid_status_with_live_stat_keeps_diagnostics(self) -> None:
        cases = (
            ("S (sleeping)", "sleeping"),
            (None, "missing"),
            ("RR (malformed)", "malformed"),
        )
        for status_state, case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(
                    proc_root,
                    200,
                    rss_kb=None,
                    status_state=status_state,
                    stat_state="S",
                )
                with self.assertRaises(ResourceError) as caught:
                    resources._read_process_rss_bytes(200, proc_root)
                diagnostic = str(caught.exception)
                self.assertIn("status_state=", diagnostic)
                self.assertIn("stat_state=S", diagnostic)
                self.assertIn("status=", diagnostic)

    def test_malformed_vmrss_is_never_forgiven_by_stat_exit(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                200,
                rss_kb=None,
                status_state="X (dead)",
                stat_state="Z",
            )
            (proc_root / "200/status").write_text(
                "Name:\ttest\nState:\tX (dead)\nVmRSS:\tnot-a-number kB\n",
                encoding="ascii",
            )
            with (
                mock.patch.object(
                    resources,
                    "_read_process_identity",
                    side_effect=AssertionError("stat must not be read"),
                ),
                self.assertRaisesRegex(ResourceError, "malformed.*VmRSS|VmRSS.*malformed"),
            ):
                resources._read_process_rss_bytes(200, proc_root)

    def test_repeated_running_transition_returns_none_when_stat_disappears(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                200,
                rss_kb=None,
                status_state="R (running)",
                stat_state="R",
            )
            stat_path = proc_root / "200/stat"
            original_read_text = Path.read_text
            stat_reads = 0

            def disappearing_stat(path, *args, **kwargs):
                nonlocal stat_reads
                if path == stat_path:
                    stat_reads += 1
                    if stat_reads == 2:
                        raise ProcessLookupError(errno.ESRCH, "process vanished")
                return original_read_text(path, *args, **kwargs)

            with mock.patch.object(Path, "read_text", new=disappearing_stat):
                self.assertIsNone(
                    resources._read_process_rss_bytes(200, proc_root)
                )

    def test_retries_exit_transition_sequences_until_zombie(self) -> None:
        cases = (
            ("R (running)", "R (running)", "Z (zombie)"),
            ("R (running)", "D (disk sleep)", "R (running)", "Z (zombie)"),
        )
        for states in cases:
            with self.subTest(states=states), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(proc_root, 200, rss_kb=None, stat_state="R")
                status_path = proc_root / "200/status"
                state_reads = iter(states)
                original_read_text = Path.read_text

                def transition_status(path, *args, **kwargs):
                    if path == status_path:
                        state = next(state_reads)
                        return f"Name:\ttest\nState:\t{state}\n"
                    return original_read_text(path, *args, **kwargs)

                with mock.patch.object(Path, "read_text", new=transition_status):
                    self.assertIsNone(
                        resources._read_process_rss_bytes(200, proc_root)
                    )

    def test_retries_running_or_uninterruptible_transition_until_valid_rss(self) -> None:
        for state_code in ("R", "D"):
            with self.subTest(state=state_code), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(
                    proc_root,
                    200,
                    rss_kb=7,
                    stat_state=state_code,
                )
                status_path = proc_root / "200/status"
                original_read_text = Path.read_text
                status_reads = 0

                def transient_status(path, *args, **kwargs):
                    nonlocal status_reads
                    if path == status_path:
                        status_reads += 1
                        if status_reads == 1:
                            return f"Name:\ttest\nState:\t{state_code}\n"
                    return original_read_text(path, *args, **kwargs)

                with mock.patch.object(Path, "read_text", new=transient_status):
                    self.assertEqual(
                        resources._read_process_rss_bytes(200, proc_root),
                        7 * 1024,
                    )
                self.assertEqual(status_reads, 2)

    def test_transition_retry_does_not_measure_reused_pid_instance(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory)
            _write_process(
                proc_root,
                200,
                rss_kb=7,
                start_time_ticks=2_000,
                stat_state="R",
            )
            status_path = proc_root / "200/status"
            stat_path = proc_root / "200/stat"
            original_read_text = Path.read_text
            status_reads = 0
            stat_reads = 0

            def reused_during_retry(path, *args, **kwargs):
                nonlocal status_reads, stat_reads
                contents = original_read_text(path, *args, **kwargs)
                if path == status_path:
                    status_reads += 1
                    if status_reads == 1:
                        return "Name:\ttest\nState:\tR (running)\n"
                if path == stat_path:
                    stat_reads += 1
                    if stat_reads == 2:
                        prefix, _, _start_time = contents.rstrip().rpartition(" ")
                        return f"{prefix} 3000\n"
                return contents

            with mock.patch.object(Path, "read_text", new=reused_during_retry):
                self.assertIsNone(
                    resources._read_process_rss_bytes(200, proc_root)
                )

    def test_transition_retry_deadline_is_bounded_and_diagnostic(self) -> None:
        class FakeClock:
            def __init__(self) -> None:
                self.now = 0.0
                self.sleeps = []

            def monotonic(self) -> float:
                return self.now

            def sleep(self, seconds: float) -> None:
                self.sleeps.append(seconds)
                self.now += seconds

        for state_code in ("R", "D"):
            with self.subTest(state=state_code), TemporaryDirectory() as directory:
                proc_root = Path(directory)
                _write_process(
                    proc_root,
                    200,
                    rss_kb=None,
                    status_state=state_code,
                    stat_state=state_code,
                )
                status_path = proc_root / "200/status"
                status_path.write_text(
                    f"Name:\ttest\nState:\t{state_code}\nExtra:\t" + "x" * 5_000,
                    encoding="ascii",
                )
                clock = FakeClock()
                with self.assertRaises(ResourceError) as caught:
                    resources._read_process_rss_bytes(
                        200,
                        proc_root,
                        monotonic=clock.monotonic,
                        sleep=clock.sleep,
                    )
                diagnostic = str(caught.exception)
                self.assertIn(f"state={state_code}", diagnostic)
                self.assertIn("status=", diagnostic)
                self.assertIn("Name:\\ttest", diagnostic)
                self.assertIn("truncated", diagnostic)
                self.assertLess(len(diagnostic), 2_000)
                self.assertGreaterEqual(clock.now, 0.010)
                self.assertTrue(clock.sleeps)

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

    def test_concurrent_active_wrapper_trees_exit_without_rss_errors_or_leaks(self) -> None:
        scenario = "import time;allocation=bytearray(1024*1024);time.sleep(0.001)"
        wrapper = (
            "import subprocess,sys;"
            f"code={scenario!r};"
            "exec('for _ in range(80):\\n "
            "process=subprocess.Popen([sys.executable,\"-c\",code])\\n "
            "process.wait()')"
        )
        processes = [
            subprocess.Popen(
                [sys.executable, "-c", wrapper],
                start_new_session=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            for _ in range(4)
        ]
        errors = []

        def sample_tree(process) -> None:
            while process.poll() is None:
                try:
                    process_tree_rss_bytes(process.pid)
                except ResourceError as error:
                    errors.append(str(error))

        try:
            with ThreadPoolExecutor(max_workers=4) as executor:
                futures = [executor.submit(sample_tree, process) for process in processes]
                while any(process.poll() is None for process in processes):
                    for process in processes:
                        if process.poll() is None:
                            try:
                                process_tree_rss_bytes(process.pid)
                            except ResourceError as error:
                                errors.append(str(error))
                for future in futures:
                    future.result()
            self.assertEqual(errors, [])
        finally:
            for process in processes:
                _cleanup_real_process(process, None)
                self.assertFalse(_process_group_exists(process.pid))

    def test_fast_exit_rss_transition_is_tolerated_without_leaking_groups(self) -> None:
        for iteration in range(24):
            with self.subTest(iteration=iteration):
                process = subprocess.Popen(
                    [
                        sys.executable,
                        "-c",
                        "import sys;"
                        "allocation=bytearray(1024*1024);"
                        "sys.stdout.write('1');sys.stdout.flush();"
                        "sys.stdin.read(1)",
                    ],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
                release = None
                try:
                    self.assertEqual(process.stdout.read(1), b"1")
                    self.assertGreater(
                        resources._read_process_rss_bytes(process.pid, Path("/proc")),
                        0,
                    )

                    def release_process() -> None:
                        process.stdin.write(b"x")
                        process.stdin.flush()
                        process.stdin.close()

                    release = threading.Thread(target=release_process)
                    release.start()
                    deadline = time.monotonic() + 1.0
                    while time.monotonic() < deadline:
                        if (
                            resources._read_process_rss_bytes(
                                process.pid,
                                Path("/proc"),
                            )
                            is None
                        ):
                            break
                    else:
                        self.fail("exiting process never became unavailable")
                    release.join()
                    self.assertEqual(process.wait(timeout=1.0), 0)
                    self.assertFalse(_process_group_exists(process.pid))
                finally:
                    if release is not None and release.is_alive():
                        release.join(timeout=1.0)
                    _cleanup_real_process(process, None)
                    if process.stdin is not None and not process.stdin.closed:
                        process.stdin.close()
                    if process.stdout is not None and not process.stdout.closed:
                        process.stdout.close()

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
            monitor = None
            monitor_finished = False
            try:
                monitor = ProcessTreeResourceMonitor(
                    process.pid,
                    detect_resource_capability(),
                    sample_interval_ms=100,
                )
                monitor.start()
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
                monitor_finished = True
            finally:
                _cleanup_real_process(
                    process,
                    None if monitor_finished else monitor,
                )

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
        monitor = None
        monitor_finished = False
        try:
            monitor = ProcessTreeResourceMonitor(
                process.pid,
                detect_resource_capability(),
            )
            monitor.start()
            exit_code = process.wait(timeout=1.0)
            measurement = monitor.finish(exit_code)
            monitor_finished = True
        finally:
            _cleanup_real_process(
                process,
                None if monitor_finished else monitor,
            )

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

    def test_synchronous_start_error_remains_an_error_at_finish(self) -> None:
        with TemporaryDirectory() as directory:
            proc_root = Path(directory) / "proc"
            proc_root.mkdir()
            _write_process(proc_root, 400, rss_kb=None)
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
            monitor = ProcessTreeResourceMonitor(400, capability)

            with self.assertRaisesRegex(ResourceError, "VmRSS"):
                monitor.start()
            with self.assertRaisesRegex(ResourceError, "VmRSS"):
                monitor.finish(1)


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

    def test_no_proc_publication_refuses_temp_name_substitution(self) -> None:
        document = {"schema_version": 1, "peak_rss_bytes": 4096}
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            parent = Path(directory)
            output_path = parent / "resource_usage.json"
            outside = Path(outside_directory) / "outside.json"
            outside.write_text('{"sentinel":true}', encoding="ascii")
            real_link = os.link
            link_sources = []

            def substituting_link(source, destination, **kwargs):
                link_sources.append(source)
                if str(source).startswith("/proc/"):
                    raise FileNotFoundError(errno.ENOENT, "proc is unavailable")
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
            self.assertEqual(len(link_sources), 1)
            self.assertFalse(str(link_sources[0]).startswith("/proc/"))
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
