"""Sequential saturated benchmark runner lifecycle tests."""

from __future__ import annotations

import csv
from io import StringIO
import json
from pathlib import Path
import shlex
import subprocess
from tempfile import TemporaryDirectory
import unittest

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

    def test_discovers_outer_root_and_default_one_repetition(self) -> None:
        self.assertEqual(discover_ns3_root(), OUTER_ROOT)
        loaded = load_runner_configuration(DEFAULT_CONFIG)
        self.assertEqual(loaded.repetitions, 1)
        self.assertEqual(loaded.effective_configuration["script"]["repetitions"], 1)

    def test_repetition_parser_rejects_nonpositive_and_boolean_values(self) -> None:
        original = DEFAULT_CONFIG.read_text(encoding="utf-8")
        for value in ("0", "-1", "true"):
            with self.subTest(value=value), TemporaryDirectory() as directory:
                config_path = Path(directory) / "config.toml"
                config_path.write_text(
                    original.replace("repetitions = 1", f"repetitions = {value}"),
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(RunnerError, "script.repetitions"):
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
            "--benchmark-sta-count-per-bss=5 "
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
                "--benchmark-sta-count-per-bss=5",
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
                try:
                    call_index = len(calls)
                    configuration = configurations[call_index]
                    attempt_directory = (
                        run_directory
                        / f"experiment_{configuration.experiment_id:03d}"
                        / "attempt_1"
                    )
                    if call_index == 1:
                        self.assertEqual(len(read_csv(run_directory / "results.csv")), 4)
                    self.assertEqual(kwargs["cwd"], root)
                    self.assertEqual(kwargs["timeout"], 600)
                    self.assertIs(kwargs["capture_output"], True)
                    self.assertIs(kwargs["text"], True)
                    calls.append((command, kwargs))
                    write_valid_output(
                        attempt_directory / "output.json",
                        configuration,
                        attempt_directory,
                    )
                    return subprocess.CompletedProcess(command, 0, "child stdout", "")
                finally:
                    active -= 1

            result = run_benchmark(
                ns3_root=root,
                config_path=DEFAULT_CONFIG,
                timestamp=timestamp,
                configurations=configurations,
                process_runner=fake_process,
                output=StringIO(),
            )

            self.assertEqual(result, run_directory)
            self.assertEqual(maximum_active, 1)
            self.assertEqual(len(calls), 2)
            self.assertEqual(len(read_csv(run_directory / "results.csv")), 7)
            for configuration in configurations:
                output_path = (
                    run_directory
                    / f"experiment_{configuration.experiment_id:03d}/attempt_1/output.json"
                )
                self.assertTrue(output_path.is_file())
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
                    self.assertEqual(kwargs["timeout"], PROCESS_TIMEOUT_SECONDS)
                    if case == "nonzero":
                        return subprocess.CompletedProcess(command, 9, "out text", "err text")
                    if case == "timeout":
                        raise subprocess.TimeoutExpired(
                            command, kwargs["timeout"], "slow out", "slow err"
                        )
                    if case == "invalid":
                        (attempt_directory / "output.json").write_text("{}", encoding="utf-8")
                    return subprocess.CompletedProcess(command, 0, "", "")

                with self.assertRaises(RunnerError) as caught:
                    run_benchmark(
                        ns3_root=root,
                        config_path=DEFAULT_CONFIG,
                        timestamp=timestamp,
                        configurations=(configuration,),
                        process_runner=fake_process,
                        output=StringIO(),
                    )
                rows = read_csv(root / "run" / f"scripted_exp_{timestamp}/results.csv")
                self.assertEqual(len(rows), 1)
                if case == "nonzero":
                    self.assertIn("out text", str(caught.exception))
                    self.assertIn("err text", str(caught.exception))
                if case == "invalid":
                    self.assertTrue((attempt_directory / "output.json").is_file())
                else:
                    self.assertFalse((attempt_directory / "output.json").exists())

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
                configuration = configurations[calls - 1]
                attempt_directory = (
                    run_directory
                    / f"experiment_{configuration.experiment_id:03d}/attempt_1"
                )
                output_path = attempt_directory / "output.json"
                if calls == 1:
                    write_valid_output(output_path, configuration, attempt_directory)
                    return subprocess.CompletedProcess(command, 0, "", "")
                output_path.write_text('{"failed_attempt":true}', encoding="utf-8")
                return subprocess.CompletedProcess(command, 3, "second out", "second err")

            with self.assertRaisesRegex(RunnerError, "return code 3"):
                run_benchmark(
                    ns3_root=root,
                    config_path=DEFAULT_CONFIG,
                    timestamp=timestamp,
                    configurations=configurations,
                    process_runner=fake_process,
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
                    process_runner=unexpected_process,
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
                return subprocess.CompletedProcess(command, 0, "", "")

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
                    process_runner=colliding_process,
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
                    process_runner=lambda *args, **kwargs: self.fail("process started"),
                    output=StringIO(),
                )
            self.assertFalse((root / "run/scripted_exp_mu").exists())

    def test_sigint_returns_nonzero_and_retains_created_state(self) -> None:
        with TemporaryDirectory() as directory:
            root = create_fake_ns3_root(directory)
            error = StringIO()

            def interrupt_process(command, **kwargs):
                raise KeyboardInterrupt

            status = main(
                ["--ns3-root", str(root), "--config", str(DEFAULT_CONFIG)],
                process_runner=interrupt_process,
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
