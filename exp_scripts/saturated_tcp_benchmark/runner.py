"""Sequential subprocess lifecycle for the saturated TCP benchmark matrix."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Iterable, Sequence
from contextlib import contextmanager
from copy import deepcopy
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
import os
from pathlib import Path
import re
import signal
import shlex
import stat
import subprocess
import sys
import time
import tomllib
from typing import BinaryIO, Iterator, TextIO

from .csv_output import BaselineKey, ExcelCsvWriter, apply_matching_baseline
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
from .resources import (
    ProcessTreeResourceMonitor,
    ResourceCapability,
    ResourceError,
    ResourceMeasurement,
    build_attempt_resource_record,
    build_sequential_resource_summary,
    detect_resource_capability,
    publish_json_exclusive,
)
from .validation import OutputValidationError, load_output_document


PROCESS_TIMEOUT_SECONDS = 600
TERM_GRACE_SECONDS = 0.2
KILL_GRACE_SECONDS = 0.2
DIAGNOSTIC_TAIL_BYTES = 8192
RNG_SEED = 12345
OUTPUT_NAME = "output.json"
DEFAULT_CONFIG_RELATIVE = Path("contrib/llm/config/saturated_tcp_config.toml")

_TIMESTAMP_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+$")
_UINT32_MAX = (1 << 32) - 1
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
        "guard_interval_ns",
        "rts_cts_threshold_bytes",
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
        "sta_count_per_bss": 1,
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
        "guard_interval_ns": 3200,
        "rts_cts_threshold_bytes": 0,
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


class _ProcessGroupState(Enum):
    """Sticky result of one stored-PGID operation."""

    PRESENT = "present"
    ABSENT = "absent"


@dataclass(frozen=True)
class _ObjectIdentity:
    """Pinned filesystem object identity from lstat or fstat."""

    device: int
    inode: int


def _object_identity(status: os.stat_result) -> _ObjectIdentity:
    return _ObjectIdentity(status.st_dev, status.st_ino)


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
    if type(repetitions) is not int or not 1 <= repetitions <= _UINT32_MAX:
        raise RunnerError(
            f"invalid saturated script.repetitions in {path}: expected uint32 in "
            f"[1, {_UINT32_MAX}]"
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


def _signal_process_group(
    process_group_id: int, signal_number: int
) -> _ProcessGroupState:
    """Signal one stored group and report whether it existed at that operation."""
    try:
        os.killpg(process_group_id, signal_number)
    except ProcessLookupError:
        return _ProcessGroupState.ABSENT
    return _ProcessGroupState.PRESENT


def _probe_process_group(process_group_id: int) -> _ProcessGroupState:
    """Probe one stored process group and return an explicit sticky state."""
    try:
        os.killpg(process_group_id, 0)
    except ProcessLookupError:
        return _ProcessGroupState.ABSENT
    return _ProcessGroupState.PRESENT


def _wait_for_process_group_absence(
    process_group_id: int,
    timeout: float,
    initial_state: _ProcessGroupState,
) -> _ProcessGroupState:
    """Return whether members remain, never probing again after observing absence."""
    deadline = time.monotonic() + timeout
    state = initial_state
    while state is _ProcessGroupState.PRESENT:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.01, remaining))
        state = _probe_process_group(process_group_id)
    return state


def _terminate_process_group(process: object, process_group_id: int) -> None:
    """TERM a dedicated group, KILL survivors, then reap the direct child."""
    state = _signal_process_group(process_group_id, signal.SIGTERM)
    if state is _ProcessGroupState.PRESENT:
        state = _wait_for_process_group_absence(
            process_group_id, TERM_GRACE_SECONDS, state
        )
    if state is _ProcessGroupState.PRESENT:
        state = _signal_process_group(process_group_id, signal.SIGKILL)
        if state is _ProcessGroupState.PRESENT:
            state = _wait_for_process_group_absence(
                process_group_id, KILL_GRACE_SECONDS, state
            )
    try:
        process.wait(timeout=KILL_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        if state is _ProcessGroupState.PRESENT:
            state = _signal_process_group(process_group_id, signal.SIGKILL)
        else:
            process.kill()
        process.wait()


def _run_process(
    process_factory: Callable[..., object],
    command: Sequence[str],
    cwd: Path,
    stdout: BinaryIO,
    stderr: BinaryIO,
    timeout_seconds: float,
    *,
    resource_capability: ResourceCapability | None = None,
    resource_callback: Callable[[ResourceMeasurement], None] | None = None,
    deferred_resource_errors: list[BaseException] | None = None,
) -> int:
    """Run one command in a dedicated group and leave no live descendants."""
    process = process_factory(
        command,
        cwd=cwd,
        stdout=stdout,
        stderr=stderr,
        start_new_session=True,
    )
    process_group_id = process.pid
    process_group_cleaned = False
    resource_monitor = None
    primary_error: BaseException | None = None
    try:
        try:
            if resource_capability is not None:
                resource_monitor = ProcessTreeResourceMonitor(
                    process.pid,
                    resource_capability,
                )
                resource_monitor.start()
        except KeyboardInterrupt:
            raise
        except BaseException as error:
            raise RunnerError(f"cannot start process resource monitor: {error}") from error

        try:
            return_code = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired as error:
            _terminate_process_group(process, process_group_id)
            process_group_cleaned = True
            raise RunnerError(
                f"benchmark process timed out after {timeout_seconds} seconds"
            ) from error

        state = _probe_process_group(process_group_id)
        if state is _ProcessGroupState.PRESENT:
            _terminate_process_group(process, process_group_id)
            process_group_cleaned = True
            raise RunnerError(
                "benchmark wrapper exited while descendant processes remained"
            )
        process_group_cleaned = True
        return return_code
    except BaseException as error:
        primary_error = error
        if not process_group_cleaned:
            try:
                _terminate_process_group(process, process_group_id)
                process_group_cleaned = True
            except BaseException as cleanup_error:
                error.add_note(f"secondary process-group cleanup failure: {cleanup_error}")
        raise
    finally:
        if resource_monitor is not None:
            try:
                exit_code = process.returncode
                if isinstance(exit_code, bool) or not isinstance(exit_code, int):
                    raise RunnerError("benchmark process ended without an integer exit code")
                measurement = resource_monitor.finish(exit_code)
                if resource_callback is not None:
                    resource_callback(measurement)
            except BaseException as retention_error:
                if isinstance(retention_error, (KeyboardInterrupt, SystemExit)):
                    secondary_error = retention_error
                elif isinstance(retention_error, RunnerError):
                    secondary_error = retention_error
                else:
                    secondary_error = RunnerError(
                        f"cannot retain process resource usage: {retention_error}"
                    )
                if primary_error is not None:
                    primary_error.add_note(
                        f"secondary process resource retention failure: {secondary_error}"
                    )
                elif deferred_resource_errors is not None and not isinstance(
                    secondary_error,
                    (KeyboardInterrupt, SystemExit),
                ):
                    deferred_resource_errors.append(secondary_error)
                elif secondary_error is retention_error:
                    raise
                else:
                    raise secondary_error from retention_error


def _resolved_contained(path: Path, container: Path, description: str) -> Path:
    try:
        resolved_path = path.resolve(strict=True)
        resolved_container = container.resolve(strict=True)
        resolved_path.relative_to(resolved_container)
    except (OSError, ValueError) as error:
        raise RunnerError(
            f"{description} is outside required containment {container}: {path}"
        ) from error
    return resolved_path


def _require_directory_no_follow(
    path: Path,
    container: Path,
    description: str,
    expected_identity: _ObjectIdentity | None = None,
) -> _ObjectIdentity:
    try:
        status = path.lstat()
    except OSError as error:
        raise RunnerError(f"cannot inspect {description} {path}: {error}") from error
    if stat.S_ISLNK(status.st_mode):
        raise RunnerError(f"{description} is a symlink: {path}")
    if not stat.S_ISDIR(status.st_mode):
        raise RunnerError(f"{description} is not a directory: {path}")
    _resolved_contained(path, container, description)
    identity = _object_identity(status)
    if expected_identity is not None and identity != expected_identity:
        raise RunnerError(
            f"{description} identity changed or was replaced: {path}"
        )
    return identity


def _require_regular_file_no_follow(
    path: Path,
    container: Path,
    description: str,
    expected_identity: _ObjectIdentity | None = None,
    descriptor: int | None = None,
) -> _ObjectIdentity:
    try:
        status = path.lstat()
    except OSError as error:
        raise RunnerError(f"cannot inspect {description} {path}: {error}") from error
    if stat.S_ISLNK(status.st_mode):
        raise RunnerError(f"{description} is a symlink: {path}")
    if not stat.S_ISREG(status.st_mode):
        raise RunnerError(f"{description} is not a regular file: {path}")
    _resolved_contained(path, container, description)
    identity = _object_identity(status)
    if descriptor is not None:
        descriptor_status = os.fstat(descriptor)
        if not stat.S_ISREG(descriptor_status.st_mode):
            raise RunnerError(f"{description} descriptor is not regular: {path}")
        descriptor_identity = _object_identity(descriptor_status)
        if expected_identity is not None and descriptor_identity != expected_identity:
            raise RunnerError(f"{description} descriptor identity changed: {path}")
        if descriptor_identity != identity:
            raise RunnerError(
                f"{description} descriptor/path identity mismatch: {path}"
            )
    if expected_identity is not None and identity != expected_identity:
        raise RunnerError(
            f"{description} identity changed or was replaced: {path}"
        )
    return identity


def _prepare_run_parent(path: Path, root: Path) -> _ObjectIdentity:
    try:
        initial_status = path.lstat()
    except FileNotFoundError:
        try:
            path.mkdir()
        except OSError as error:
            raise RunnerError(f"cannot create benchmark run parent {path}: {error}") from error
        return _require_directory_no_follow(path, root, "benchmark run parent")
    except OSError as error:
        raise RunnerError(f"cannot inspect benchmark run parent {path}: {error}") from error
    return _require_directory_no_follow(
        path,
        root,
        "benchmark run parent",
        _object_identity(initial_status),
    )


def _create_directory_exclusively(
    path: Path,
    container: Path,
    description: str,
) -> _ObjectIdentity:
    try:
        path.mkdir()
    except FileExistsError as error:
        raise RunnerError(f"{description} already exists: {path}") from error
    except OSError as error:
        raise RunnerError(f"cannot create {description} {path}: {error}") from error
    return _require_directory_no_follow(path, container, description)


def _require_attempt_hierarchy(
    root: Path,
    run_parent: Path,
    run_directory: Path,
    experiment_directory: Path,
    attempt_directory: Path,
    run_parent_identity: _ObjectIdentity,
    run_directory_identity: _ObjectIdentity,
    experiment_directory_identity: _ObjectIdentity,
    attempt_directory_identity: _ObjectIdentity,
) -> None:
    _require_directory_no_follow(
        run_parent,
        root,
        "benchmark run parent",
        run_parent_identity,
    )
    _require_directory_no_follow(
        run_directory,
        run_parent,
        "benchmark timestamp directory",
        run_directory_identity,
    )
    _require_directory_no_follow(
        experiment_directory,
        run_directory,
        "experiment directory",
        experiment_directory_identity,
    )
    _require_directory_no_follow(
        attempt_directory,
        experiment_directory,
        "attempt directory",
        attempt_directory_identity,
    )


def _nofollow_opener(path: str, flags: int) -> int:
    return os.open(
        path,
        flags | os.O_NOFOLLOW | os.O_CLOEXEC,
        0o644,
    )


@contextmanager
def _exclusive_regular_log(
    path: Path,
) -> Iterator[tuple[BinaryIO, _ObjectIdentity]]:
    with open(path, "xb", opener=_nofollow_opener) as output:
        identity = _require_regular_file_no_follow(
            path,
            path.parent,
            "attempt log",
            descriptor=output.fileno(),
        )
        yield output, identity


def _read_log_tail(path: Path) -> str:
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        status = os.fstat(descriptor)
        if not stat.S_ISREG(status.st_mode):
            raise RunnerError(f"attempt log is not a regular file: {path}")
        offset = max(0, status.st_size - DIAGNOSTIC_TAIL_BYTES)
        os.lseek(descriptor, offset, os.SEEK_SET)
        chunks = []
        remaining = DIAGNOSTIC_TAIL_BYTES
        while remaining > 0:
            chunk = os.read(descriptor, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
    finally:
        os.close(descriptor)
    return b"".join(chunks).decode("utf-8", errors="replace")


def _child_failure(
    message: str,
    command: Sequence[str],
    stdout_path: Path,
    stderr_path: Path,
) -> RunnerError:
    details = [message, f"command: {command!r}"]
    for label, path in (("stdout", stdout_path), ("stderr", stderr_path)):
        try:
            tail = _read_log_tail(path)
        except (OSError, RunnerError) as error:
            details.append(f"{label} tail unavailable: {error}")
        else:
            if tail:
                details.append(
                    f"{label} tail (last {DIAGNOSTIC_TAIL_BYTES} bytes):\n{tail}"
                )
    return RunnerError("\n".join(details))


def _attach_deferred_resource_errors(
    primary_error: BaseException,
    deferred_errors: list[BaseException],
) -> None:
    for deferred_error in deferred_errors:
        primary_error.add_note(
            f"secondary process resource retention failure: {deferred_error}"
        )


def _raise_deferred_resource_errors(
    deferred_errors: list[BaseException],
    command: Sequence[str],
    stdout_path: Path,
    stderr_path: Path,
) -> None:
    if not deferred_errors:
        return
    failure = _child_failure(
        str(deferred_errors[0]),
        command,
        stdout_path,
        stderr_path,
    )
    for deferred_error in deferred_errors[1:]:
        failure.add_note(
            f"additional process resource retention failure: {deferred_error}"
        )
    raise failure from deferred_errors[0]


def _publish_resource_document(
    path: Path,
    document: dict[str, object],
    description: str,
    expected_parent_identity: _ObjectIdentity,
) -> None:
    try:
        publish_json_exclusive(
            path,
            document,
            expected_parent_identity=(
                expected_parent_identity.device,
                expected_parent_identity.inode,
            ),
        )
    except FileExistsError as error:
        raise RunnerError(f"{description} already exists: {path}") from error
    except (OSError, ResourceError, ValueError) as error:
        raise RunnerError(f"cannot publish {description} {path}: {error}") from error


@contextmanager
def _retain_resource_summary(
    *,
    root: Path,
    run_parent: Path,
    run_directory: Path,
    run_parent_identity: _ObjectIdentity,
    run_directory_identity: _ObjectIdentity,
    requested_experiment_ids: tuple[int, ...],
    complete_matrix: bool,
    attempt_records: list[dict[str, object]],
) -> Iterator[None]:
    primary_error: BaseException | None = None
    try:
        yield
    except BaseException as error:
        primary_error = error
        raise
    finally:
        try:
            _require_directory_no_follow(
                run_parent,
                root,
                "benchmark run parent",
                run_parent_identity,
            )
            _require_directory_no_follow(
                run_directory,
                run_parent,
                "benchmark timestamp directory",
                run_directory_identity,
            )
            summary = build_sequential_resource_summary(
                requested_experiment_ids,
                complete_matrix,
                attempt_records,
            )
            _publish_resource_document(
                run_directory / "resource_summary.json",
                summary,
                "benchmark resource summary",
                run_directory_identity,
            )
        except BaseException as retention_error:
            if primary_error is not None:
                primary_error.add_note(
                    f"secondary resource summary retention failure: {retention_error}"
                )
            else:
                raise


def run_benchmark(
    *,
    ns3_root: str | Path,
    config_path: str | Path,
    timestamp: str | None = None,
    configurations: Iterable[ExperimentConfiguration] | None = None,
    process_factory: Callable[..., object] = subprocess.Popen,
    resource_capability: ResourceCapability | None = None,
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
    resource_capability = (
        resource_capability
        if resource_capability is not None
        else detect_resource_capability()
    )
    if resource_capability.sequential_only:
        print(f"resource: {resource_capability.diagnostic}", file=output, flush=True)

    timestamp = timestamp if timestamp is not None else _timestamp_now()
    if (
        not isinstance(timestamp, str)
        or not timestamp
        or not _TIMESTAMP_PATTERN.fullmatch(timestamp)
    ):
        raise RunnerError(f"invalid benchmark timestamp component: {timestamp!r}")
    run_parent = root / "run"
    run_parent_identity = _prepare_run_parent(run_parent, root)
    run_directory = run_parent / f"scripted_exp_{timestamp}"
    run_directory_identity = _create_directory_exclusively(
        run_directory, run_parent, "benchmark timestamp directory"
    )

    attempt_count = len(requested) * loaded.repetitions
    completed = 0
    baselines: dict[BaselineKey, float] = {}
    experiment_identities: dict[int, _ObjectIdentity] = {}
    resource_records: list[dict[str, object]] = []
    try:
        csv_output = ExcelCsvWriter(run_directory / "results.csv")
    except FileExistsError as error:
        raise RunnerError(
            f"benchmark CSV already exists: {run_directory / 'results.csv'}"
        ) from error
    except OSError as error:
        raise RunnerError(f"cannot create benchmark CSV: {error}") from error

    with (
        _retain_resource_summary(
            root=root,
            run_parent=run_parent,
            run_directory=run_directory,
            run_parent_identity=run_parent_identity,
            run_directory_identity=run_directory_identity,
            requested_experiment_ids=tuple(
                configuration.experiment_id for configuration in requested
            ),
            complete_matrix=requested == build_matrix(),
            attempt_records=resource_records,
        ),
        csv_output,
    ):
        results_path = run_directory / "results.csv"
        results_identity = _ObjectIdentity(*csv_output.identity())
        _require_regular_file_no_follow(
            results_path,
            run_directory,
            "benchmark CSV",
            results_identity,
            descriptor=csv_output.fileno(),
        )
        for attempt in iter_experiment_attempts(requested, loaded.repetitions):
            configuration = attempt.configuration
            experiment_directory = run_directory / f"experiment_{configuration.experiment_id:03d}"
            _require_directory_no_follow(
                run_parent,
                root,
                "benchmark run parent",
                run_parent_identity,
            )
            _require_directory_no_follow(
                run_directory,
                run_parent,
                "benchmark timestamp directory",
                run_directory_identity,
            )
            if configuration.experiment_id not in experiment_identities:
                experiment_identity = _create_directory_exclusively(
                    experiment_directory, run_directory, "experiment directory"
                )
                experiment_identities[configuration.experiment_id] = experiment_identity
            else:
                experiment_identity = experiment_identities[configuration.experiment_id]
                _require_directory_no_follow(
                    experiment_directory,
                    run_directory,
                    "experiment directory",
                    experiment_identity,
                )
            attempt_directory = experiment_directory / f"attempt_{attempt.repetition_attempt}"
            attempt_identity = _create_directory_exclusively(
                attempt_directory, experiment_directory, "attempt directory"
            )
            _require_attempt_hierarchy(
                root,
                run_parent,
                run_directory,
                experiment_directory,
                attempt_directory,
                run_parent_identity,
                run_directory_identity,
                experiment_identity,
                attempt_identity,
            )
            output_path = attempt_directory / OUTPUT_NAME
            try:
                output_path.lstat()
            except FileNotFoundError:
                pass
            else:
                raise RunnerError(f"benchmark output already exists: {output_path}")
            stdout_path = attempt_directory / "stdout.log"
            stderr_path = attempt_directory / "stderr.log"
            resource_usage_path = attempt_directory / "resource_usage.json"
            deferred_resource_errors: list[BaseException] = []

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
            stdout_identity = None
            stderr_identity = None
            try:
                with (
                    _exclusive_regular_log(stdout_path) as stdout_pinned,
                    _exclusive_regular_log(stderr_path) as stderr_pinned,
                ):
                    stdout_log, stdout_identity = stdout_pinned
                    stderr_log, stderr_identity = stderr_pinned
                    try:
                        _require_attempt_hierarchy(
                            root,
                            run_parent,
                            run_directory,
                            experiment_directory,
                            attempt_directory,
                            run_parent_identity,
                            run_directory_identity,
                            experiment_identity,
                            attempt_identity,
                        )
                        _require_regular_file_no_follow(
                            results_path,
                            run_directory,
                            "benchmark CSV",
                            results_identity,
                            csv_output.fileno(),
                        )
                        _require_regular_file_no_follow(
                            stdout_path,
                            attempt_directory,
                            "attempt stdout log",
                            stdout_identity,
                            stdout_log.fileno(),
                        )
                        _require_regular_file_no_follow(
                            stderr_path,
                            attempt_directory,
                            "attempt stderr log",
                            stderr_identity,
                            stderr_log.fileno(),
                        )

                        def retain_attempt_resource(
                            measurement: ResourceMeasurement,
                        ) -> None:
                            record = build_attempt_resource_record(
                                configuration.experiment_id,
                                attempt.repetition_attempt,
                                measurement,
                            )
                            resource_records.append(record)
                            _require_attempt_hierarchy(
                                root,
                                run_parent,
                                run_directory,
                                experiment_directory,
                                attempt_directory,
                                run_parent_identity,
                                run_directory_identity,
                                experiment_identity,
                                attempt_identity,
                            )
                            _publish_resource_document(
                                resource_usage_path,
                                record,
                                "attempt resource usage",
                                attempt_identity,
                            )

                        return_code = _run_process(
                            process_factory,
                            command,
                            root,
                            stdout_log,
                            stderr_log,
                            PROCESS_TIMEOUT_SECONDS,
                            resource_capability=resource_capability,
                            resource_callback=retain_attempt_resource,
                            deferred_resource_errors=deferred_resource_errors,
                        )
                    finally:
                        _require_attempt_hierarchy(
                            root,
                            run_parent,
                            run_directory,
                            experiment_directory,
                            attempt_directory,
                            run_parent_identity,
                            run_directory_identity,
                            experiment_identity,
                            attempt_identity,
                        )
                        _require_regular_file_no_follow(
                            results_path,
                            run_directory,
                            "benchmark CSV",
                            results_identity,
                            csv_output.fileno(),
                        )
                        _require_regular_file_no_follow(
                            stdout_path,
                            attempt_directory,
                            "attempt stdout log",
                            stdout_identity,
                            stdout_log.fileno(),
                        )
                        _require_regular_file_no_follow(
                            stderr_path,
                            attempt_directory,
                            "attempt stderr log",
                            stderr_identity,
                            stderr_log.fileno(),
                        )
            except RunnerError as error:
                _require_attempt_hierarchy(
                    root,
                    run_parent,
                    run_directory,
                    experiment_directory,
                    attempt_directory,
                    run_parent_identity,
                    run_directory_identity,
                    experiment_identity,
                    attempt_identity,
                )
                if stdout_identity is not None:
                    _require_regular_file_no_follow(
                        stdout_path,
                        attempt_directory,
                        "attempt stdout log",
                        stdout_identity,
                    )
                if stderr_identity is not None:
                    _require_regular_file_no_follow(
                        stderr_path,
                        attempt_directory,
                        "attempt stderr log",
                        stderr_identity,
                    )
                failure = _child_failure(
                    str(error),
                    command,
                    stdout_path,
                    stderr_path,
                )
                _attach_deferred_resource_errors(
                    failure,
                    deferred_resource_errors,
                )
                raise failure from error

            _require_attempt_hierarchy(
                root,
                run_parent,
                run_directory,
                experiment_directory,
                attempt_directory,
                run_parent_identity,
                run_directory_identity,
                experiment_identity,
                attempt_identity,
            )
            _require_regular_file_no_follow(
                results_path,
                run_directory,
                "benchmark CSV",
                results_identity,
                csv_output.fileno(),
            )
            _require_regular_file_no_follow(
                stdout_path,
                attempt_directory,
                "attempt stdout log",
                stdout_identity,
            )
            _require_regular_file_no_follow(
                stderr_path,
                attempt_directory,
                "attempt stderr log",
                stderr_identity,
            )
            if return_code != 0:
                failure = _child_failure(
                    f"benchmark process failed with return code {return_code}",
                    command,
                    stdout_path,
                    stderr_path,
                )
                _attach_deferred_resource_errors(
                    failure,
                    deferred_resource_errors,
                )
                raise failure
            try:
                _require_regular_file_no_follow(
                    output_path, attempt_directory, "benchmark output"
                )
            except RunnerError as error:
                failure = _child_failure(
                    str(error), command, stdout_path, stderr_path
                )
                _attach_deferred_resource_errors(
                    failure,
                    deferred_resource_errors,
                )
                raise failure from error

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
                failure = _child_failure(
                    f"benchmark output validation failed: {error}",
                    command,
                    stdout_path,
                    stderr_path,
                )
                _attach_deferred_resource_errors(
                    failure,
                    deferred_resource_errors,
                )
                raise failure from error
            _raise_deferred_resource_errors(
                deferred_resource_errors,
                command,
                stdout_path,
                stderr_path,
            )
            _require_attempt_hierarchy(
                root,
                run_parent,
                run_directory,
                experiment_directory,
                attempt_directory,
                run_parent_identity,
                run_directory_identity,
                experiment_identity,
                attempt_identity,
            )
            _require_regular_file_no_follow(
                results_path,
                run_directory,
                "benchmark CSV",
                results_identity,
                csv_output.fileno(),
            )
            _require_regular_file_no_follow(
                stdout_path,
                attempt_directory,
                "attempt stdout log",
                stdout_identity,
            )
            _require_regular_file_no_follow(
                stderr_path,
                attempt_directory,
                "attempt stderr log",
                stderr_identity,
            )
            if configuration.sta_count_per_bss == 1:
                for row in rows:
                    key: BaselineKey = (
                        configuration.rssi_range,
                        configuration.interference_mode,
                        configuration.traffic_mode,
                        configuration.mimo_mode,
                        attempt.repetition_attempt,
                        row.bss_id,
                    )
                    if key in baselines:
                        raise RunnerError(f"duplicate matching baseline for {key!r}")
                    baselines[key] = row.aggregate_data_tx_rate_over_interval_mbps
            try:
                rows = apply_matching_baseline(rows, baselines)
            except ValueError as error:
                raise RunnerError(f"cannot apply matching baseline: {error}") from error
            csv_output.append_attempt(rows)
            _require_attempt_hierarchy(
                root,
                run_parent,
                run_directory,
                experiment_directory,
                attempt_directory,
                run_parent_identity,
                run_directory_identity,
                experiment_identity,
                attempt_identity,
            )
            _require_regular_file_no_follow(
                results_path,
                run_directory,
                "benchmark CSV",
                results_identity,
                csv_output.fileno(),
            )
            _require_regular_file_no_follow(
                stdout_path,
                attempt_directory,
                "attempt stdout log",
                stdout_identity,
            )
            _require_regular_file_no_follow(
                stderr_path,
                attempt_directory,
                "attempt stderr log",
                stderr_identity,
            )
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
    process_factory: Callable[..., object] = subprocess.Popen,
    resource_capability: ResourceCapability | None = None,
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
            process_factory=process_factory,
            resource_capability=resource_capability,
            output=output,
        )
    except KeyboardInterrupt:
        print("error: saturated benchmark interrupted; completed data was retained", file=error)
        return 130
    except RunnerError as runner_error:
        print(f"error: {runner_error}", file=error)
        return 1
    return 0
