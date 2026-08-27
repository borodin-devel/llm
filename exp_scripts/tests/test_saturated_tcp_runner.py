"""Sequential saturated benchmark runner lifecycle tests."""

from __future__ import annotations

import csv
from io import StringIO
import json
import os
from pathlib import Path
import shlex
import signal
import stat
import subprocess
import sys
from tempfile import TemporaryDirectory
import time
import unittest
from unittest import mock

from saturated_tcp_benchmark import runner
from saturated_tcp_benchmark.matrix import ExperimentConfiguration, build_matrix
from saturated_tcp_benchmark.runner import (
    PROCESS_TIMEOUT_SECONDS,
    RunnerError,
    build_ns3_command,
    discover_ns3_root,
    load_runner_configuration,
    main,
    run_benchmark,
)
from tests.test_saturated_tcp_validation import make_output_document


OUTER_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_CONFIG = OUTER_ROOT / "contrib/llm/config/saturated_tcp_config.toml"


class _FakeProcess:
    """Small Popen-compatible process with deterministic wait effects."""

    def __init__(self, returncode: int = 0, *, wait_effects=(), on_reap=None) -> None:
        self.pid = 999999999
        self.returncode = None
        self._final_returncode = returncode
        self._wait_effects = list(wait_effects)
        self._on_reap = on_reap

    def wait(self, timeout=None):
        if self._wait_effects:
            effect = self._wait_effects.pop(0)
            if isinstance(effect, BaseException):
                raise effect
        self.returncode = self._final_returncode
        if self._on_reap is not None:
            callback, self._on_reap = self._on_reap, None
            callback()
        return self.returncode


class _InterruptingProcess:
    """Real process wrapper that injects one SIGINT-equivalent wait interruption."""

    def __init__(self, process: subprocess.Popen) -> None:
        self._process = process
        self.pid = process.pid
        self.returncode = None
        self._interrupted = False

    def wait(self, timeout=None):
        if not self._interrupted:
            self._interrupted = True
            raise KeyboardInterrupt
        result = self._process.wait(timeout=timeout)
        self.returncode = result
        return result

    def kill(self) -> None:
        self._process.kill()


def _read_test_process_identity(process_id: int):
    try:
        stat_text = Path(f"/proc/{process_id}/stat").read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    _, separator, stat_fields = stat_text.rpartition(") ")
    fields = stat_fields.split()
    if not separator or len(fields) <= 19:
        raise RuntimeError(f"cannot read process identity for PID {process_id}")
    return process_id, int(fields[19])


def _stored_test_process_identity(identity_path: Path) -> tuple[int, int]:
    fields = identity_path.read_text(encoding="utf-8").split()
    if len(fields) != 2:
        raise RuntimeError(f"invalid stored process identity in {identity_path}")
    return int(fields[0]), int(fields[1])


def _cleanup_test_process(identity_path: Path) -> None:
    if not identity_path.exists():
        return
    identity = _stored_test_process_identity(identity_path)
    current = _read_test_process_identity(identity[0])
    if current is None:
        return
    if current != identity:
        raise RuntimeError(f"process identity changed for PID {identity[0]}")
    try:
        os.kill(identity[0], signal.SIGKILL)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 1.0
    while _read_test_process_identity(identity[0]) == identity and time.monotonic() < deadline:
        time.sleep(0.01)


def _assert_test_process_reaped(test: unittest.TestCase, identity: tuple[int, int]) -> None:
    status_path = Path(f"/proc/{identity[0]}/stat")
    deadline = time.monotonic() + 1.0
    while _read_test_process_identity(identity[0]) == identity and time.monotonic() < deadline:
        state = status_path.read_text(encoding="utf-8").rpartition(") ")[2].split()[0]
        test.assertEqual(state, "Z", "descendant remains live after process-group cleanup")
        time.sleep(0.01)
    test.assertNotEqual(_read_test_process_identity(identity[0]), identity)


def create_fake_ns3_root(directory: str) -> Path:
    """Create the runner's minimum outer-root filesystem contract."""
    root = Path(directory)
    ns3 = root / "ns3"
    ns3.write_text("#!/bin/sh\nexit 99\n", encoding="ascii")
    ns3.chmod(0o755)
    return root


def write_valid_output(
    output_path: Path,
    configuration: ExperimentConfiguration,
    run_folder: Path,
    *,
    repetition_attempt: int = 1,
    repetitions: int = 1,
) -> None:
    """Write one complete fixture as if the ns-3 child succeeded."""
    document, _ = make_output_document(
        configuration,
        repetition_attempt=repetition_attempt,
        repetitions=repetitions,
        run_folder=str(run_folder),
    )
    output_path.write_text(json.dumps(document), encoding="utf-8")


def read_csv(path: Path) -> list[list[str]]:
    """Read the semicolon CSV using its published encoding."""
    with path.open("r", encoding="utf-8-sig", newline="") as input_file:
        return list(csv.reader(input_file, delimiter=";"))


class SaturatedTcpRunnerTest(unittest.TestCase):
    """Protect exact commands, sequential publication, failure, and retention."""

    def _assert_popen_kwargs(self, kwargs, root: Path) -> None:
        self.assertEqual(kwargs["cwd"], root)
        self.assertIs(kwargs["start_new_session"], True)
        self.assertNotIn("timeout", kwargs)
        self.assertNotIn("capture_output", kwargs)
        self.assertNotIn("text", kwargs)
        self.assertNotEqual(kwargs["stdout"], subprocess.PIPE)
        self.assertNotEqual(kwargs["stderr"], subprocess.PIPE)
        self.assertTrue(stat.S_ISREG(os.fstat(kwargs["stdout"].fileno()).st_mode))
        self.assertTrue(stat.S_ISREG(os.fstat(kwargs["stderr"].fileno()).st_mode))
        self.assertTrue(str(kwargs["stdout"].name).endswith("stdout.log"))
        self.assertTrue(str(kwargs["stderr"].name).endswith("stderr.log"))

    def test_discovers_outer_root_and_default_one_repetition(self) -> None:
        self.assertEqual(discover_ns3_root(), OUTER_ROOT)
        loaded = load_runner_configuration(DEFAULT_CONFIG)
        self.assertEqual(loaded.repetitions, 1)
        self.assertEqual(loaded.effective_configuration["script"]["repetitions"], 1)
        self.assertEqual(loaded.effective_configuration["wifi"]["guard_interval_ns"], 3200)
        self.assertEqual(
            loaded.effective_configuration["wifi"]["rts_cts_threshold_bytes"], 0
        )

    def test_repetition_parser_enforces_exact_uint32_range(self) -> None:
        original = DEFAULT_CONFIG.read_text(encoding="utf-8")
        with TemporaryDirectory() as directory:
            maximum_path = Path(directory) / "maximum.toml"
            maximum_path.write_text(
                original.replace("repetitions = 1", "repetitions = 4294967295"),
                encoding="utf-8",
            )
            self.assertEqual(load_runner_configuration(maximum_path).repetitions, 4294967295)

        for value in ("0", "-1", "true", "4294967296"):
            with self.subTest(value=value), TemporaryDirectory() as directory:
                config_path = Path(directory) / "config.toml"
                config_path.write_text(
                    original.replace("repetitions = 1", f"repetitions = {value}"),
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(RunnerError, "script.repetitions.*uint32"):
                    load_runner_configuration(config_path)

    def test_builds_exact_ns3_command_and_deterministic_paths(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            attempt_directory = (
                root
                / "run/scripted_exp_20260827_120000/experiment_001/attempt_2"
            )
            command = build_ns3_command(
                root / "ns3",
                DEFAULT_CONFIG,
                configuration,
                repetition_attempt=2,
                run_directory=attempt_directory,
            )

        expected_command_string = (
            f"saturated-tcp-scenario --config {DEFAULT_CONFIG} "
            "--benchmark-sta-count-per-bss=1 "
            "--benchmark-rssi-range=high "
            "--benchmark-interference-mode=isolated "
            "--benchmark-traffic-mode=ul "
            "--benchmark-mimo-mode=su "
            "--simulation-rng-seed=12345 "
            "--simulation-rng-run=2 "
            f"--general-run-folder={attempt_directory} "
            "--general-output-name=output.json"
        )
        self.assertEqual(command, [str(root / "ns3"), "run", expected_command_string])
        self.assertEqual(
            shlex.split(command[2]),
            [
                "saturated-tcp-scenario",
                "--config",
                str(DEFAULT_CONFIG),
                "--benchmark-sta-count-per-bss=1",
                "--benchmark-rssi-range=high",
                "--benchmark-interference-mode=isolated",
                "--benchmark-traffic-mode=ul",
                "--benchmark-mimo-mode=su",
                "--simulation-rng-seed=12345",
                "--simulation-rng-run=2",
                f"--general-run-folder={attempt_directory}",
                "--general-output-name=output.json",
            ],
        )

    def test_runs_one_process_at_a_time_and_publishes_before_the_next(self) -> None:
        configurations = build_matrix()[:2]
        timestamp = "20260827_120000"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            calls = []
            active = 0
            maximum_active = 0

            def fake_process(command, **kwargs):
                nonlocal active, maximum_active
                self.assertEqual(active, 0)
                active += 1
                maximum_active = max(maximum_active, active)
                call_index = len(calls)
                configuration = configurations[call_index]
                attempt_directory = (
                    run_directory
                    / f"experiment_{configuration.experiment_id:03d}"
                    / "attempt_1"
                )
                if call_index == 1:
                    self.assertEqual(len(read_csv(run_directory / "results.csv")), 4)
                self._assert_popen_kwargs(kwargs, root)
                calls.append((command, kwargs))
                kwargs["stdout"].write(b"child stdout\n")
                kwargs["stderr"].write(b"child stderr\n")
                write_valid_output(
                    attempt_directory / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(on_reap=lambda: active_setter(0))

            def active_setter(value):
                nonlocal active
                active = value

            result = run_benchmark(
                ns3_root=root,
                config_path=DEFAULT_CONFIG,
                timestamp=timestamp,
                configurations=configurations,
                process_factory=fake_process,
                output=StringIO(),
            )

            self.assertEqual(result, run_directory)
            self.assertEqual(maximum_active, 1)
            self.assertEqual(len(calls), 2)
            published_rows = read_csv(run_directory / "results.csv")
            self.assertEqual(len(published_rows), 7)
            self.assertEqual(
                [row[12] for row in published_rows[1:]],
                ["0.0", "0.0", "", "0.0", "0.0", ""],
                "positive baseline self-overhead is zero and a zero baseline stays empty",
            )
            for configuration in configurations:
                output_path = (
                    run_directory
                    / f"experiment_{configuration.experiment_id:03d}/attempt_1/output.json"
                )
                self.assertTrue(output_path.is_file())
                attempt_directory = output_path.parent
                self.assertEqual(
                    (attempt_directory / "stdout.log").read_text(encoding="utf-8"),
                    "child stdout\n",
                )
                self.assertEqual(
                    (attempt_directory / "stderr.log").read_text(encoding="utf-8"),
                    "child stderr\n",
                )
            self.assertEqual(list(run_directory.rglob("*.csv")), [run_directory / "results.csv"])

    def test_process_failures_stop_with_no_rows_for_failed_attempt(self) -> None:
        configuration = build_matrix()[0]
        cases = ("nonzero", "timeout", "missing", "invalid")
        for case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                root = create_fake_ns3_root(directory)
                timestamp = f"failure_{case}"
                attempt_directory = (
                    root / "run" / f"scripted_exp_{timestamp}/experiment_001/attempt_1"
                )

                def fake_process(command, **kwargs):
                    self._assert_popen_kwargs(kwargs, root)
                    if case == "nonzero":
                        kwargs["stdout"].write(
                            b"BEGIN_SHOULD_NOT_APPEAR" + b"x" * 10000 + b"out text\n"
                        )
                        kwargs["stderr"].write(b"y" * 10000 + b"err text\n")
                        return _FakeProcess(9)
                    if case == "timeout":
                        kwargs["stdout"].write(b"slow out\n")
                        kwargs["stderr"].write(b"slow err\n")
                        return _FakeProcess(
                            -signal.SIGTERM,
                            wait_effects=(
                                subprocess.TimeoutExpired(
                                    command, PROCESS_TIMEOUT_SECONDS
                                ),
                            ),
                        )
                    if case == "invalid":
                        (attempt_directory / "output.json").write_text("{}", encoding="utf-8")
                    return _FakeProcess(0)

                with self.assertRaises(RunnerError) as caught:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=(configuration,),
                        process_factory=fake_process,
                        output=StringIO(),
                    )
                rows = read_csv(root / "run" / f"scripted_exp_{timestamp}/results.csv")
                self.assertEqual(len(rows), 1)
                if case == "nonzero":
                    self.assertIn("out text", str(caught.exception))
                    self.assertIn("err text", str(caught.exception))
                    self.assertNotIn("BEGIN_SHOULD_NOT_APPEAR", str(caught.exception))
                if case == "invalid":
                    self.assertTrue((attempt_directory / "output.json").is_file())
                else:
                    self.assertFalse((attempt_directory / "output.json").exists())
                self.assertTrue((attempt_directory / "stdout.log").is_file())
                self.assertTrue((attempt_directory / "stderr.log").is_file())

    def test_completed_rows_and_every_created_json_survive_later_failure(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "retention"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            calls = 0

            def fake_process(command, **kwargs):
                nonlocal calls
                calls += 1
                self._assert_popen_kwargs(kwargs, root)
                configuration = configurations[calls - 1]
                attempt_directory = (
                    run_directory
                    / f"experiment_{configuration.experiment_id:03d}/attempt_1"
                )
                output_path = attempt_directory / "output.json"
                if calls == 1:
                    write_valid_output(output_path, configuration, attempt_directory)
                    return _FakeProcess(0)
                output_path.write_text('{"failed_attempt":true}', encoding="utf-8")
                kwargs["stdout"].write(b"second out\n")
                kwargs["stderr"].write(b"second err\n")
                return _FakeProcess(3)

            with self.assertRaisesRegex(RunnerError, "return code 3"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_factory=fake_process,
                    output=StringIO(),
                )
            self.assertEqual(calls, 2)
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 4)
            self.assertTrue((run_directory / "experiment_001/attempt_1/output.json").is_file())
            failed_output = run_directory / "experiment_002/attempt_1/output.json"
            self.assertEqual(failed_output.read_text(encoding="utf-8"), '{"failed_attempt":true}')
            self.assertFalse((run_directory / "experiment_003").exists())

    def test_existing_timestamp_and_attempt_paths_are_never_overwritten(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            existing = root / "run/scripted_exp_collision"
            existing.mkdir(parents=True)
            sentinel = existing / "sentinel.txt"
            sentinel.write_text("keep", encoding="ascii")
            calls = 0

            def unexpected_process(command, **kwargs):
                nonlocal calls
                calls += 1
                raise AssertionError("process must not start")

            with self.assertRaisesRegex(RunnerError, "already exists"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="collision",
                    configurations=(configuration,),
                    process_factory=unexpected_process,
                    output=StringIO(),
                )
            self.assertEqual(calls, 0)
            self.assertEqual(sentinel.read_text(encoding="ascii"), "keep")

        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run/scripted_exp_attempt_collision"
            calls = 0

            def colliding_process(command, **kwargs):
                nonlocal calls
                calls += 1
                self._assert_popen_kwargs(kwargs, root)
                first_attempt = run_directory / "experiment_001/attempt_1"
                write_valid_output(
                    first_attempt / "output.json",
                    configuration,
                    first_attempt,
                    repetitions=2,
                )
                future_attempt = run_directory / "experiment_001/attempt_2"
                future_attempt.mkdir()
                (future_attempt / "sentinel.txt").write_text("keep", encoding="ascii")
                return _FakeProcess(0)

            repeated_config = Path(directory) / "repeated.toml"
            repeated_config.write_text(
                DEFAULT_CONFIG.read_text(encoding="utf-8").replace(
                    "repetitions = 1", "repetitions = 2"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RunnerError, "already exists"):
                run_benchmark(
                    ns3_root=root,
                    config_path=repeated_config,
                    timestamp="attempt_collision",
                    configurations=(configuration,),
                    process_factory=colliding_process,
                    output=StringIO(),
                )
            self.assertEqual(calls, 1)
            self.assertEqual(
                (run_directory / "experiment_001/attempt_2/sentinel.txt").read_text(
                    encoding="ascii"
                ),
                "keep",
            )
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 4)

    def test_rejects_symlink_outer_run_directory_without_following_it(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            root = create_fake_ns3_root(directory)
            outside = Path(outside_directory)
            (root / "run").symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(RunnerError, "run.*symlink|symlink.*run"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="run_symlink",
                    configurations=(configuration,),
                    process_factory=lambda *args, **kwargs: self.fail("process started"),
                    output=StringIO(),
                )
            self.assertEqual(list(outside.iterdir()), [])

    def test_rejects_output_symlink_to_outside_attempt(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run/scripted_exp_output_symlink"
            attempt_directory = run_directory / "experiment_001/attempt_1"
            outside_output = Path(outside_directory) / "outside.json"

            def symlink_output(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                write_valid_output(outside_output, configuration, attempt_directory)
                (attempt_directory / "output.json").symlink_to(outside_output)
                return _FakeProcess(0)

            with self.assertRaisesRegex(RunnerError, "output.*symlink|regular.*output"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="output_symlink",
                    configurations=(configuration,),
                    process_factory=symlink_output,
                    output=StringIO(),
                )
            self.assertTrue(outside_output.is_file())
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 1)

    def test_rejects_attempt_directory_replaced_outside_containment(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory, TemporaryDirectory() as outside_directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run/scripted_exp_replaced_attempt"
            attempt_directory = run_directory / "experiment_001/attempt_1"
            displaced_attempt = run_directory / "experiment_001/displaced_attempt"
            outside = Path(outside_directory)

            def replace_attempt(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                attempt_directory.rename(displaced_attempt)
                attempt_directory.symlink_to(outside, target_is_directory=True)
                write_valid_output(
                    outside / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(0)

            with self.assertRaisesRegex(RunnerError, "attempt.*symlink|outside.*containment"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="replaced_attempt",
                    configurations=(configuration,),
                    process_factory=replace_attempt,
                    output=StringIO(),
                )
            self.assertTrue((outside / "output.json").is_file())
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 1)

    def test_rejects_attempt_directory_replaced_by_contained_directory(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run/scripted_exp_replaced_regular_attempt"
            experiment_directory = run_directory / "experiment_001"
            attempt_directory = experiment_directory / "attempt_1"
            displaced_attempt = experiment_directory / "displaced_attempt"

            def replace_attempt(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                kwargs["stdout"].write(b"original stdout\n")
                kwargs["stderr"].write(b"original stderr\n")
                attempt_directory.rename(displaced_attempt)
                attempt_directory.mkdir()
                (attempt_directory / "stdout.log").write_text(
                    "replacement stdout\n", encoding="utf-8"
                )
                (attempt_directory / "stderr.log").write_text(
                    "replacement stderr\n", encoding="utf-8"
                )
                write_valid_output(
                    attempt_directory / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(0)

            with self.assertRaisesRegex(RunnerError, "identity|replaced"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="replaced_regular_attempt",
                    configurations=(configuration,),
                    process_factory=replace_attempt,
                    output=StringIO(),
                )
            self.assertTrue((displaced_attempt / "stdout.log").is_file())
            self.assertTrue((attempt_directory / "output.json").is_file())
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 1)

    def test_rejects_log_replaced_by_regular_contained_file(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run/scripted_exp_replaced_log"
            attempt_directory = run_directory / "experiment_001/attempt_1"
            displaced_log = attempt_directory / "stdout.original.log"

            def replace_log(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                kwargs["stdout"].write(b"original stdout\n")
                (attempt_directory / "stdout.log").rename(displaced_log)
                (attempt_directory / "stdout.log").write_text(
                    "replacement stdout\n", encoding="utf-8"
                )
                write_valid_output(
                    attempt_directory / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(0)

            with self.assertRaisesRegex(RunnerError, "stdout.*identity|identity.*stdout"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="replaced_log",
                    configurations=(configuration,),
                    process_factory=replace_log,
                    output=StringIO(),
                )
            self.assertTrue(displaced_log.is_file())
            self.assertEqual(
                (attempt_directory / "stdout.log").read_text(encoding="utf-8"),
                "replacement stdout\n",
            )
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 1)

    def test_rejects_results_csv_replaced_during_append(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run/scripted_exp_replaced_csv"
            results_path = run_directory / "results.csv"
            displaced_results = run_directory / "results.original.csv"

            def valid_process(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                attempt_directory = run_directory / "experiment_001/attempt_1"
                write_valid_output(
                    attempt_directory / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(0)

            original_append = runner.ExcelCsvWriter.append_attempt

            def replace_after_append(writer, rows):
                original_append(writer, rows)
                writer.path.rename(displaced_results)
                writer.path.write_bytes(displaced_results.read_bytes())

            with (
                mock.patch.object(
                    runner.ExcelCsvWriter,
                    "append_attempt",
                    new=replace_after_append,
                ),
                self.assertRaisesRegex(RunnerError, "CSV.*identity|identity.*CSV"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="replaced_csv",
                    configurations=(configuration,),
                    process_factory=valid_process,
                    output=StringIO(),
                )
            self.assertTrue(displaced_results.is_file())
            self.assertTrue(results_path.is_file())
            self.assertEqual(results_path.read_bytes(), displaced_results.read_bytes())

    def test_sigterm_esrch_is_sticky_and_never_probes_group(self) -> None:
        process = _FakeProcess(-signal.SIGTERM)
        with (
            mock.patch.object(runner, "TERM_GRACE_SECONDS", 0),
            mock.patch.object(
                runner.os,
                "killpg",
                side_effect=(
                    ProcessLookupError(),
                    AssertionError("PGID was touched after SIGTERM reported absence"),
                ),
            ) as killpg,
        ):
            runner._terminate_process_group(process, process.pid)
        self.assertEqual(killpg.call_count, 1)
        killpg.assert_called_once_with(process.pid, signal.SIGTERM)

    def test_timeout_kills_term_ignoring_process_tree(self) -> None:
        with TemporaryDirectory() as directory:
            temporary = Path(directory)
            identity_path = temporary / "grandchild.identity"
            marker_path = temporary / "survived.marker"
            grandchild = (
                "import os,signal,time,pathlib;"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                "fields=pathlib.Path('/proc/self/stat').read_text().rpartition(') ')[2].split();"
                f"pathlib.Path({str(identity_path)!r}).write_text("
                "str(os.getpid())+' '+fields[19]);"
                "time.sleep(0.6);"
                f"pathlib.Path({str(marker_path)!r}).write_text('survived');"
                "time.sleep(30)"
            )
            parent = (
                "import subprocess,sys,time;"
                f"subprocess.Popen([sys.executable, '-c', {grandchild!r}]);"
                "time.sleep(30)"
            )
            stdout_path = temporary / "stdout.log"
            stderr_path = temporary / "stderr.log"
            started = time.monotonic()
            try:
                with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                    with self.assertRaisesRegex(RunnerError, "timed out"):
                        runner._run_process(
                            subprocess.Popen,
                            [sys.executable, "-c", parent],
                            temporary,
                            stdout,
                            stderr,
                            0.1,
                        )
                self.assertLess(time.monotonic() - started, 1.0)
                self.assertTrue(identity_path.exists(), "grandchild did not start")
                time.sleep(0.65)
                self.assertFalse(marker_path.exists(), "grandchild survived timeout")
                _assert_test_process_reaped(
                    self, _stored_test_process_identity(identity_path)
                )
            finally:
                _cleanup_test_process(identity_path)

    def test_wrapper_exit_kills_term_ignoring_child_after_logs_close(self) -> None:
        with TemporaryDirectory() as directory:
            temporary = Path(directory)
            identity_path = temporary / "grandchild.identity"
            marker_path = temporary / "survived.marker"
            grandchild = (
                "import os,signal,time,pathlib;"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                "fields=pathlib.Path('/proc/self/stat').read_text().rpartition(') ')[2].split();"
                f"pathlib.Path({str(identity_path)!r}).write_text("
                "str(os.getpid())+' '+fields[19]);"
                "time.sleep(0.6);"
                f"pathlib.Path({str(marker_path)!r}).write_text('survived');"
                "time.sleep(30)"
            )
            parent = (
                "import pathlib,subprocess,sys,time;"
                f"subprocess.Popen([sys.executable, '-c', {grandchild!r}],"
                "stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);"
                f"identity=pathlib.Path({str(identity_path)!r});"
                "deadline=time.monotonic()+1;"
                "exec('while not identity.exists() and time.monotonic() < deadline:\\n "
                "time.sleep(0.01)')"
            )
            stdout_path = temporary / "stdout.log"
            stderr_path = temporary / "stderr.log"
            started = time.monotonic()
            try:
                with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                    with self.assertRaisesRegex(RunnerError, "descendant"):
                        runner._run_process(
                            subprocess.Popen,
                            [sys.executable, "-c", parent],
                            temporary,
                            stdout,
                            stderr,
                            2.0,
                        )
                self.assertLess(time.monotonic() - started, 1.0)
                self.assertTrue(identity_path.exists(), "grandchild did not start")
                time.sleep(0.65)
                self.assertFalse(marker_path.exists(), "grandchild survived wrapper exit")
                _assert_test_process_reaped(
                    self, _stored_test_process_identity(identity_path)
                )
            finally:
                _cleanup_test_process(identity_path)

    def test_sigint_kills_real_term_ignoring_process_tree(self) -> None:
        with TemporaryDirectory() as directory:
            temporary = Path(directory)
            identity_path = temporary / "grandchild.identity"
            marker_path = temporary / "survived.marker"
            grandchild = (
                "import os,signal,time,pathlib;"
                "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                "fields=pathlib.Path('/proc/self/stat').read_text().rpartition(') ')[2].split();"
                f"pathlib.Path({str(identity_path)!r}).write_text("
                "str(os.getpid())+' '+fields[19]);"
                "time.sleep(0.6);"
                f"pathlib.Path({str(marker_path)!r}).write_text('survived');"
                "time.sleep(30)"
            )
            parent = (
                "import subprocess,sys,time;"
                f"subprocess.Popen([sys.executable, '-c', {grandchild!r}]);"
                "time.sleep(30)"
            )

            def interrupting_factory(command, **kwargs):
                process = subprocess.Popen(command, **kwargs)
                deadline = time.monotonic() + 1.0
                while not identity_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                return _InterruptingProcess(process)

            stdout_path = temporary / "stdout.log"
            stderr_path = temporary / "stderr.log"
            try:
                with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                    with self.assertRaises(KeyboardInterrupt):
                        runner._run_process(
                            interrupting_factory,
                            [sys.executable, "-c", parent],
                            temporary,
                            stdout,
                            stderr,
                            2.0,
                        )
                self.assertTrue(identity_path.exists(), "grandchild did not start")
                time.sleep(0.65)
                self.assertFalse(marker_path.exists(), "grandchild survived SIGINT cleanup")
                _assert_test_process_reaped(
                    self, _stored_test_process_identity(identity_path)
                )
            finally:
                _cleanup_test_process(identity_path)

    def test_unsupported_mu_stops_before_creating_output(self) -> None:
        mu_configuration = ExperimentConfiguration(1, 5, "high", "isolated", "ul", "mu")
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            with self.assertRaisesRegex(RunnerError, "unsupported.*mu"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="mu",
                    configurations=(mu_configuration,),
                    process_factory=lambda *args, **kwargs: self.fail("process started"),
                    output=StringIO(),
                )
            self.assertFalse((root / "run/scripted_exp_mu").exists())

    def test_sigint_returns_nonzero_and_retains_created_state(self) -> None:
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            error = StringIO()

            def interrupt_process(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                return _FakeProcess(
                    -signal.SIGTERM,
                    wait_effects=(KeyboardInterrupt(),),
                )

            status = main(
                ["--ns3-root", str(root), "--config", str(DEFAULT_CONFIG)],
                process_factory=interrupt_process,
                timestamp_factory=lambda: "interrupt",
                output=StringIO(),
                error=error,
            )
            run_directory = root / "run/scripted_exp_interrupt"
            self.assertEqual(status, 130)
            self.assertIn("interrupted", error.getvalue().lower())
            self.assertTrue((run_directory / "results.csv").is_file())
            self.assertTrue((run_directory / "experiment_001/attempt_1").is_dir())
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 1)


if __name__ == "__main__":
    unittest.main()
