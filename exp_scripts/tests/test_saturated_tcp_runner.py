"""Resource-aware saturated benchmark runner lifecycle tests."""

from __future__ import annotations

import _thread
import csv
from concurrent.futures import Future, ThreadPoolExecutor as RealThreadPoolExecutor
from dataclasses import replace
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
import threading
import time
import unittest
from unittest import mock

from saturated_tcp_benchmark import resources
from saturated_tcp_benchmark import runner
from saturated_tcp_benchmark.csv_output import BssCsvRow, StationCsvMetrics
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
from saturated_tcp_benchmark.resources import (
    LINUX_PROC_MONITOR_MODE,
    MemorySnapshot,
    ResourceCapability,
    ResourceMeasurement,
    detect_resource_capability,
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


def _process_group_exists(process_group_id: int) -> bool:
    try:
        os.killpg(process_group_id, 0)
    except ProcessLookupError:
        return False
    return True


def _cleanup_real_process_group(process: subprocess.Popen) -> None:
    if process.poll() is None or _process_group_exists(process.pid):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.wait(timeout=1.0)


def create_fake_ns3_root(directory: str) -> Path:
    """Create the runner's minimum outer-root filesystem contract."""
    root = Path(directory)
    ns3 = root / "ns3"
    ns3.write_text(
        "#!/bin/sh\n[ \"$1\" = build ] && exit 0\nexit 99\n",
        encoding="ascii",
    )
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


def read_json(path: Path) -> dict[str, object]:
    """Read one retained resource document without changing key order."""
    return json.loads(path.read_text(encoding="utf-8"))


def fallback_resource_capability(directory: str | Path):
    """Return deterministic no-proc capability for fake-process runner tests."""
    missing = Path(directory) / "missing-proc"
    return detect_resource_capability(
        proc_root=missing,
        meminfo_path=missing / "meminfo",
    )


class SaturatedTcpRunnerTest(unittest.TestCase):
    """Protect commands, ordered parallel publication, failure, and retention."""

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
        self.assertEqual(
            loaded.effective_configuration["benchmark"]["traffic_warmup_seconds"], 0
        )
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

    def test_traffic_warmup_parser_enforces_exact_uint32_range(self) -> None:
        original = DEFAULT_CONFIG.read_text(encoding="utf-8")
        with TemporaryDirectory() as directory:
            maximum_path = Path(directory) / "maximum.toml"
            maximum_path.write_text(
                original.replace(
                    "traffic_warmup_seconds = 0",
                    "traffic_warmup_seconds = 4294967295",
                ),
                encoding="utf-8",
            )
            loaded = load_runner_configuration(maximum_path)
            self.assertEqual(
                loaded.effective_configuration["benchmark"]["traffic_warmup_seconds"],
                4294967295,
            )

        for value in ("-1", "true", "4294967296"):
            with self.subTest(value=value), TemporaryDirectory() as directory:
                config_path = Path(directory) / "config.toml"
                config_path.write_text(
                    original.replace(
                        "traffic_warmup_seconds = 0",
                        f"traffic_warmup_seconds = {value}",
                    ),
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(
                    RunnerError,
                    "benchmark.traffic_warmup_seconds.*uint32",
                ):
                    load_runner_configuration(config_path)

    def test_builds_exact_ns3_command_and_deterministic_paths(self) -> None:
        configuration = replace(build_matrix()[0], traffic_warmup_seconds=5)
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
            "--benchmark-traffic-warmup-seconds=5 "
            "--simulation-rng-seed=12345 "
            "--simulation-rng-run=2 "
            f"--general-run-folder={attempt_directory} "
            "--general-output-name=output.json"
        )
        self.assertEqual(
            command,
            [str(root / "ns3"), "run", "--no-build", expected_command_string],
        )
        self.assertEqual(
            shlex.split(command[3]),
            [
                "saturated-tcp-scenario",
                "--config",
                str(DEFAULT_CONFIG),
                "--benchmark-sta-count-per-bss=1",
                "--benchmark-rssi-range=high",
                "--benchmark-interference-mode=isolated",
                "--benchmark-traffic-mode=ul",
                "--benchmark-mimo-mode=su",
                "--benchmark-traffic-warmup-seconds=5",
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
                resource_capability=fallback_resource_capability(directory),
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

    def test_parallel_controller_orders_results_and_is_the_only_csv_owner(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "parallel_order"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            id_three_finished = threading.Event()
            process_calls = []
            completion_order = []
            build_calls = []

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    self.root_pid = root_pid

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        sample_interval_ms=100,
                        peak_rss_bytes=1_000,
                        minimum_mem_available_bytes=90_000,
                        minimum_mem_available_percent=90.0,
                        wall_time_seconds=0.01,
                        exit_code=exit_code,
                        monitor_mode=LINUX_PROC_MONITOR_MODE,
                    )

            class GateProcess(_FakeProcess):
                def __init__(self, experiment_id):
                    super().__init__(0)
                    self.pid = 900_000 + experiment_id
                    self.experiment_id = experiment_id

                def wait(self, timeout=None):
                    if self.experiment_id == 2:
                        if not id_three_finished.wait(2.0):
                            raise AssertionError("experiment 2 did not overlap experiment 3")
                    elif self.experiment_id == 3:
                        id_three_finished.set()
                    completion_order.append(self.experiment_id)
                    return super().wait(timeout)

            def fake_process(command, **kwargs):
                self._assert_popen_kwargs(kwargs, root)
                command_arguments = shlex.split(command[-1])
                run_folder = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in command_arguments
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(run_folder.parent.name.split("_", 1)[1])
                configuration = configurations[experiment_id - 1]
                write_valid_output(
                    run_folder / "output.json",
                    configuration,
                    run_folder,
                )
                process_calls.append(experiment_id)
                return GateProcess(experiment_id)

            capability = ResourceCapability(
                monitor_mode=LINUX_PROC_MONITOR_MODE,
                proc_root=Path("/proc"),
                meminfo_path=Path("/proc/meminfo"),
                initial_memory_snapshot=MemorySnapshot(100_000, 90_000),
                diagnostic="test Linux proc capability",
            )
            append_thread_ids = []
            appended_keys = []
            original_append = runner.ExcelCsvWriter.append_attempt

            def record_append(writer, rows):
                rows = tuple(rows)
                append_thread_ids.append(threading.get_ident())
                appended_keys.append(
                    (
                        rows[0].configuration.experiment_id,
                        rows[0].repetition_attempt,
                        len(rows),
                    )
                )
                original_append(writer, rows)

            controller_thread = threading.get_ident()
            with mock.patch.object(
                runner.ExcelCsvWriter,
                "append_attempt",
                new=record_append,
            ):
                result = run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_factory=fake_process,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: MemorySnapshot(100_000, 90_000),
                    active_rss_reader=lambda process_ids: tuple(0 for _ in process_ids),
                    logical_cpu_count=8,
                    jobs=2,
                    build_runner=lambda command, cwd: build_calls.append(
                        (tuple(command), cwd)
                    )
                    or 0,
                    output=StringIO(),
                )

            self.assertEqual(result, run_directory)
            self.assertEqual(
                build_calls,
                [
                    (
                        (str(root / "ns3"), "build", "saturated-tcp-scenario"),
                        root,
                    )
                ],
            )
            self.assertEqual(process_calls[0], 1, "subset calibration runs first")
            self.assertEqual(process_calls.count(1), 1, "calibration result is reused")
            self.assertEqual(completion_order[:3], [1, 3, 2])
            self.assertEqual(appended_keys, [(1, 1, 3), (2, 1, 3), (3, 1, 3)])
            self.assertEqual(append_thread_ids, [controller_thread] * 3)
            self.assertEqual(
                [row[0] for row in read_csv(run_directory / "results.csv")[1:]],
                ["1"] * 3 + ["2"] * 3 + ["3"] * 3,
            )
            summary = read_json(run_directory / "resource_summary.json")
            self.assertEqual(summary["calibrated_peak_rss_bytes"], 1_000)
            self.assertEqual(summary["worker_peak_estimate_bytes"], 1_250)
            self.assertEqual(summary["maximum_parallel_workers"], 2)
            self.assertEqual(
                [record["experiment_id"] for record in summary["attempts"]],
                [1, 2, 3],
            )

    def test_full_synthetic_run_calibrates_126_once_and_reuses_buffered_result(self) -> None:
        timestamp = "full_synthetic_calibration"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            process_calls = []

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                if experiment_id == 1:
                    self.assertEqual(
                        len(read_csv(run_directory / "results.csv")),
                        1,
                        "calibration 126 remains buffered behind canonical ID 1",
                    )
                if experiment_id == 19:
                    self.assertEqual(
                        len(read_csv(run_directory / "results.csv")),
                        55,
                        "all 18 one-STA baselines publish before dependents",
                    )
                process_calls.append(experiment_id)
                (attempt_directory / "output.json").write_text("{}", encoding="utf-8")
                process = _FakeProcess(0)
                process.pid = 940_000 + experiment_id
                return process

            def validated_rows(
                output_path,
                configuration,
                repetition_attempt,
                *,
                expected_configuration,
            ):
                metric = StationCsvMetrics(100.0, 1.0, 50.0, 1.0, 0.98, "profile")
                stations = (
                    (metric,) * configuration.sta_count_per_bss
                    + (None,) * (30 - configuration.sta_count_per_bss)
                )
                return tuple(
                    BssCsvRow(
                        configuration,
                        repetition_attempt,
                        -41.5 if configuration.rssi_range == "high" else -50.0,
                        bss_id,
                        100.0,
                        50.0,
                        float(configuration.sta_count_per_bss),
                        None,
                        stations,
                    )
                    for bss_id in range(3)
                )

            with mock.patch.object(
                runner,
                "load_output_document",
                side_effect=validated_rows,
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    process_factory=fake_process,
                    resource_capability=fallback_resource_capability(directory),
                    output=StringIO(),
                )

            self.assertEqual(process_calls[0], 126)
            self.assertEqual(process_calls.count(126), 1)
            self.assertEqual(process_calls[1:19], list(range(1, 19)))
            self.assertEqual(process_calls[19:], list(range(19, 126)))
            published = read_csv(run_directory / "results.csv")
            self.assertEqual(len(published), 379)
            self.assertEqual(
                [published[index][0] for index in range(1, 379, 3)],
                [str(experiment_id) for experiment_id in range(1, 127)],
            )

    def test_csv_fsync_failure_rolls_back_the_uncommitted_three_row_batch(self) -> None:
        configuration = build_matrix()[0]
        timestamp = "csv_fsync_failure"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"

            def valid_process(command, **kwargs):
                attempt_directory = run_directory / "experiment_001/attempt_1"
                write_valid_output(
                    attempt_directory / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(0)

            original_synchronize = runner.ExcelCsvWriter._synchronize
            synchronize_calls = 0

            def fail_append_synchronize(writer):
                nonlocal synchronize_calls
                synchronize_calls += 1
                if synchronize_calls == 1:
                    original_synchronize(writer)
                    return
                raise OSError("injected append fsync failure")

            with (
                mock.patch.object(
                    runner.ExcelCsvWriter,
                    "_synchronize",
                    new=fail_append_synchronize,
                ),
                self.assertRaisesRegex(RunnerError, "append.*CSV|CSV.*append"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=(configuration,),
                    process_factory=valid_process,
                    resource_capability=fallback_resource_capability(directory),
                    output=StringIO(),
                )

            self.assertEqual(len(read_csv(run_directory / "results.csv")), 1)
            self.assertTrue(
                (run_directory / "experiment_001/attempt_1/output.json").is_file()
            )
            self.assertTrue(
                (
                    run_directory
                    / "experiment_001/attempt_1/resource_usage.json"
                ).is_file()
            )

    def test_parallel_failure_interrupt_and_timeout_stop_every_active_group(self) -> None:
        configurations = build_matrix()[:3]
        cases = ("failure", "interrupt", "timeout")
        for case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                root = create_fake_ns3_root(directory)
                timestamp = f"parallel_stop_{case}"
                run_directory = root / "run" / f"scripted_exp_{timestamp}"
                sibling_started = Path(directory) / "sibling.started"
                real_processes = []

                class FixedMonitor:
                    def __init__(self, root_pid, capability):
                        pass

                    def start(self):
                        pass

                    def finish(self, exit_code):
                        return ResourceMeasurement(
                            sample_interval_ms=100,
                            peak_rss_bytes=1_000,
                            minimum_mem_available_bytes=90_000,
                            minimum_mem_available_percent=90.0,
                            wall_time_seconds=0.01,
                            exit_code=exit_code,
                            monitor_mode=LINUX_PROC_MONITOR_MODE,
                        )

                def fake_process(command, **kwargs):
                    command_arguments = shlex.split(command[-1])
                    attempt_directory = Path(
                        next(
                            argument.split("=", 1)[1]
                            for argument in command_arguments
                            if argument.startswith("--general-run-folder=")
                        )
                    )
                    experiment_id = int(
                        attempt_directory.parent.name.split("_", 1)[1]
                    )
                    if experiment_id == 1:
                        write_valid_output(
                            attempt_directory / "output.json",
                            configurations[0],
                            attempt_directory,
                        )
                        process = _FakeProcess(0)
                        process.pid = 910_001
                        return process
                    (attempt_directory / "output.json").write_text(
                        json.dumps({"retained_experiment_id": experiment_id}),
                        encoding="utf-8",
                    )
                    if experiment_id == 2:
                        sibling_script = (
                            "import pathlib,signal,time;"
                            "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                            f"pathlib.Path({str(sibling_started)!r}).write_text('started');"
                            "time.sleep(30)"
                        )
                        process = subprocess.Popen(
                            [sys.executable, "-c", sibling_script],
                            **kwargs,
                        )
                        real_processes.append(process)
                        return process
                    deadline = time.monotonic() + 2.0
                    while not sibling_started.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    if not sibling_started.exists():
                        raise AssertionError("active sibling process did not start")
                    if case == "failure":
                        process = _FakeProcess(7)
                    elif case == "interrupt":
                        process = _FakeProcess(
                            -signal.SIGTERM,
                            wait_effects=(KeyboardInterrupt(),),
                        )
                    else:
                        process = _FakeProcess(
                            -signal.SIGTERM,
                            wait_effects=(
                                subprocess.TimeoutExpired(
                                    command,
                                    PROCESS_TIMEOUT_SECONDS,
                                ),
                            ),
                        )
                    process.pid = 910_003
                    return process

                capability = ResourceCapability(
                    monitor_mode=LINUX_PROC_MONITOR_MODE,
                    proc_root=Path("/proc"),
                    meminfo_path=Path("/proc/meminfo"),
                    initial_memory_snapshot=MemorySnapshot(100_000, 90_000),
                    diagnostic="test Linux proc capability",
                )
                caught = None
                try:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=configurations,
                        process_factory=fake_process,
                        build_runner=lambda command, cwd: 0,
                        resource_capability=capability,
                        resource_monitor_factory=FixedMonitor,
                        memory_snapshot_reader=lambda: MemorySnapshot(
                            100_000,
                            90_000,
                        ),
                        active_rss_reader=lambda process_ids: tuple(
                            0 for _ in process_ids
                        ),
                        logical_cpu_count=8,
                        jobs=2,
                        output=StringIO(),
                    )
                except BaseException as error:
                    caught = error
                finally:
                    for process in real_processes:
                        _cleanup_real_process_group(process)

                self.assertIsNotNone(caught)
                if case == "interrupt":
                    self.assertIsInstance(caught, KeyboardInterrupt)
                else:
                    self.assertIsInstance(caught, RunnerError)
                    expected = "return code 7" if case == "failure" else "timed out"
                    self.assertIn(expected, str(caught))
                self.assertEqual(len(real_processes), 1)
                self.assertIsNotNone(real_processes[0].returncode)
                self.assertFalse(_process_group_exists(real_processes[0].pid))
                self.assertEqual(len(read_csv(run_directory / "results.csv")), 4)
                for experiment_id in (1, 2, 3):
                    attempt_directory = (
                        run_directory
                        / f"experiment_{experiment_id:03d}/attempt_1"
                    )
                    self.assertTrue((attempt_directory / "stdout.log").is_file())
                    self.assertTrue((attempt_directory / "stderr.log").is_file())
                    self.assertTrue((attempt_directory / "resource_usage.json").is_file())
                summary = read_json(run_directory / "resource_summary.json")
                self.assertEqual(
                    [record["experiment_id"] for record in summary["attempts"]],
                    [1, 2, 3],
                )

    def test_admission_reserves_growth_for_submitted_worker_before_popen_registers(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "launching_growth_reserve"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            id_two_launching = threading.Event()
            release_id_two = threading.Event()
            id_three_started = threading.Event()
            controller_errors = []

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        sample_interval_ms=100,
                        peak_rss_bytes=1_000,
                        minimum_mem_available_bytes=4_000,
                        minimum_mem_available_percent=40.0,
                        wall_time_seconds=0.01,
                        exit_code=exit_code,
                        monitor_mode=LINUX_PROC_MONITOR_MODE,
                    )

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                if experiment_id == 2:
                    id_two_launching.set()
                    if not release_id_two.wait(2.0):
                        raise AssertionError("test did not release experiment 2 Popen")
                elif experiment_id == 3:
                    id_three_started.set()
                write_valid_output(
                    attempt_directory / "output.json",
                    configurations[experiment_id - 1],
                    attempt_directory,
                )
                process = _FakeProcess(0)
                process.pid = 920_000 + experiment_id
                return process

            capability = ResourceCapability(
                monitor_mode=LINUX_PROC_MONITOR_MODE,
                proc_root=Path("/proc"),
                meminfo_path=Path("/proc/meminfo"),
                initial_memory_snapshot=MemorySnapshot(10_000, 4_000),
                diagnostic="test Linux proc capability",
            )

            def run_controller():
                try:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=configurations,
                        process_factory=fake_process,
                        build_runner=lambda command, cwd: 0,
                        resource_capability=capability,
                        resource_monitor_factory=FixedMonitor,
                        memory_snapshot_reader=lambda: MemorySnapshot(10_000, 4_000),
                        active_rss_reader=lambda process_ids: tuple(
                            0 for _ in process_ids
                        ),
                        logical_cpu_count=8,
                        jobs=2,
                        output=StringIO(),
                    )
                except BaseException as error:
                    controller_errors.append(error)

            controller = threading.Thread(target=run_controller)
            controller.start()
            self.assertTrue(id_two_launching.wait(2.0))
            admitted_too_early = id_three_started.wait(0.2)
            release_id_two.set()
            controller.join(3.0)

            self.assertFalse(controller.is_alive())
            self.assertEqual(controller_errors, [])
            self.assertFalse(
                admitted_too_early,
                "unregistered submitted work still consumes one peak estimate",
            )
            self.assertTrue(id_three_started.is_set())

    def test_admission_uses_memavailable_sample_after_live_rss_growth(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "rss_then_memory"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            growth_sampled = threading.Event()
            id_two_waiting = threading.Event()
            release_id_two = threading.Event()
            id_three_started = threading.Event()
            controller_errors = []

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        4_400,
                        44.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            class WaitingProcess(_FakeProcess):
                def wait(self, timeout=None):
                    id_two_waiting.set()
                    if not release_id_two.wait(3.0):
                        raise AssertionError("test did not release experiment 2")
                    return super().wait(timeout)

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                write_valid_output(
                    attempt_directory / "output.json",
                    configurations[experiment_id - 1],
                    attempt_directory,
                )
                if experiment_id == 2:
                    process = WaitingProcess(0)
                else:
                    process = _FakeProcess(0)
                if experiment_id == 3:
                    id_three_started.set()
                process.pid = 985_000 + experiment_id
                return process

            def read_active_rss(process_ids):
                if not process_ids:
                    return ()
                if not id_two_waiting.wait(2.0):
                    raise AssertionError("active worker did not begin waiting")
                growth_sampled.set()
                return (1_000,)

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(10_000, 4_400),
                "test Linux proc capability",
            )

            def run_controller():
                try:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=configurations,
                        process_factory=fake_process,
                        build_runner=lambda command, cwd: 0,
                        resource_capability=capability,
                        resource_monitor_factory=FixedMonitor,
                        memory_snapshot_reader=lambda: MemorySnapshot(
                            10_000,
                            3_499 if growth_sampled.is_set() else 4_400,
                        ),
                        active_rss_reader=read_active_rss,
                        logical_cpu_count=8,
                        jobs=2,
                        output=StringIO(),
                    )
                except BaseException as error:
                    controller_errors.append(error)

            controller = threading.Thread(target=run_controller)
            controller.start()
            self.assertTrue(id_two_waiting.wait(2.0))
            self.assertTrue(growth_sampled.wait(2.0))
            admitted_from_stale_snapshot = id_three_started.wait(0.2)
            release_id_two.set()
            controller.join(3.0)

            self.assertFalse(controller.is_alive())
            self.assertEqual(controller_errors, [])
            self.assertFalse(admitted_from_stale_snapshot)
            self.assertTrue(id_three_started.is_set())

    def test_experiment_id_parser_and_cli_forward_exact_scheduler_controls(self) -> None:
        self.assertEqual(runner.parse_experiment_ids("19, 37,126"), (19, 37, 126))
        for invalid in ("", "19,", "19,19", "0", "127", "true"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    runner.parse_experiment_ids(invalid)

        with mock.patch.object(
            runner,
            "run_benchmark",
            return_value=Path("/unused"),
        ) as run:
            status = main(
                [
                    "--ns3-root",
                    str(OUTER_ROOT),
                    "--jobs",
                    "4",
                    "--memory-reserve-percent",
                    "25",
                    "--experiment-ids",
                    "19,37,126",
                ],
                timestamp_factory=lambda: "cli_controls",
                output=StringIO(),
                error=StringIO(),
            )
        self.assertEqual(status, 0)
        keyword_arguments = run.call_args.kwargs
        self.assertEqual(keyword_arguments["jobs"], 4)
        self.assertEqual(keyword_arguments["memory_reserve_percent"], 25)
        self.assertEqual(keyword_arguments["experiment_ids"], (19, 37, 126))

    def test_subset_auto_includes_baseline_and_records_exact_manifest(self) -> None:
        timestamp = "subset_manifest"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            process_calls = []

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                (attempt_directory / "output.json").write_text(
                    "{}",
                    encoding="utf-8",
                )
                process = _FakeProcess(0)
                process.pid = 930_000 + experiment_id
                return process

            def validated_rows(
                output_path,
                configuration,
                repetition_attempt,
                *,
                expected_configuration,
            ):
                metric = StationCsvMetrics(100.0, 1.0, 50.0, 1.0, 0.98, "profile")
                stations = (
                    (metric,) * configuration.sta_count_per_bss
                    + (None,) * (30 - configuration.sta_count_per_bss)
                )
                return tuple(
                    BssCsvRow(
                        configuration=configuration,
                        repetition_attempt=repetition_attempt,
                        target_rssi_dbm=-41.5,
                        bss_id=bss_id,
                        mean_dominant_data_phy_rate_mbps=100.0,
                        mean_effective_phy_rate_mbps=50.0,
                        aggregate_data_tx_rate_over_interval_mbps=float(
                            configuration.sta_count_per_bss
                        ),
                        competition_overhead_vs_single_sta=None,
                        stations=stations,
                    )
                    for bss_id in range(3)
                )

            with mock.patch.object(
                runner,
                "load_output_document",
                side_effect=validated_rows,
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    experiment_ids=(19,),
                    process_factory=fake_process,
                    resource_capability=fallback_resource_capability(directory),
                    output=StringIO(),
                )

            self.assertEqual(process_calls, [1, 19])
            summary = read_json(run_directory / "resource_summary.json")
            self.assertIs(summary["complete_matrix"], False)
            self.assertEqual(summary["requested_experiment_ids"], [19])
            self.assertEqual(summary["executed_experiment_ids"], [1, 19])
            self.assertEqual(summary["auto_included_baseline_ids"], [1])
            self.assertEqual(
                [row[0] for row in read_csv(run_directory / "results.csv")[1:]],
                ["1"] * 3 + ["19"] * 3,
            )

    def test_explicit_all_ids_remains_subset_while_no_filter_is_complete(self) -> None:
        all_ids = tuple(range(1, 127))
        explicit = runner._select_run(None, all_ids)
        default = runner._select_run(None, None)

        self.assertIs(explicit.complete_matrix, False)
        self.assertEqual(explicit.requested_experiment_ids, all_ids)
        self.assertEqual(explicit.executed_experiment_ids, all_ids)
        self.assertEqual(explicit.auto_included_baseline_ids, ())
        self.assertIs(default.complete_matrix, True)
        self.assertEqual(default.executed_experiment_ids, all_ids)

    def test_build_failure_launches_no_simulation_worker(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            simulation_calls = []
            build_calls = []

            with self.assertRaisesRegex(RunnerError, "build.*return code 9"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp="build_failure",
                    configurations=(configuration,),
                    process_factory=lambda command, **kwargs: simulation_calls.append(
                        command
                    ),
                    build_runner=lambda command, cwd: build_calls.append(
                        (tuple(command), cwd)
                    )
                    or 9,
                    resource_capability=fallback_resource_capability(directory),
                    output=StringIO(),
                )

            self.assertEqual(
                build_calls,
                [
                    (
                        (str(root / "ns3"), "build", "saturated-tcp-scenario"),
                        root,
                    )
                ],
            )
            self.assertEqual(simulation_calls, [])
            self.assertFalse((root / "run").exists())

    def test_completed_failure_is_consumed_before_any_further_admission(self) -> None:
        configurations = build_matrix()[:5]
        timestamp = "prompt_failure_stop"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            process_calls = []

            class ImmediateExecutor:
                def __init__(self, **kwargs):
                    pass

                def submit(self, function, **kwargs):
                    future = Future()
                    try:
                        future.set_result(function(**kwargs))
                    except BaseException as error:
                        future.set_exception(error)
                    return future

                def shutdown(self, **kwargs):
                    pass

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        90_000,
                        90.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                if experiment_id == 1:
                    write_valid_output(
                        attempt_directory / "output.json",
                        configurations[0],
                        attempt_directory,
                    )
                process = _FakeProcess(0 if experiment_id == 1 else 7)
                process.pid = 950_000 + experiment_id
                return process

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(100_000, 90_000),
                "test Linux proc capability",
            )
            with (
                mock.patch.object(runner, "ThreadPoolExecutor", ImmediateExecutor),
                self.assertRaisesRegex(RunnerError, "return code 7"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_factory=fake_process,
                    build_runner=lambda command, cwd: 0,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: MemorySnapshot(100_000, 90_000),
                    active_rss_reader=lambda process_ids: tuple(
                        0 for _ in process_ids
                    ),
                    logical_cpu_count=8,
                    jobs=4,
                    output=StringIO(),
                )

            self.assertEqual(process_calls, [1, 2])
            self.assertFalse((run_directory / "experiment_003").exists())

    def test_failure_queued_while_rss_reader_blocks_gates_next_submission(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "blocked_reader_failure"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            allow_failure = threading.Event()
            failure_queued = threading.Event()
            process_calls = []

            class FutureProxy:
                def __init__(self, future, experiment_id):
                    self._future = future
                    self._experiment_id = experiment_id

                def add_done_callback(self, callback):
                    def observed_callback(_future):
                        callback(self)
                        if self._experiment_id == 2:
                            failure_queued.set()

                    self._future.add_done_callback(observed_callback)

                def cancel(self):
                    return self._future.cancel()

                def cancelled(self):
                    return self._future.cancelled()

                def result(self):
                    return self._future.result()

            class ObservedExecutor:
                def __init__(self, **kwargs):
                    self._executor = RealThreadPoolExecutor(**kwargs)

                def submit(self, function, **kwargs):
                    experiment_id = kwargs["context"].attempt.configuration.experiment_id
                    return FutureProxy(
                        self._executor.submit(function, **kwargs),
                        experiment_id,
                    )

                def shutdown(self, **kwargs):
                    self._executor.shutdown(**kwargs)

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        4_400,
                        44.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            class ReleasedFailureProcess(_FakeProcess):
                def wait(self, timeout=None):
                    if not allow_failure.wait(3.0):
                        raise AssertionError("RSS reader did not release failure")
                    return super().wait(timeout)

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                if experiment_id != 2:
                    write_valid_output(
                        attempt_directory / "output.json",
                        configurations[experiment_id - 1],
                        attempt_directory,
                    )
                if experiment_id == 2:
                    process = ReleasedFailureProcess(7)
                else:
                    process = _FakeProcess(0)
                process.pid = 986_000 + experiment_id
                return process

            def blocking_active_rss(process_ids):
                if not process_ids:
                    return ()
                allow_failure.set()
                if not failure_queued.wait(3.0):
                    raise AssertionError("worker failure was not queued")
                return (1_000,)

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(10_000, 4_400),
                "test Linux proc capability",
            )
            with (
                mock.patch.object(runner, "ThreadPoolExecutor", ObservedExecutor),
                self.assertRaisesRegex(RunnerError, "return code 7"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_factory=fake_process,
                    build_runner=lambda command, cwd: 0,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: MemorySnapshot(10_000, 4_400),
                    active_rss_reader=blocking_active_rss,
                    logical_cpu_count=8,
                    jobs=2,
                    output=StringIO(),
                )

            self.assertEqual(process_calls, [1, 2])
            self.assertFalse(
                (root / "run" / f"scripted_exp_{timestamp}/experiment_003").exists()
            )

    def test_queued_higher_peak_success_restarts_admission_decision(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "higher_peak_resample"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            allow_success = threading.Event()
            success_queued = threading.Event()
            process_calls = []

            class FutureProxy:
                def __init__(self, future, experiment_id):
                    self._future = future
                    self._experiment_id = experiment_id

                def add_done_callback(self, callback):
                    def observed_callback(_future):
                        callback(self)
                        if self._experiment_id == 2:
                            success_queued.set()

                    self._future.add_done_callback(observed_callback)

                def cancel(self):
                    return self._future.cancel()

                def cancelled(self):
                    return self._future.cancelled()

                def result(self):
                    return self._future.result()

            class ObservedExecutor:
                def __init__(self, **kwargs):
                    self._executor = RealThreadPoolExecutor(**kwargs)

                def submit(self, function, **kwargs):
                    experiment_id = kwargs["context"].attempt.configuration.experiment_id
                    return FutureProxy(
                        self._executor.submit(function, **kwargs),
                        experiment_id,
                    )

                def shutdown(self, **kwargs):
                    self._executor.shutdown(**kwargs)

            class PeakMonitor:
                def __init__(self, root_pid, capability):
                    self._root_pid = root_pid

                def start(self):
                    pass

                def finish(self, exit_code):
                    peak = 4_000 if self._root_pid == 989_002 else 1_000
                    return ResourceMeasurement(
                        100,
                        peak,
                        4_000,
                        40.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            class ReleasedSuccessProcess(_FakeProcess):
                def wait(self, timeout=None):
                    if not allow_success.wait(3.0):
                        raise AssertionError("RSS reader did not release success")
                    return super().wait(timeout)

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                write_valid_output(
                    attempt_directory / "output.json",
                    configurations[experiment_id - 1],
                    attempt_directory,
                )
                if experiment_id == 2:
                    process = ReleasedSuccessProcess(0)
                else:
                    process = _FakeProcess(0)
                process.pid = 989_000 + experiment_id
                return process

            def blocking_active_rss(process_ids):
                if not process_ids:
                    return ()
                allow_success.set()
                if not success_queued.wait(3.0):
                    raise AssertionError("worker success was not queued")
                return (1_000,)

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(10_000, 4_000),
                "test Linux proc capability",
            )
            caught = None
            with mock.patch.object(runner, "ThreadPoolExecutor", ObservedExecutor):
                try:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=configurations,
                        process_factory=fake_process,
                        build_runner=lambda command, cwd: 0,
                        resource_capability=capability,
                        resource_monitor_factory=PeakMonitor,
                        memory_snapshot_reader=lambda: MemorySnapshot(10_000, 4_000),
                        active_rss_reader=blocking_active_rss,
                        logical_cpu_count=8,
                        jobs=2,
                        output=StringIO(),
                    )
                except RunnerError as error:
                    caught = error

            self.assertIsNotNone(caught)
            self.assertIn("insufficient available memory", str(caught))
            self.assertEqual(process_calls, [1, 2])
            summary = read_json(
                root / "run" / f"scripted_exp_{timestamp}/resource_summary.json"
            )
            self.assertEqual(summary["worker_peak_estimate_bytes"], 5_000)

    def test_no_active_worker_with_unsatisfied_reserve_fails_without_waiting(self) -> None:
        configurations = build_matrix()[:2]
        timestamp = "no_active_memory"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            process_calls = []
            snapshots = iter(
                (
                    MemorySnapshot(10_000, 9_000),
                    MemorySnapshot(10_000, 2_000),
                )
            )

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        9_000,
                        90.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                write_valid_output(
                    attempt_directory / "output.json",
                    configurations[experiment_id - 1],
                    attempt_directory,
                )
                process = _FakeProcess(0)
                process.pid = 960_000 + experiment_id
                return process

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(10_000, 9_000),
                "test Linux proc capability",
            )
            started = time.monotonic()
            with self.assertRaisesRegex(RunnerError, "insufficient available memory"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_factory=fake_process,
                    build_runner=lambda command, cwd: 0,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: next(snapshots),
                    active_rss_reader=lambda process_ids: (),
                    logical_cpu_count=8,
                    jobs=2,
                    output=StringIO(),
                )
            self.assertLess(time.monotonic() - started, 1.0)
            self.assertEqual(process_calls, [1])

    def test_process_born_after_registry_stop_self_terminates_and_is_reaped(self) -> None:
        with TemporaryDirectory() as directory:
            temporary = Path(directory)
            stdout_path = temporary / "stdout.log"
            stderr_path = temporary / "stderr.log"
            factory_entered = threading.Event()
            allow_popen = threading.Event()
            processes = []
            errors = []
            registry = runner._ActiveProcessRegistry()

            def delayed_factory(command, **kwargs):
                factory_entered.set()
                if not allow_popen.wait(2.0):
                    raise AssertionError("test did not release delayed Popen")
                process = subprocess.Popen(
                    [
                        sys.executable,
                        "-c",
                        "import signal,time;"
                        "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                        "time.sleep(30)",
                    ],
                    **kwargs,
                )
                processes.append(process)
                return process

            def launch_after_stop(stdout, stderr):
                try:
                    registry.launch(
                        delayed_factory,
                        ["unused"],
                        cwd=temporary,
                        stdout=stdout,
                        stderr=stderr,
                        start_new_session=True,
                    )
                except BaseException as error:
                    errors.append(error)

            try:
                with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                    launcher = threading.Thread(
                        target=launch_after_stop,
                        args=(stdout, stderr),
                    )
                    launcher.start()
                    self.assertTrue(factory_entered.wait(2.0))
                    registry.stop()
                    allow_popen.set()
                    launcher.join(2.0)
                self.assertFalse(launcher.is_alive())
                self.assertEqual(len(processes), 1)
                self.assertEqual(len(errors), 1)
                self.assertIsInstance(errors[0], runner._AdmissionsStopped)
                self.assertIsNotNone(processes[0].returncode)
                self.assertFalse(_process_group_exists(processes[0].pid))
            finally:
                allow_popen.set()
                for process in processes:
                    _cleanup_real_process_group(process)

    def test_failure_sets_sticky_stop_before_cancelling_queued_future(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "stop_before_cancel"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            id_three_submitted = threading.Event()
            process_calls = []

            class DeferredFuture:
                def __init__(self, function, kwargs):
                    self._function = function
                    self._kwargs = kwargs
                    self._callbacks = []
                    self._done = False
                    self._result = None
                    self._error = None

                def add_done_callback(self, callback):
                    if self._done:
                        callback(self)
                    else:
                        self._callbacks.append(callback)

                def cancel(self):
                    if self._done:
                        return False
                    try:
                        self._result = self._function(**self._kwargs)
                    except BaseException as error:
                        self._error = error
                    self._done = True
                    for callback in self._callbacks:
                        callback(self)
                    return False

                def cancelled(self):
                    return False

                def result(self):
                    if self._error is not None:
                        raise self._error
                    return self._result

            class DeferredExecutor:
                def __init__(self, **kwargs):
                    self._executor = RealThreadPoolExecutor(**kwargs)

                def submit(self, function, **kwargs):
                    experiment_id = kwargs["context"].attempt.configuration.experiment_id
                    if experiment_id == 3:
                        future = DeferredFuture(function, kwargs)
                        id_three_submitted.set()
                        return future
                    return self._executor.submit(function, **kwargs)

                def shutdown(self, **kwargs):
                    self._executor.shutdown(**kwargs)

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        90_000,
                        90.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            class FailureAfterQueueProcess(_FakeProcess):
                def wait(self, timeout=None):
                    if not id_three_submitted.wait(3.0):
                        raise AssertionError("experiment 3 was not queued")
                    return super().wait(timeout)

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                if experiment_id != 2:
                    write_valid_output(
                        attempt_directory / "output.json",
                        configurations[experiment_id - 1],
                        attempt_directory,
                    )
                if experiment_id == 2:
                    process = FailureAfterQueueProcess(7)
                else:
                    process = _FakeProcess(0)
                process.pid = 987_000 + experiment_id
                return process

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(100_000, 90_000),
                "test Linux proc capability",
            )
            with (
                mock.patch.object(runner, "ThreadPoolExecutor", DeferredExecutor),
                self.assertRaisesRegex(RunnerError, "return code 7"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_factory=fake_process,
                    build_runner=lambda command, cwd: 0,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: MemorySnapshot(100_000, 90_000),
                    active_rss_reader=lambda process_ids: tuple(
                        0 for _ in process_ids
                    ),
                    logical_cpu_count=8,
                    jobs=2,
                    output=StringIO(),
                )

            self.assertEqual(process_calls, [1, 2])

    def test_controller_sigint_breaks_completion_wait_and_stops_active_group(self) -> None:
        configuration = build_matrix()[0]
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            marker = Path(directory) / "active.marker"
            processes = []
            timers = []

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        90_000,
                        90.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            def real_process(command, **kwargs):
                script = (
                    "import pathlib,signal,time;"
                    "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
                    f"pathlib.Path({str(marker)!r}).write_text('active');"
                    "time.sleep(30)"
                )
                process = subprocess.Popen(
                    [sys.executable, "-c", script],
                    **kwargs,
                )
                processes.append(process)
                deadline = time.monotonic() + 1.0
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not marker.exists():
                    raise AssertionError("real worker did not reach completion wait")
                timer = threading.Timer(0.05, _thread.interrupt_main)
                timers.append(timer)
                timer.start()
                return process

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(100_000, 90_000),
                "test Linux proc capability",
            )
            started = time.monotonic()
            try:
                with self.assertRaises(KeyboardInterrupt):
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp="controller_sigint",
                        configurations=(configuration,),
                        process_factory=real_process,
                        build_runner=lambda command, cwd: 0,
                        resource_capability=capability,
                        resource_monitor_factory=FixedMonitor,
                        memory_snapshot_reader=lambda: MemorySnapshot(
                            100_000,
                            90_000,
                        ),
                        active_rss_reader=lambda process_ids: tuple(
                            0 for _ in process_ids
                        ),
                        logical_cpu_count=8,
                        jobs=2,
                        output=StringIO(),
                    )
                self.assertLess(time.monotonic() - started, 1.0)
                self.assertEqual(len(processes), 1)
                self.assertIsNotNone(processes[0].returncode)
                self.assertFalse(_process_group_exists(processes[0].pid))
            finally:
                for timer in timers:
                    timer.cancel()
                    timer.join()
                for process in processes:
                    _cleanup_real_process_group(process)

    def test_complete_run_below_acceptance_floor_finishes_healthy_work_then_fails(self) -> None:
        matrix = build_matrix()
        selected = (matrix[17], matrix[125])
        selection = runner._RunSelection(
            configurations=selected,
            requested_experiment_ids=(18, 126),
            executed_experiment_ids=(18, 126),
            auto_included_baseline_ids=(),
            complete_matrix=True,
        )
        timestamp = "acceptance_floor"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            process_calls = []

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        14_000,
                        14.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                process_calls.append(experiment_id)
                (attempt_directory / "output.json").write_text("{}", encoding="utf-8")
                process = _FakeProcess(0)
                process.pid = 970_000 + experiment_id
                return process

            def validated_rows(
                output_path,
                configuration,
                repetition_attempt,
                *,
                expected_configuration,
            ):
                metric = StationCsvMetrics(100.0, 1.0, 50.0, 1.0, 0.98, "profile")
                stations = (
                    (metric,) * configuration.sta_count_per_bss
                    + (None,) * (30 - configuration.sta_count_per_bss)
                )
                return tuple(
                    BssCsvRow(
                        configuration,
                        repetition_attempt,
                        -60.0,
                        bss_id,
                        100.0,
                        50.0,
                        float(configuration.sta_count_per_bss),
                        None,
                        stations,
                    )
                    for bss_id in range(3)
                )

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(100_000, 90_000),
                "test Linux proc capability",
            )
            with (
                mock.patch.object(runner, "_select_run", return_value=selection),
                mock.patch.object(
                    runner,
                    "load_output_document",
                    side_effect=validated_rows,
                ),
                self.assertRaisesRegex(RunnerError, "15 percent.*acceptance floor"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    process_factory=fake_process,
                    build_runner=lambda command, cwd: 0,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: MemorySnapshot(100_000, 90_000),
                    active_rss_reader=lambda process_ids: tuple(
                        0 for _ in process_ids
                    ),
                    logical_cpu_count=8,
                    jobs=2,
                    output=StringIO(),
                )

            self.assertEqual(process_calls, [126, 18])
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 7)
            summary = read_json(run_directory / "resource_summary.json")
            self.assertIs(summary["complete_matrix"], True)
            self.assertEqual(summary["minimum_mem_available_percent"], 14.0)
            self.assertEqual(
                [record["exit_code"] for record in summary["attempts"]],
                [0, 0],
            )

    def test_resource_summary_includes_controller_memory_snapshot_minimum(self) -> None:
        matrix = build_matrix()
        selected = (matrix[17], matrix[125])
        selection = runner._RunSelection(
            configurations=selected,
            requested_experiment_ids=(18, 126),
            executed_experiment_ids=(18, 126),
            auto_included_baseline_ids=(),
            complete_matrix=True,
        )
        timestamp = "controller_memory_minimum"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            snapshots = iter(
                (
                    MemorySnapshot(10_000, 9_000),
                    MemorySnapshot(10_000, 1_400),
                )
            )

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        9_000,
                        90.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            def calibration_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                (attempt_directory / "output.json").write_text("{}", encoding="utf-8")
                process = _FakeProcess(0)
                process.pid = 988_126
                return process

            def validated_rows(
                output_path,
                configuration,
                repetition_attempt,
                *,
                expected_configuration,
            ):
                metric = StationCsvMetrics(100.0, 1.0, 50.0, 1.0, 0.98, "profile")
                return tuple(
                    BssCsvRow(
                        configuration,
                        repetition_attempt,
                        -60.0,
                        bss_id,
                        100.0,
                        50.0,
                        30.0,
                        None,
                        (metric,) * 30,
                    )
                    for bss_id in range(3)
                )

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(10_000, 9_000),
                "test Linux proc capability",
            )
            with (
                mock.patch.object(runner, "_select_run", return_value=selection),
                mock.patch.object(
                    runner,
                    "load_output_document",
                    side_effect=validated_rows,
                ),
                self.assertRaisesRegex(RunnerError, "insufficient available memory"),
            ):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    process_factory=calibration_process,
                    build_runner=lambda command, cwd: 0,
                    resource_capability=capability,
                    resource_monitor_factory=FixedMonitor,
                    memory_snapshot_reader=lambda: next(snapshots),
                    active_rss_reader=lambda process_ids: (),
                    logical_cpu_count=8,
                    jobs=2,
                    output=StringIO(),
                )

            summary = read_json(run_directory / "resource_summary.json")
            self.assertEqual(summary["minimum_mem_available_bytes"], 1_400)
            self.assertEqual(summary["minimum_mem_available_percent"], 14.0)
            self.assertEqual(summary["attempts"][0]["minimum_mem_available_percent"], 90.0)

    def test_runtime_floor_breach_pauses_then_resumes_without_killing_active_work(self) -> None:
        configurations = build_matrix()[:3]
        timestamp = "floor_pause_resume"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            id_two_waiting = threading.Event()
            release_id_two = threading.Event()
            memory_recovered = threading.Event()
            id_three_started = threading.Event()
            controller_errors = []

            class FixedMonitor:
                def __init__(self, root_pid, capability):
                    pass

                def start(self):
                    pass

                def finish(self, exit_code):
                    return ResourceMeasurement(
                        100,
                        1_000,
                        1_400,
                        14.0,
                        0.01,
                        exit_code,
                        LINUX_PROC_MONITOR_MODE,
                    )

            class WaitingProcess(_FakeProcess):
                def wait(self, timeout=None):
                    id_two_waiting.set()
                    if not release_id_two.wait(3.0):
                        raise AssertionError("test did not release healthy active work")
                    return super().wait(timeout)

            snapshot_calls = 0

            def memory_snapshot():
                nonlocal snapshot_calls
                snapshot_calls += 1
                if snapshot_calls <= 2 or memory_recovered.is_set():
                    return MemorySnapshot(10_000, 9_000)
                return MemorySnapshot(10_000, 1_400)

            def fake_process(command, **kwargs):
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                write_valid_output(
                    attempt_directory / "output.json",
                    configurations[experiment_id - 1],
                    attempt_directory,
                )
                if experiment_id == 2:
                    process = WaitingProcess(0)
                else:
                    process = _FakeProcess(0)
                if experiment_id == 3:
                    id_three_started.set()
                process.pid = 980_000 + experiment_id
                return process

            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(10_000, 9_000),
                "test Linux proc capability",
            )

            def run_controller():
                try:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=configurations,
                        process_factory=fake_process,
                        build_runner=lambda command, cwd: 0,
                        resource_capability=capability,
                        resource_monitor_factory=FixedMonitor,
                        memory_snapshot_reader=memory_snapshot,
                        active_rss_reader=lambda process_ids: tuple(
                            0 for _ in process_ids
                        ),
                        logical_cpu_count=8,
                        jobs=2,
                        output=StringIO(),
                    )
                except BaseException as error:
                    controller_errors.append(error)

            controller = threading.Thread(target=run_controller)
            controller.start()
            self.assertTrue(id_two_waiting.wait(2.0))
            self.assertFalse(id_three_started.wait(0.2))
            self.assertFalse(release_id_two.is_set())
            memory_recovered.set()
            self.assertTrue(id_three_started.wait(2.0))
            release_id_two.set()
            controller.join(3.0)

            self.assertFalse(controller.is_alive())
            self.assertEqual(controller_errors, [])
            results_path = root / "run" / f"scripted_exp_{timestamp}/results.csv"
            self.assertEqual(len(read_csv(results_path)), 10)

    def test_retains_exact_ordered_attempt_records_and_sequential_root_summary(self) -> None:
        configurations = tuple(reversed(build_matrix()[:2]))
        timestamp = "resources_success"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            calls = 0

            def fake_process(command, **kwargs):
                nonlocal calls
                calls += 1
                attempt_directory = Path(
                    next(
                        argument.split("=", 1)[1]
                        for argument in shlex.split(command[-1])
                        if argument.startswith("--general-run-folder=")
                    )
                )
                experiment_id = int(
                    attempt_directory.parent.name.split("_", 1)[1]
                )
                configuration = build_matrix()[experiment_id - 1]
                write_valid_output(
                    attempt_directory / "output.json",
                    configuration,
                    attempt_directory,
                )
                return _FakeProcess(0)

            run_benchmark(
                ns3_root=root,
                config_path=DEFAULT_CONFIG,
                timestamp=timestamp,
                configurations=configurations,
                process_factory=fake_process,
                resource_capability=fallback_resource_capability(directory),
                output=StringIO(),
            )

            attempt_records = []
            expected_attempt_keys = [
                "schema_version",
                "experiment_id",
                "repetition_attempt",
                "sample_interval_ms",
                "peak_rss_bytes",
                "minimum_mem_available_bytes",
                "minimum_mem_available_percent",
                "wall_time_seconds",
                "exit_code",
                "monitor_mode",
            ]
            for experiment_id in (1, 2):
                record = read_json(
                    run_directory
                    / f"experiment_{experiment_id:03d}/attempt_1/resource_usage.json"
                )
                self.assertEqual(list(record), expected_attempt_keys)
                self.assertEqual(
                    {
                        key: record[key]
                        for key in expected_attempt_keys
                        if key != "wall_time_seconds"
                    },
                    {
                        "schema_version": 1,
                        "experiment_id": experiment_id,
                        "repetition_attempt": 1,
                        "sample_interval_ms": 100,
                        "peak_rss_bytes": None,
                        "minimum_mem_available_bytes": None,
                        "minimum_mem_available_percent": None,
                        "exit_code": 0,
                        "monitor_mode": "sequential_fallback",
                    },
                )
                self.assertGreaterEqual(record["wall_time_seconds"], 0.0)
                attempt_records.append(record)

            summary = read_json(run_directory / "resource_summary.json")
            self.assertEqual(
                list(summary),
                [
                    "schema_version",
                    "complete_matrix",
                    "requested_experiment_ids",
                    "executed_experiment_ids",
                    "auto_included_baseline_ids",
                    "memory_reserve_percent",
                    "calibrated_peak_rss_bytes",
                    "worker_peak_estimate_bytes",
                    "maximum_parallel_workers",
                    "minimum_mem_available_bytes",
                    "minimum_mem_available_percent",
                    "attempts",
                ],
            )
            self.assertEqual(
                {key: summary[key] for key in summary if key != "attempts"},
                {
                    "schema_version": 1,
                    "complete_matrix": False,
                    "requested_experiment_ids": [2, 1],
                    "executed_experiment_ids": [1, 2],
                    "auto_included_baseline_ids": [],
                    "memory_reserve_percent": 20,
                    "calibrated_peak_rss_bytes": None,
                    "worker_peak_estimate_bytes": None,
                    "maximum_parallel_workers": 1,
                    "minimum_mem_available_bytes": None,
                    "minimum_mem_available_percent": None,
                },
            )
            self.assertEqual(summary["attempts"], attempt_records)

    def test_calibration_failure_before_popen_reports_zero_parallel_workers(self) -> None:
        configuration = build_matrix()[0]
        timestamp = "calibration_prelaunch_failure"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            capability = ResourceCapability(
                LINUX_PROC_MONITOR_MODE,
                Path("/proc"),
                Path("/proc/meminfo"),
                MemorySnapshot(100_000, 90_000),
                "test Linux proc capability",
            )
            process_calls = []

            def fail_snapshot():
                raise resources.ResourceError("injected calibration snapshot failure")

            with self.assertRaisesRegex(RunnerError, "calibration snapshot failure"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=(configuration,),
                    process_factory=lambda *args, **kwargs: process_calls.append(args),
                    resource_capability=capability,
                    memory_snapshot_reader=fail_snapshot,
                    active_rss_reader=lambda process_ids: (),
                    output=StringIO(),
                )

            summary = read_json(
                root / f"run/scripted_exp_{timestamp}/resource_summary.json"
            )
            self.assertEqual(process_calls, [])
            self.assertEqual(summary["attempts"], [])
            self.assertEqual(summary["maximum_parallel_workers"], 0)

    def test_retains_resource_records_and_partial_summary_on_process_failures(self) -> None:
        configuration = build_matrix()[0]
        cases = (
            ("nonzero", _FakeProcess(9), 9, "return code 9"),
            (
                "timeout",
                _FakeProcess(
                    -signal.SIGTERM,
                    wait_effects=(
                        subprocess.TimeoutExpired(["fake"], PROCESS_TIMEOUT_SECONDS),
                    ),
                ),
                -signal.SIGTERM,
                "timed out",
            ),
        )
        for case, process, expected_exit_code, diagnostic in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                root = create_fake_ns3_root(directory)
                run_directory = root / "run" / f"scripted_exp_resource_{case}"
                with self.assertRaisesRegex(RunnerError, diagnostic):
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=f"resource_{case}",
                        configurations=(configuration,),
                        process_factory=lambda command, **kwargs: process,
                        resource_capability=fallback_resource_capability(directory),
                        output=StringIO(),
                    )

                attempt_record = read_json(
                    run_directory / "experiment_001/attempt_1/resource_usage.json"
                )
                self.assertEqual(attempt_record["exit_code"], expected_exit_code)
                self.assertEqual(attempt_record["monitor_mode"], "sequential_fallback")
                summary = read_json(run_directory / "resource_summary.json")
                self.assertEqual(summary["attempts"], [attempt_record])

    def test_interrupt_retains_attempt_resource_and_partial_summary(self) -> None:
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            error = StringIO()
            status = main(
                [
                    "--ns3-root",
                    str(root),
                    "--config",
                    str(DEFAULT_CONFIG),
                    "--experiment-ids",
                    "1",
                ],
                process_factory=lambda command, **kwargs: _FakeProcess(
                    -signal.SIGTERM,
                    wait_effects=(KeyboardInterrupt(),),
                ),
                resource_capability=fallback_resource_capability(directory),
                timestamp_factory=lambda: "resource_interrupt",
                output=StringIO(),
                error=error,
            )
            run_directory = root / "run/scripted_exp_resource_interrupt"
            attempt_record = read_json(
                run_directory / "experiment_001/attempt_1/resource_usage.json"
            )
            summary = read_json(run_directory / "resource_summary.json")

            self.assertEqual(status, 130)
            self.assertEqual(attempt_record["exit_code"], -signal.SIGTERM)
            self.assertEqual(summary["attempts"], [attempt_record])

    def test_resource_symlink_collisions_never_touch_outside_targets(self) -> None:
        configuration = build_matrix()[0]
        for resource_name in ("resource_usage.json", "resource_summary.json"):
            with self.subTest(resource_name=resource_name), TemporaryDirectory() as directory:
                root = create_fake_ns3_root(directory)
                run_directory = root / "run/scripted_exp_resource_symlink"
                attempt_directory = run_directory / "experiment_001/attempt_1"
                outside = Path(directory) / "outside.json"
                outside.write_text('{"sentinel":true}', encoding="ascii")

                def fake_process(command, **kwargs):
                    collision_parent = (
                        attempt_directory
                        if resource_name == "resource_usage.json"
                        else run_directory
                    )
                    (collision_parent / resource_name).symlink_to(outside)
                    write_valid_output(
                        attempt_directory / "output.json",
                        configuration,
                        attempt_directory,
                    )
                    return _FakeProcess(0)

                with self.assertRaisesRegex(RunnerError, "resource.*exists|exists.*resource"):
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp="resource_symlink",
                        configurations=(configuration,),
                        process_factory=fake_process,
                        resource_capability=fallback_resource_capability(directory),
                        output=StringIO(),
                    )
                self.assertEqual(outside.read_text(encoding="ascii"), '{"sentinel":true}')

    def test_monitor_start_failures_terminate_and_reap_process(self) -> None:
        cases = (
            (RuntimeError("injected thread start failure"), RunnerError),
            (KeyboardInterrupt(), KeyboardInterrupt),
        )
        for start_error, expected_error in cases:
            with (
                self.subTest(start_error=type(start_error).__name__),
                TemporaryDirectory() as directory,
            ):
                temporary = Path(directory)
                stdout_path = temporary / "stdout.log"
                stderr_path = temporary / "stderr.log"
                processes = []

                def process_factory(command, **kwargs):
                    process = subprocess.Popen(command, **kwargs)
                    processes.append(process)
                    return process

                original_start = resources.threading.Thread.start

                def start_then_fail(thread):
                    original_start(thread)
                    raise start_error

                try:
                    with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                        with (
                            mock.patch.object(
                                resources.threading.Thread,
                                "start",
                                new=start_then_fail,
                            ),
                            self.assertRaises(expected_error),
                        ):
                            runner._run_process(
                                process_factory,
                                [sys.executable, "-c", "import time; time.sleep(30)"],
                                temporary,
                                stdout,
                                stderr,
                                2.0,
                                resource_capability=runner.detect_resource_capability(),
                            )

                    self.assertEqual(len(processes), 1)
                    self.assertIsNotNone(
                        processes[0].poll(),
                        "start failure leaked direct child",
                    )
                    self.assertFalse(_process_group_exists(processes[0].pid))
                finally:
                    for process in processes:
                        _cleanup_real_process_group(process)

    def test_primary_failures_survive_overlapping_resource_retention_errors(self) -> None:
        configuration = build_matrix()[0]
        cases = ("timeout_attempt", "nonzero_summary", "interrupt_both")
        for case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                root = create_fake_ns3_root(directory)
                run_directory = root / "run" / f"scripted_exp_overlap_{case}"
                attempt_directory = run_directory / "experiment_001/attempt_1"
                outside = Path(directory) / "outside.json"
                outside.write_text('{"sentinel":true}', encoding="ascii")

                def failing_process(command, **kwargs):
                    if case in ("timeout_attempt", "interrupt_both"):
                        (attempt_directory / "resource_usage.json").symlink_to(outside)
                    if case in ("nonzero_summary", "interrupt_both"):
                        (run_directory / "resource_summary.json").symlink_to(outside)
                    if case == "timeout_attempt":
                        return _FakeProcess(
                            -signal.SIGTERM,
                            wait_effects=(
                                subprocess.TimeoutExpired(
                                    command,
                                    PROCESS_TIMEOUT_SECONDS,
                                ),
                            ),
                        )
                    if case == "nonzero_summary":
                        return _FakeProcess(7)
                    return _FakeProcess(
                        -signal.SIGTERM,
                        wait_effects=(KeyboardInterrupt(),),
                    )

                if case == "interrupt_both":
                    error = StringIO()
                    status = main(
                        [
                            "--ns3-root",
                            str(root),
                            "--config",
                            str(DEFAULT_CONFIG),
                            "--experiment-ids",
                            "1",
                        ],
                        process_factory=failing_process,
                        resource_capability=fallback_resource_capability(directory),
                        timestamp_factory=lambda: f"overlap_{case}",
                        output=StringIO(),
                        error=error,
                    )
                    self.assertEqual(status, 130)
                    self.assertIn("interrupted", error.getvalue().lower())
                else:
                    diagnostic = "timed out" if case == "timeout_attempt" else "return code 7"
                    with self.assertRaisesRegex(RunnerError, diagnostic):
                        run_benchmark(
                            ns3_root=root,
                            config_path=DEFAULT_CONFIG,
                            timestamp=f"overlap_{case}",
                            configurations=(configuration,),
                            process_factory=failing_process,
                            resource_capability=fallback_resource_capability(directory),
                            output=StringIO(),
                        )
                self.assertEqual(outside.read_text(encoding="ascii"), '{"sentinel":true}')

    def test_summary_publishes_before_csv_close_and_close_is_secondary(self) -> None:
        configuration = build_matrix()[0]
        timestamp = "csv_close_secondary"
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            run_directory = root / "run" / f"scripted_exp_{timestamp}"
            close_started = False
            summary_published_before_close = []
            original_close = runner.ExcelCsvWriter.close
            original_publish = runner._publish_resource_document

            def failing_close(writer):
                nonlocal close_started
                close_started = True
                original_close(writer)
                raise OSError("injected CSV close failure")

            def observe_summary_publish(path, document, description, parent_identity):
                if path.name == "resource_summary.json":
                    summary_published_before_close.append(not close_started)
                return original_publish(path, document, description, parent_identity)

            caught = None
            with (
                mock.patch.object(
                    runner.ExcelCsvWriter,
                    "close",
                    new=failing_close,
                ),
                mock.patch.object(
                    runner,
                    "_publish_resource_document",
                    new=observe_summary_publish,
                ),
            ):
                try:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=(configuration,),
                        process_factory=lambda command, **kwargs: _FakeProcess(7),
                        resource_capability=fallback_resource_capability(directory),
                        output=StringIO(),
                    )
                except BaseException as error:
                    caught = error

            self.assertIsInstance(caught, RunnerError)
            self.assertIn("return code 7", str(caught))
            self.assertTrue(
                any(
                    "secondary CSV close failure" in note
                    and "injected CSV close failure" in note
                    for note in getattr(caught, "__notes__", ())
                )
            )
            self.assertEqual(summary_published_before_close, [True])
            self.assertTrue((run_directory / "resource_summary.json").is_file())

    def test_attempt_collision_is_secondary_to_nonzero_and_invalid_output(self) -> None:
        configuration = build_matrix()[0]
        cases = ("nonzero", "invalid")
        for case in cases:
            with self.subTest(case=case), TemporaryDirectory() as directory:
                root = create_fake_ns3_root(directory)
                run_directory = root / "run" / f"scripted_exp_attempt_primary_{case}"
                attempt_directory = run_directory / "experiment_001/attempt_1"
                outside = Path(directory) / "outside.json"
                outside.write_text('{"sentinel":true}', encoding="ascii")

                def failing_process(command, **kwargs):
                    (attempt_directory / "resource_usage.json").symlink_to(outside)
                    if case == "invalid":
                        (attempt_directory / "output.json").write_text("{}", encoding="utf-8")
                        return _FakeProcess(0)
                    return _FakeProcess(7)

                diagnostic = (
                    "return code 7"
                    if case == "nonzero"
                    else "output validation failed"
                )
                with self.assertRaisesRegex(RunnerError, diagnostic):
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=f"attempt_primary_{case}",
                        configurations=(configuration,),
                        process_factory=failing_process,
                        resource_capability=fallback_resource_capability(directory),
                        output=StringIO(),
                    )
                self.assertEqual(outside.read_text(encoding="ascii"), '{"sentinel":true}')

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
                    resource_capability=fallback_resource_capability(directory),
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
                    resource_capability=fallback_resource_capability(directory),
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
            processes = []

            def process_factory(command, **kwargs):
                process = subprocess.Popen(command, **kwargs)
                processes.append(process)
                return process

            started = time.monotonic()
            try:
                with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                    with self.assertRaisesRegex(RunnerError, "timed out"):
                        runner._run_process(
                            process_factory,
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
                for process in processes:
                    _cleanup_real_process_group(process)
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
            processes = []

            def process_factory(command, **kwargs):
                process = subprocess.Popen(command, **kwargs)
                processes.append(process)
                return process

            started = time.monotonic()
            try:
                with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                    with self.assertRaisesRegex(RunnerError, "descendant"):
                        runner._run_process(
                            process_factory,
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
                for process in processes:
                    _cleanup_real_process_group(process)
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
            processes = []

            def interrupting_factory(command, **kwargs):
                process = subprocess.Popen(command, **kwargs)
                processes.append(process)
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
                for process in processes:
                    _cleanup_real_process_group(process)
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
                [
                    "--ns3-root",
                    str(root),
                    "--config",
                    str(DEFAULT_CONFIG),
                    "--experiment-ids",
                    "1",
                ],
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
