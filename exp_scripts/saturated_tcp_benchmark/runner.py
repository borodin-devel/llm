"""Sequential subprocess lifecycle for the saturated TCP benchmark matrix."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Iterable, Sequence
from copy import deepcopy
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tomllib
from typing import TextIO

from .csv_output import ExcelCsvWriter
from .matrix import (
    INTERFERENCE_MODES,
    MIMO_MODES,
    RSSI_RANGES,
    STA_COUNTS,
    TRAFFIC_MODES,
    ExperimentConfiguration,
    build_matrix,
    iter_experiment_attempts,
)
from .validation import OutputValidationError, load_output_document


PROCESS_TIMEOUT_SECONDS = 600
RNG_SEED = 12345
OUTPUT_NAME = "output.json"
DEFAULT_CONFIG_RELATIVE = Path("contrib/llm/config/saturated_tcp_config.toml")

_TIMESTAMP_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+$")
_CONFIGURATION_KEYS = {
    "general": ("output_name", "run_folder"),
    "script": ("repetitions",),
    "simulation": ("rng_seed", "rng_run"),
    "benchmark": (
        "sta_count_per_bss",
        "rssi_range",
        "interference_mode",
        "traffic_mode",
        "mimo_mode",
    ),
    "wifi": (
        "band",
        "channel_number",
        "bandwidth_mhz",
        "primary_20_index",
        "tx_power_dbm",
        "rate_manager",
        "antennas",
        "max_tx_spatial_streams",
        "max_rx_spatial_streams",
    ),
    "tcp": (
        "congestion_control",
        "segment_size_bytes",
        "send_buffer_bytes",
        "receive_buffer_bytes",
        "wired_rate",
        "wired_delay",
    ),
    "statistics": ("window_ms",),
    "logging": ("scenario_level",),
}
_DEFAULT_EFFECTIVE_CONFIGURATION: dict[str, object] = {
    "general": {"output_name": "output.json", "run_folder": None},
    "script": {"repetitions": 1},
    "simulation": {"rng_seed": 12345, "rng_run": 1},
    "benchmark": {
        "sta_count_per_bss": 5,
        "rssi_range": "high",
        "interference_mode": "isolated",
        "traffic_mode": "ul",
        "mimo_mode": "su",
    },
    "wifi": {
        "band": "5GHz",
        "channel_number": 42,
        "bandwidth_mhz": 80,
        "primary_20_index": 0,
        "tx_power_dbm": 20.0,
        "rate_manager": "ns3::MinstrelHtWifiManager",
        "antennas": 2,
        "max_tx_spatial_streams": 2,
        "max_rx_spatial_streams": 2,
    },
    "tcp": {
        "congestion_control": "ns3::TcpHighSpeed",
        "segment_size_bytes": 1460,
        "send_buffer_bytes": 33554432,
        "receive_buffer_bytes": 33554432,
        "wired_rate": "10Gbps",
        "wired_delay": "0.1ms",
    },
    "statistics": {"window_ms": 10},
    "logging": {"scenario_level": "info"},
}


class RunnerError(RuntimeError):
    """A fail-fast runner lifecycle or configuration error."""


@dataclass(frozen=True)
class RunnerConfiguration:
    """Merged TOML values needed for iteration and output validation."""

    repetitions: int
    effective_configuration: dict[str, object]


def discover_ns3_root() -> Path:
    """Return the outer ns-3 root containing this nested runner package."""
    return Path(__file__).resolve().parents[4]


def _timestamp_now() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S_%f")


def _resolve_config_path(config_path: str | Path, ns3_root: Path) -> Path:
    path = Path(config_path)
    if not path.is_absolute():
        path = ns3_root / path
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise RunnerError(f"benchmark config path is unavailable: {path}: {error}") from error
    if not resolved.is_file():
        raise RunnerError(f"benchmark config path is not a regular file: {resolved}")
    return resolved


def load_runner_configuration(config_path: str | Path) -> RunnerConfiguration:
    """Parse and merge the TOML document, including external repetitions."""
    path = Path(config_path)
    try:
        with path.open("rb") as input_file:
            document = tomllib.load(input_file)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise RunnerError(f"cannot parse saturated benchmark TOML {path}: {error}") from error
    if type(document) is not dict:
        raise RunnerError(f"invalid saturated benchmark TOML root in {path}")

    effective = deepcopy(_DEFAULT_EFFECTIVE_CONFIGURATION)
    for section, values in document.items():
        if section not in _CONFIGURATION_KEYS:
            raise RunnerError(f"unknown saturated TOML section {section!r} in {path}")
        if type(values) is not dict:
            raise RunnerError(f"saturated TOML section {section!r} must be a table in {path}")
        for field, value in values.items():
            if field not in _CONFIGURATION_KEYS[section]:
                raise RunnerError(f"unknown saturated TOML field {section}.{field} in {path}")
            effective[section][field] = value

    repetitions = effective["script"]["repetitions"]
    if isinstance(repetitions, bool) or not isinstance(repetitions, int) or repetitions <= 0:
        raise RunnerError(
            f"invalid saturated script.repetitions in {path}: expected a positive integer"
        )
    return RunnerConfiguration(repetitions=repetitions, effective_configuration=effective)


def _validate_requested_configurations(
    configurations: tuple[ExperimentConfiguration, ...],
) -> None:
    if not configurations:
        raise RunnerError("benchmark configuration sequence must not be empty")
    experiment_ids = []
    for configuration in configurations:
        if not isinstance(configuration, ExperimentConfiguration):
            raise RunnerError("benchmark configuration sequence contains an invalid value")
        if configuration.mimo_mode not in MIMO_MODES:
            raise RunnerError(
                f"unsupported benchmark MIMO mode {configuration.mimo_mode!r}: only su is supported"
            )
        if configuration.sta_count_per_bss not in STA_COUNTS:
            raise RunnerError("benchmark sta_count_per_bss is outside the fixed matrix")
        if configuration.rssi_range not in RSSI_RANGES:
            raise RunnerError("benchmark rssi_range is outside the fixed matrix")
        if configuration.interference_mode not in INTERFERENCE_MODES:
            raise RunnerError("benchmark interference_mode is outside the fixed matrix")
        if configuration.traffic_mode not in TRAFFIC_MODES:
            raise RunnerError("benchmark traffic_mode is outside the fixed matrix")
        if isinstance(configuration.experiment_id, bool) or configuration.experiment_id <= 0:
            raise RunnerError("benchmark experiment_id must be a positive integer")
        experiment_ids.append(configuration.experiment_id)
    if len(set(experiment_ids)) != len(experiment_ids):
        raise RunnerError("benchmark experiment IDs must be unique")


def build_ns3_command(
    ns3: str | Path,
    config_path: str | Path,
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    run_directory: str | Path,
) -> list[str]:
    """Build the exact one-process ns-3 wrapper invocation."""
    if configuration.mimo_mode != "su":
        raise RunnerError(
            f"unsupported benchmark MIMO mode {configuration.mimo_mode!r}: only su is supported"
        )
    if (
        isinstance(repetition_attempt, bool)
        or not isinstance(repetition_attempt, int)
        or repetition_attempt <= 0
    ):
        raise RunnerError("repetition_attempt must be a positive integer")
    arguments = [
        "saturated-tcp-scenario",
        "--config",
        str(config_path),
        f"--benchmark-sta-count-per-bss={configuration.sta_count_per_bss}",
        f"--benchmark-rssi-range={configuration.rssi_range}",
        f"--benchmark-interference-mode={configuration.interference_mode}",
        f"--benchmark-traffic-mode={configuration.traffic_mode}",
        f"--benchmark-mimo-mode={configuration.mimo_mode}",
        f"--simulation-rng-seed={RNG_SEED}",
        f"--simulation-rng-run={repetition_attempt}",
        f"--general-run-folder={run_directory}",
        f"--general-output-name={OUTPUT_NAME}",
    ]
    return [str(ns3), "run", shlex.join(arguments)]


def _expected_configuration(
    loaded: RunnerConfiguration,
    configuration: ExperimentConfiguration,
    repetition_attempt: int,
    run_directory: Path,
) -> dict[str, object]:
    expected = deepcopy(loaded.effective_configuration)
    expected["general"]["output_name"] = OUTPUT_NAME
    expected["general"]["run_folder"] = str(run_directory)
    expected["simulation"]["rng_seed"] = RNG_SEED
    expected["simulation"]["rng_run"] = repetition_attempt
    expected["benchmark"].update(
        {
            "sta_count_per_bss": configuration.sta_count_per_bss,
            "rssi_range": configuration.rssi_range,
            "interference_mode": configuration.interference_mode,
            "traffic_mode": configuration.traffic_mode,
            "mimo_mode": configuration.mimo_mode,
        }
    )
    return expected


def _diagnostic_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def _child_failure(
    message: str,
    command: Sequence[str],
    stdout: object = None,
    stderr: object = None,
) -> RunnerError:
    details = [message, f"command: {command!r}"]
    stdout_text = _diagnostic_text(stdout)
    stderr_text = _diagnostic_text(stderr)
    if stdout_text:
        details.append(f"stdout:\n{stdout_text}")
    if stderr_text:
        details.append(f"stderr:\n{stderr_text}")
    return RunnerError("\n".join(details))


def _create_directory_exclusively(path: Path, description: str) -> None:
    try:
        path.mkdir()
    except FileExistsError as error:
        raise RunnerError(f"{description} already exists: {path}") from error
    except OSError as error:
        raise RunnerError(f"cannot create {description} {path}: {error}") from error


def run_benchmark(
    *,
    ns3_root: str | Path,
    config_path: str | Path,
    timestamp: str | None = None,
    configurations: Iterable[ExperimentConfiguration] | None = None,
    process_runner: Callable[..., object] = subprocess.run,
    output: TextIO | None = None,
) -> Path:
    """Run configurations sequentially and retain every created artifact."""
    output = output if output is not None else sys.stdout
    try:
        root = Path(ns3_root).resolve(strict=True)
    except OSError as error:
        raise RunnerError(f"ns-3 root is unavailable: {ns3_root}: {error}") from error
    if not root.is_dir():
        raise RunnerError(f"ns-3 root is not a directory: {root}")
    ns3 = root / "ns3"
    if not ns3.is_file():
        raise RunnerError(f"ns-3 wrapper is not a regular file: {ns3}")
    resolved_config = _resolve_config_path(config_path, root)
    loaded = load_runner_configuration(resolved_config)
    requested = tuple(configurations) if configurations is not None else build_matrix()
    _validate_requested_configurations(requested)

    timestamp = timestamp if timestamp is not None else _timestamp_now()
    if (
        not isinstance(timestamp, str)
        or not timestamp
        or not _TIMESTAMP_PATTERN.fullmatch(timestamp)
    ):
        raise RunnerError(f"invalid benchmark timestamp component: {timestamp!r}")
    run_parent = root / "run"
    try:
        run_parent.mkdir(exist_ok=True)
    except OSError as error:
        raise RunnerError(f"cannot prepare benchmark run parent {run_parent}: {error}") from error
    if not run_parent.is_dir():
        raise RunnerError(f"benchmark run parent is not a directory: {run_parent}")
    run_directory = run_parent / f"scripted_exp_{timestamp}"
    _create_directory_exclusively(run_directory, "benchmark timestamp directory")

    attempt_count = len(requested) * loaded.repetitions
    completed = 0
    created_experiments: set[int] = set()
    try:
        csv_output = ExcelCsvWriter(run_directory / "results.csv")
    except FileExistsError as error:
        raise RunnerError(
            f"benchmark CSV already exists: {run_directory / 'results.csv'}"
        ) from error
    except OSError as error:
        raise RunnerError(f"cannot create benchmark CSV: {error}") from error

    with csv_output:
        for attempt in iter_experiment_attempts(requested, loaded.repetitions):
            configuration = attempt.configuration
            experiment_directory = run_directory / f"experiment_{configuration.experiment_id:03d}"
            if configuration.experiment_id not in created_experiments:
                _create_directory_exclusively(experiment_directory, "experiment directory")
                created_experiments.add(configuration.experiment_id)
            attempt_directory = experiment_directory / f"attempt_{attempt.repetition_attempt}"
            _create_directory_exclusively(attempt_directory, "attempt directory")
            output_path = attempt_directory / OUTPUT_NAME
            if output_path.exists():
                raise RunnerError(f"benchmark output already exists: {output_path}")

            command = build_ns3_command(
                ns3,
                resolved_config,
                configuration,
                attempt.repetition_attempt,
                attempt_directory,
            )
            print(
                f"[{completed + 1}/{attempt_count}] experiment "
                f"{configuration.experiment_id:03d}, attempt {attempt.repetition_attempt}",
                file=output,
                flush=True,
            )
            try:
                result = process_runner(
                    command,
                    cwd=root,
                    timeout=PROCESS_TIMEOUT_SECONDS,
                    capture_output=True,
                    text=True,
                )
            except subprocess.TimeoutExpired as error:
                raise _child_failure(
                    f"benchmark process timed out after {PROCESS_TIMEOUT_SECONDS} seconds",
                    command,
                    error.stdout,
                    error.stderr,
                ) from error
            return_code = getattr(result, "returncode", None)
            if not isinstance(return_code, int):
                raise RunnerError("benchmark process runner returned no integer return code")
            if return_code != 0:
                raise _child_failure(
                    f"benchmark process failed with return code {return_code}",
                    command,
                    getattr(result, "stdout", None),
                    getattr(result, "stderr", None),
                )
            if not output_path.is_file():
                raise RunnerError(f"successful benchmark process did not create {output_path}")

            expected_configuration = _expected_configuration(
                loaded,
                configuration,
                attempt.repetition_attempt,
                attempt_directory,
            )
            try:
                rows = load_output_document(
                    output_path,
                    configuration,
                    attempt.repetition_attempt,
                    expected_configuration=expected_configuration,
                )
            except OutputValidationError as error:
                raise RunnerError(f"benchmark output validation failed: {error}") from error
            csv_output.append_attempt(rows)
            completed += 1

    print(f"Completed {completed} attempts: {run_directory}", file=output, flush=True)
    return run_directory


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the complete sequential saturated TCP benchmark matrix.",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG_RELATIVE,
        help=f"benchmark TOML path (default: {DEFAULT_CONFIG_RELATIVE})",
    )
    parser.add_argument(
        "--ns3-root",
        type=Path,
        default=None,
        help="outer ns-3 project root (default: discovered from this script)",
    )
    return parser


def main(
    argv: Sequence[str] | None = None,
    *,
    process_runner: Callable[..., object] = subprocess.run,
    timestamp_factory: Callable[[], str] = _timestamp_now,
    output: TextIO | None = None,
    error: TextIO | None = None,
) -> int:
    """Run the CLI and map failures or SIGINT to nonzero exit statuses."""
    output = output if output is not None else sys.stdout
    error = error if error is not None else sys.stderr
    arguments = _argument_parser().parse_args(argv)
    root = arguments.ns3_root if arguments.ns3_root is not None else discover_ns3_root()
    try:
        run_benchmark(
            ns3_root=root,
            config_path=arguments.config,
            timestamp=timestamp_factory(),
            process_runner=process_runner,
            output=output,
        )
    except KeyboardInterrupt:
        print("error: saturated benchmark interrupted; completed data was retained", file=error)
        return 130
    except RunnerError as runner_error:
        print(f"error: {runner_error}", file=error)
        return 1
    return 0
