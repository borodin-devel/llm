"""Self-tests for live policy, orchestration, failures, and process-tree timeouts."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import live_trace_runner
from live_trace_common import (
    POLICY, LiveTraceError, build_llm_command, reject_legacy_console,
    validate_policy_coverage,
)
from live_trace_runner import _run_captured, run_one_trace


class _FakeProcess:
    def __init__(self, command, returncode, stdout):
        self.args = command
        self.returncode = returncode
        self.stdout = stdout
        self.pid = 999999999

    def communicate(self, timeout=None):
        return self.stdout, None


def _read_test_process_identity(process_id):
    try:
        stat_text = Path(f"/proc/{process_id}/stat").read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    _, separator, stat_fields = stat_text.rpartition(") ")
    fields = stat_fields.split()
    if not separator or len(fields) <= 19:
        raise RuntimeError(f"cannot read process identity for PID {process_id}")
    return process_id, int(fields[19])


def _kill_test_process(
    process_identity,
    *,
    identity_reader=_read_test_process_identity,
    signal_process=os.kill,
):
    if process_identity is None:
        return
    process_id, _ = process_identity
    current_identity = identity_reader(process_id)
    if current_identity is None:
        return
    if current_identity != process_identity:
        raise RuntimeError(f"process identity changed for PID {process_id}")
    try:
        signal_process(process_id, signal.SIGKILL)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 1.0
    while (
        identity_reader(process_id) == process_identity
        and time.monotonic() < deadline
    ):
        time.sleep(0.01)


class LiveTraceRunnerTest(unittest.TestCase):
    def setUp(self):
        self.trace = "contrib/llm/traces/1W_high_load_1s.json"
        self.source = Path("/tmp/fake/output.json")

    def test_policy_coverage_accepts_exact_set(self):
        directory = Path("/workspace/contrib/llm/traces")
        discovered = [directory / name for name in reversed(POLICY)]
        self.assertEqual(validate_policy_coverage(discovered, directory), tuple(POLICY))

    def test_policy_coverage_rejects_missing_and_unknown(self):
        directory = Path("/workspace/contrib/llm/traces")
        missing = [directory / name for name in POLICY if name != "1W_high_load_1s.json"]
        with self.assertRaisesRegex(LiveTraceError, "1W_high_load_1s.json"):
            validate_policy_coverage(missing, directory)
        unknown = [directory / name for name in POLICY] + [directory / "new.json"]
        with self.assertRaisesRegex(LiveTraceError, "new.json"):
            validate_policy_coverage(unknown, directory)

    def test_builds_all_four_exact_commands(self):
        run_directory = Path("/tmp/llm-trace-live.test.random")
        for name, policy in POLICY.items():
            expected_inner = (
                "llm_sample --config contrib/llm/config/basic_config.toml "
                f"--general-trace-file contrib/llm/traces/{name} "
                f"--general-run-folder {run_directory}"
            )
            if policy["mode"] == "fixed":
                expected_inner += (
                    " --simulation-duration-mode fixed "
                    "--simulation-fixed-duration-seconds 1.0"
                )
            self.assertEqual(
                build_llm_command(Path("contrib/llm/traces") / name, run_directory, policy),
                ["./ns3", "run", expected_inner],
            )

    def test_rejects_legacy_console_marker(self):
        with self.assertRaisesRegex(LiveTraceError, "Final per-second"):
            reject_legacy_console("prefix [Final per-second] row", self.source)

    def test_timeout_never_reprobes_group_after_observing_absence(self):
        process = _FakeProcess(["command"], -signal.SIGTERM, "output")
        with (
            mock.patch.object(live_trace_runner, "TERM_GRACE_SECONDS", 0),
            mock.patch.object(
                live_trace_runner, "_process_group_exists", side_effect=(False, True)
            ) as group_exists,
            mock.patch.object(live_trace_runner, "_signal_process_group") as signal_group,
        ):
            live_trace_runner._terminate_process_group(process, "timeout output")
        with self.subTest("probe count"):
            self.assertEqual(group_exists.call_count, 1)
        with self.subTest("signals"):
            signal_group.assert_called_once_with(process, signal.SIGTERM)

    def test_cleanup_refuses_reused_pid_without_signaling(self):
        process_identity = (123, 456)
        identity_reader = mock.Mock(return_value=(123, 789))
        signal_process = mock.Mock()
        with self.subTest("identity mismatch"):
            with self.assertRaisesRegex(RuntimeError, "identity changed"):
                _kill_test_process(
                    process_identity,
                    identity_reader=identity_reader,
                    signal_process=signal_process,
                )
        with self.subTest("no signal"):
            signal_process.assert_not_called()

    def test_cleanup_accepts_absent_process_without_signaling(self):
        signal_process = mock.Mock()
        _kill_test_process(
            (123, 456),
            identity_reader=mock.Mock(return_value=None),
            signal_process=signal_process,
        )
        signal_process.assert_not_called()

    def test_cleans_up_after_command_failure(self):
        self._assert_run_failure_cleans(parse_failure=False)

    def test_cleans_up_after_parse_failure(self):
        self._assert_run_failure_cleans(parse_failure=True)

    def test_cleans_up_after_command_construction_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)
            trace = outer / self.trace
            trace.parent.mkdir(parents=True)
            trace.write_text('{"traces": []}', encoding="utf-8")
            temporary_parent = outer / "tmp"
            temporary_parent.mkdir()

            def factory(command, **kwargs):
                self._assert_popen_kwargs(kwargs)
                return _FakeProcess(command, 0, "valid\n")

            invalid_policy = {"mode": "invalid", "timeout_seconds": 1}
            with self.assertRaises(LiveTraceError):
                run_one_trace(
                    outer,
                    trace,
                    invalid_policy,
                    process_factory=factory,
                    temporary_parent=temporary_parent,
                )
            self.assertEqual(list(temporary_parent.iterdir()), [])

    def _assert_popen_kwargs(self, kwargs):
        self.assertTrue(kwargs["start_new_session"])
        self.assertNotIn("timeout", kwargs)
        self.assertNotIn("check", kwargs)
        self.assertEqual(kwargs["stdout"], subprocess.PIPE)
        self.assertEqual(kwargs["stderr"], subprocess.STDOUT)

    def _assert_run_failure_cleans(self, parse_failure):
        with tempfile.TemporaryDirectory() as directory:
            outer = Path(directory)
            trace = outer / self.trace
            trace.parent.mkdir(parents=True)
            trace.write_text('{"traces": []}', encoding="utf-8")
            temporary_parent = outer / "tmp"
            temporary_parent.mkdir()

            def factory(command, **kwargs):
                self._assert_popen_kwargs(kwargs)
                if command[0] == "python3":
                    return _FakeProcess(command, 0, "valid\n")
                run_arguments = shlex.split(command[2])
                run_directory = Path(
                    run_arguments[run_arguments.index("--general-run-folder") + 1]
                )
                if parse_failure:
                    (run_directory / "output.json").write_text("{broken", encoding="utf-8")
                    return _FakeProcess(command, 0, "completed\n")
                return _FakeProcess(command, 7, "failed\n")

            with self.assertRaises(LiveTraceError):
                run_one_trace(
                    outer,
                    trace,
                    POLICY[trace.name],
                    process_factory=factory,
                    temporary_parent=temporary_parent,
                )
            self.assertEqual(list(temporary_parent.iterdir()), [])

    def test_timeout_terminates_grandchild_process_group_and_closes_pipe(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            pid_path = temporary / "grandchild.pid"
            marker_path = temporary / "survived.marker"
            grandchild = (
                "import os,signal,time,pathlib;"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                f"pathlib.Path({str(pid_path)!r}).write_text(str(os.getpid()));"
                "time.sleep(0.6);"
                f"pathlib.Path({str(marker_path)!r}).write_text('survived');"
                "time.sleep(0.4)"
            )
            parent = (
                "import subprocess,sys,time;"
                f"subprocess.Popen([sys.executable, '-c', {grandchild!r}]);"
                "time.sleep(30)"
            )
            started = time.monotonic()
            with self.assertRaisesRegex(LiveTraceError, "exceeded"):
                _run_captured(
                    subprocess.Popen,
                    [sys.executable, "-c", parent],
                    temporary,
                    0.1,
                    self.source,
                )
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 0.8, "inherited output pipe stayed open")
            self.assertTrue(pid_path.exists(), "grandchild did not start")
            time.sleep(0.6)
            self.assertFalse(marker_path.exists(), "grandchild survived timeout cleanup")
            grandchild_pid = int(pid_path.read_text(encoding="utf-8"))
            status_path = Path(f"/proc/{grandchild_pid}/stat")
            reap_deadline = time.monotonic() + 1.0
            while status_path.exists() and time.monotonic() < reap_deadline:
                process_state = status_path.read_text(encoding="utf-8").split()[2]
                self.assertEqual(process_state, "Z", "grandchild remains live after timeout")
                time.sleep(0.01)
            self.assertFalse(status_path.exists(), "grandchild was not reaped after timeout")

    def test_timeout_kills_grandchild_after_leader_closes_pipe(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            pid_path = temporary / "grandchild.pid"
            marker_path = temporary / "survived.marker"
            grandchild = (
                "import os,signal,time,pathlib;"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                f"pathlib.Path({str(pid_path)!r}).write_text(str(os.getpid()));"
                "time.sleep(0.6);"
                f"pathlib.Path({str(marker_path)!r}).write_text('survived');"
                "time.sleep(30)"
            )
            parent = (
                "import subprocess,sys,time;"
                f"subprocess.Popen([sys.executable, '-c', {grandchild!r}],"
                "stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);"
                "time.sleep(30)"
            )
            started = time.monotonic()
            with self.assertRaisesRegex(LiveTraceError, "exceeded"):
                _run_captured(
                    subprocess.Popen,
                    [sys.executable, "-c", parent],
                    temporary,
                    0.1,
                    self.source,
                )
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 0.8, "timeout cleanup did not return promptly")
            self.assertTrue(pid_path.exists(), "grandchild did not start")
            grandchild_pid = int(pid_path.read_text(encoding="utf-8"))
            grandchild_identity = _read_test_process_identity(grandchild_pid)
            try:
                time.sleep(0.6)
                with self.subTest("survival marker"):
                    self.assertFalse(marker_path.exists(), "grandchild survived timeout cleanup")
                with self.subTest("process state"):
                    status_path = Path(f"/proc/{grandchild_pid}/stat")
                    reap_deadline = time.monotonic() + 1.0
                    while status_path.exists() and time.monotonic() < reap_deadline:
                        process_state = status_path.read_text(encoding="utf-8").split()[2]
                        self.assertEqual(
                            process_state, "Z", "grandchild remains live after timeout"
                        )
                        time.sleep(0.01)
                    self.assertFalse(
                        status_path.exists(), "grandchild was not reaped after timeout"
                    )
            finally:
                _kill_test_process(grandchild_identity)
