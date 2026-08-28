"""Resource-aware parallel lifecycle for the saturated TCP benchmark matrix."""

from __future__ import annotations

import argparse
from collections import deque
from collections.abc import Callable, Iterable, Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from contextlib import contextmanager
from copy import deepcopy
from dataclasses import dataclass, field, replace
from datetime import datetime
from enum import Enum
import os
from pathlib import Path
import queue
import re
import signal
import shlex
import stat
import subprocess
import sys
import threading
import time
import tomllib
from typing import BinaryIO, Iterator, TextIO

from .csv_output import (
    BaselineKey,
    BssCsvRow,
    ExcelCsvWriter,
    apply_matching_baseline,
)
from .matrix import (
    INTERFERENCE_MODES,
    MIMO_MODES,
    RSSI_RANGES,
    STA_COUNTS,
    TRAFFIC_MODES,
    ExperimentConfiguration,
    ExperimentAttempt,
    build_matrix,
    expand_experiment_ids,
    iter_experiment_attempts,
)
from .resources import (
    MemorySnapshot,
    ProcessTreeResourceMonitor,
    ResourceCapability,
    ResourceError,
    ResourceMeasurement,
    build_attempt_resource_record,
    detect_resource_capability,
    process_tree_rss_bytes,
    publish_json_exclusive,
    read_memory_snapshot,
)
from .scheduler import (
    DEFAULT_MEMORY_RESERVE_PERCENT,
    MIN_MEMORY_RESERVE_PERCENT,
    ResourceScheduler,
    SchedulerError,
    calculate_max_workers,
    calculate_worker_peak_estimate,
    plan_execution,
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
        "traffic_warmup_seconds",
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
        "traffic_warmup_seconds": 0,
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
    traffic_warmup_seconds = effective["benchmark"]["traffic_warmup_seconds"]
    if (
        type(traffic_warmup_seconds) is not int
        or not 0 <= traffic_warmup_seconds <= _UINT32_MAX
    ):
        raise RunnerError(
            f"invalid saturated benchmark.traffic_warmup_seconds in {path}: "
            f"expected uint32 in [0, {_UINT32_MAX}]"
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
        if (
            type(configuration.traffic_warmup_seconds) is not int
            or not 0 <= configuration.traffic_warmup_seconds <= _UINT32_MAX
        ):
            raise RunnerError("benchmark traffic_warmup_seconds must be a uint32 integer")
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
        f"--benchmark-traffic-warmup-seconds={configuration.traffic_warmup_seconds}",
        f"--simulation-rng-seed={RNG_SEED}",
        f"--simulation-rng-run={repetition_attempt}",
        f"--general-run-folder={run_directory}",
        f"--general-output-name={OUTPUT_NAME}",
    ]
    return [str(ns3), "run", "--no-build", shlex.join(arguments)]


def build_ns3_build_command(ns3: str | Path) -> list[str]:
    """Return the one required build command run before any simulation."""
    return [str(ns3), "build", "saturated-tcp-scenario"]


def _default_build_runner(command: Sequence[str], cwd: Path) -> int:
    try:
        completed = subprocess.run(command, cwd=cwd, check=False)
    except OSError as error:
        raise RunnerError(f"cannot start saturated benchmark build: {error}") from error
    return completed.returncode


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
            "traffic_warmup_seconds": configuration.traffic_warmup_seconds,
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


def _terminate_process_group_members(process_group_ids: Iterable[int]) -> None:
    """Terminate registered groups together without reaping worker-owned children."""
    states = {
        process_group_id: _signal_process_group(process_group_id, signal.SIGTERM)
        for process_group_id in process_group_ids
    }
    deadline = time.monotonic() + TERM_GRACE_SECONDS
    while any(state is _ProcessGroupState.PRESENT for state in states.values()):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.01, remaining))
        for process_group_id, state in tuple(states.items()):
            if state is _ProcessGroupState.PRESENT:
                states[process_group_id] = _probe_process_group(process_group_id)
    for process_group_id, state in tuple(states.items()):
        if state is _ProcessGroupState.PRESENT:
            states[process_group_id] = _signal_process_group(
                process_group_id,
                signal.SIGKILL,
            )
    deadline = time.monotonic() + KILL_GRACE_SECONDS
    while any(state is _ProcessGroupState.PRESENT for state in states.values()):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.01, remaining))
        for process_group_id, state in tuple(states.items()):
            if state is _ProcessGroupState.PRESENT:
                states[process_group_id] = _probe_process_group(process_group_id)


class _AdmissionsStopped(RunnerError):
    """A worker observed the controller stop before it could register its child."""


class _ActiveProcessRegistry:
    """Close Popen/stop races while leaving direct-child reap to each worker."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._stopped = False
        self._launching = 0
        self._processes: dict[int, object] = {}
        self._maximum_processes = 0

    def launch(
        self,
        process_factory: Callable[..., object],
        command: Sequence[str],
        **kwargs: object,
    ) -> object:
        """Launch and register, or self-terminate if stop won the launch race."""
        with self._lock:
            if self._stopped:
                raise _AdmissionsStopped("benchmark worker admissions were stopped")
            self._launching += 1
        try:
            process = process_factory(command, **kwargs)
        except BaseException:
            with self._lock:
                self._launching -= 1
            raise
        process_group_id = getattr(process, "pid", None)
        if (
            isinstance(process_group_id, bool)
            or not isinstance(process_group_id, int)
            or process_group_id <= 0
        ):
            with self._lock:
                self._launching -= 1
            raise RunnerError("benchmark process returned an invalid process group ID")
        with self._lock:
            self._launching -= 1
            stopped = self._stopped
            if not stopped:
                self._processes[process_group_id] = process
                self._maximum_processes = max(
                    self._maximum_processes,
                    len(self._processes),
                )
        if stopped:
            try:
                _terminate_process_group(process, process_group_id)
            except BaseException as cleanup_error:
                failure = _AdmissionsStopped(
                    "benchmark worker launched after admissions stopped"
                )
                failure.add_note(
                    f"secondary post-stop process cleanup failure: {cleanup_error}"
                )
                raise failure from cleanup_error
            raise _AdmissionsStopped(
                "benchmark worker launched after admissions stopped and was terminated"
            )
        return process

    def unregister(self, process_group_id: int) -> None:
        with self._lock:
            self._processes.pop(process_group_id, None)

    def process_ids(self) -> tuple[int, ...]:
        with self._lock:
            return tuple(self._processes)

    def maximum_processes(self) -> int:
        with self._lock:
            return self._maximum_processes

    def stop_admissions(self) -> None:
        """Make the sticky stop visible before cancellation can dequeue work."""
        with self._lock:
            self._stopped = True

    def terminate_active(self) -> None:
        """Terminate every group registered after admissions were stopped."""
        with self._lock:
            process_group_ids = tuple(self._processes)
        _terminate_process_group_members(process_group_ids)

    def stop(self) -> None:
        """Stop future registration and terminate every currently active group."""
        self.stop_admissions()
        self.terminate_active()


def _run_process(
    process_factory: Callable[..., object],
    command: Sequence[str],
    cwd: Path,
    stdout: BinaryIO,
    stderr: BinaryIO,
    timeout_seconds: float,
    *,
    resource_capability: ResourceCapability | None = None,
    resource_monitor_factory: Callable[..., object] = ProcessTreeResourceMonitor,
    resource_callback: Callable[[ResourceMeasurement], None] | None = None,
    deferred_resource_errors: list[BaseException] | None = None,
    process_registry: _ActiveProcessRegistry | None = None,
) -> int:
    """Run one command in a dedicated group and leave no live descendants."""
    process_arguments = {
        "cwd": cwd,
        "stdout": stdout,
        "stderr": stderr,
        "start_new_session": True,
    }
    if process_registry is None:
        process = process_factory(command, **process_arguments)
    else:
        process = process_registry.launch(
            process_factory,
            command,
            **process_arguments,
        )
    process_group_id = process.pid
    process_group_cleaned = False
    resource_monitor = None
    primary_error: BaseException | None = None
    try:
        try:
            if resource_capability is not None:
                resource_monitor = resource_monitor_factory(
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
        try:
            if resource_monitor is not None:
                try:
                    exit_code = process.returncode
                    if isinstance(exit_code, bool) or not isinstance(exit_code, int):
                        raise RunnerError(
                            "benchmark process ended without an integer exit code"
                        )
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
                            "secondary process resource retention failure: "
                            f"{secondary_error}"
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
        finally:
            if process_registry is not None:
                process_registry.unregister(process_group_id)


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


@dataclass(frozen=True)
class _RunSelection:
    configurations: tuple[ExperimentConfiguration, ...]
    requested_experiment_ids: tuple[int, ...]
    executed_experiment_ids: tuple[int, ...]
    auto_included_baseline_ids: tuple[int, ...]
    complete_matrix: bool


def _select_run(
    configurations: Iterable[ExperimentConfiguration] | None,
    experiment_ids: Iterable[int] | None,
) -> _RunSelection:
    if configurations is not None and experiment_ids is not None:
        raise RunnerError("configurations and experiment_ids are mutually exclusive")
    matrix = build_matrix()
    by_id = {configuration.experiment_id: configuration for configuration in matrix}
    if configurations is not None:
        requested_configurations = tuple(configurations)
        _validate_requested_configurations(requested_configurations)
        requested_ids = tuple(
            configuration.experiment_id for configuration in requested_configurations
        )
        canonical = tuple(
            sorted(
                requested_configurations,
                key=lambda configuration: configuration.experiment_id,
            )
        )
        return _RunSelection(
            canonical,
            requested_ids,
            tuple(configuration.experiment_id for configuration in canonical),
            (),
            False,
        )
    if experiment_ids is not None:
        try:
            requested_ids, executed_ids, automatic_ids = expand_experiment_ids(
                experiment_ids
            )
        except ValueError as error:
            raise RunnerError(f"invalid benchmark experiment subset: {error}") from error
        return _RunSelection(
            tuple(by_id[experiment_id] for experiment_id in executed_ids),
            requested_ids,
            executed_ids,
            automatic_ids,
            False,
        )
    return _RunSelection(
        matrix,
        tuple(configuration.experiment_id for configuration in matrix),
        tuple(configuration.experiment_id for configuration in matrix),
        (),
        True,
    )


@dataclass(frozen=True)
class _AttemptContext:
    attempt: ExperimentAttempt
    experiment_directory: Path
    attempt_directory: Path
    experiment_identity: _ObjectIdentity
    attempt_identity: _ObjectIdentity
    output_path: Path
    stdout_path: Path
    stderr_path: Path
    resource_usage_path: Path
    command: tuple[str, ...]


@dataclass(frozen=True)
class _WorkerOutcome:
    attempt: ExperimentAttempt
    rows: tuple[BssCsvRow, ...] | None
    measurement: ResourceMeasurement | None
    error: BaseException | None


@dataclass
class _ResourceSummaryState:
    requested_experiment_ids: tuple[int, ...]
    executed_experiment_ids: tuple[int, ...]
    auto_included_baseline_ids: tuple[int, ...]
    complete_matrix: bool
    memory_reserve_percent: int
    maximum_parallel_workers: int = 0
    calibrated_peak_rss_bytes: int | None = None
    worker_peak_estimate_bytes: int | None = None
    acceptance_floor_breached: bool = False
    controller_minimum_mem_available_bytes: int | None = None
    controller_minimum_mem_available_percent: float | None = None
    attempt_records: list[dict[str, object]] = field(default_factory=list)


def _build_resource_summary(state: _ResourceSummaryState) -> dict[str, object]:
    ordered_attempts = sorted(
        state.attempt_records,
        key=lambda record: (record["experiment_id"], record["repetition_attempt"]),
    )
    available_bytes = [
        record["minimum_mem_available_bytes"]
        for record in ordered_attempts
        if record["minimum_mem_available_bytes"] is not None
    ]
    available_percent = [
        record["minimum_mem_available_percent"]
        for record in ordered_attempts
        if record["minimum_mem_available_percent"] is not None
    ]
    if state.controller_minimum_mem_available_bytes is not None:
        available_bytes.append(state.controller_minimum_mem_available_bytes)
    if state.controller_minimum_mem_available_percent is not None:
        available_percent.append(state.controller_minimum_mem_available_percent)
    return {
        "schema_version": 1,
        "complete_matrix": state.complete_matrix,
        "requested_experiment_ids": list(state.requested_experiment_ids),
        "executed_experiment_ids": list(state.executed_experiment_ids),
        "auto_included_baseline_ids": list(state.auto_included_baseline_ids),
        "memory_reserve_percent": state.memory_reserve_percent,
        "calibrated_peak_rss_bytes": state.calibrated_peak_rss_bytes,
        "worker_peak_estimate_bytes": state.worker_peak_estimate_bytes,
        "maximum_parallel_workers": state.maximum_parallel_workers,
        "minimum_mem_available_bytes": min(available_bytes, default=None),
        "minimum_mem_available_percent": min(available_percent, default=None),
        "attempts": ordered_attempts,
    }


@contextmanager
def _retain_resource_summary(
    *,
    root: Path,
    run_parent: Path,
    run_directory: Path,
    run_parent_identity: _ObjectIdentity,
    run_directory_identity: _ObjectIdentity,
    state: _ResourceSummaryState,
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
            summary = _build_resource_summary(state)
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


@contextmanager
def _close_csv_after_summary(csv_output: ExcelCsvWriter) -> Iterator[None]:
    """Close CSV last without replacing an active primary failure."""
    primary_error: BaseException | None = None
    try:
        yield
    except BaseException as error:
        primary_error = error
        raise
    finally:
        try:
            csv_output.close()
        except BaseException as close_error:
            if primary_error is not None:
                primary_error.add_note(
                    f"secondary CSV close failure: {close_error}"
                )
            else:
                raise


def _prepare_attempt_context(
    *,
    root: Path,
    run_parent: Path,
    run_directory: Path,
    run_parent_identity: _ObjectIdentity,
    run_directory_identity: _ObjectIdentity,
    experiment_identities: dict[int, _ObjectIdentity],
    ns3: Path,
    resolved_config: Path,
    attempt: ExperimentAttempt,
) -> _AttemptContext:
    """Allocate one attempt hierarchy exclusively in the controller thread."""
    configuration = attempt.configuration
    experiment_directory = (
        run_directory / f"experiment_{configuration.experiment_id:03d}"
    )
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
            experiment_directory,
            run_directory,
            "experiment directory",
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
    attempt_directory = (
        experiment_directory / f"attempt_{attempt.repetition_attempt}"
    )
    attempt_identity = _create_directory_exclusively(
        attempt_directory,
        experiment_directory,
        "attempt directory",
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
    return _AttemptContext(
        attempt=attempt,
        experiment_directory=experiment_directory,
        attempt_directory=attempt_directory,
        experiment_identity=experiment_identity,
        attempt_identity=attempt_identity,
        output_path=output_path,
        stdout_path=attempt_directory / "stdout.log",
        stderr_path=attempt_directory / "stderr.log",
        resource_usage_path=attempt_directory / "resource_usage.json",
        command=tuple(
            build_ns3_command(
                ns3,
                resolved_config,
                configuration,
                attempt.repetition_attempt,
                attempt_directory,
            )
        ),
    )


def _verify_attempt_context(
    *,
    root: Path,
    run_parent: Path,
    run_directory: Path,
    run_parent_identity: _ObjectIdentity,
    run_directory_identity: _ObjectIdentity,
    context: _AttemptContext,
) -> None:
    _require_attempt_hierarchy(
        root,
        run_parent,
        run_directory,
        context.experiment_directory,
        context.attempt_directory,
        run_parent_identity,
        run_directory_identity,
        context.experiment_identity,
        context.attempt_identity,
    )


def _copy_error_notes(source: BaseException, destination: BaseException) -> None:
    for note in getattr(source, "__notes__", ()):
        destination.add_note(note)


def _run_attempt_worker(
    *,
    root: Path,
    run_parent: Path,
    run_directory: Path,
    run_parent_identity: _ObjectIdentity,
    run_directory_identity: _ObjectIdentity,
    loaded: RunnerConfiguration,
    context: _AttemptContext,
    process_factory: Callable[..., object],
    resource_capability: ResourceCapability,
    resource_monitor_factory: Callable[..., object],
    process_registry: _ActiveProcessRegistry,
) -> _WorkerOutcome:
    """Own one process/log/resource lifecycle and return one immutable outcome."""
    attempt = context.attempt
    configuration = attempt.configuration
    measurement: ResourceMeasurement | None = None
    stdout_identity: _ObjectIdentity | None = None
    stderr_identity: _ObjectIdentity | None = None
    deferred_resource_errors: list[BaseException] = []

    def failure(error: BaseException) -> _WorkerOutcome:
        if isinstance(error, RunnerError):
            retained_error = _child_failure(
                str(error),
                context.command,
                context.stdout_path,
                context.stderr_path,
            )
            _copy_error_notes(error, retained_error)
        else:
            retained_error = error
        return _WorkerOutcome(attempt, None, measurement, retained_error)

    try:
        try:
            with (
                _exclusive_regular_log(context.stdout_path) as stdout_pinned,
                _exclusive_regular_log(context.stderr_path) as stderr_pinned,
            ):
                stdout_log, stdout_identity = stdout_pinned
                stderr_log, stderr_identity = stderr_pinned
                try:
                    _verify_attempt_context(
                        root=root,
                        run_parent=run_parent,
                        run_directory=run_directory,
                        run_parent_identity=run_parent_identity,
                        run_directory_identity=run_directory_identity,
                        context=context,
                    )
                    _require_regular_file_no_follow(
                        context.stdout_path,
                        context.attempt_directory,
                        "attempt stdout log",
                        stdout_identity,
                        stdout_log.fileno(),
                    )
                    _require_regular_file_no_follow(
                        context.stderr_path,
                        context.attempt_directory,
                        "attempt stderr log",
                        stderr_identity,
                        stderr_log.fileno(),
                    )

                    def retain_attempt_resource(
                        retained_measurement: ResourceMeasurement,
                    ) -> None:
                        nonlocal measurement
                        measurement = retained_measurement
                        record = build_attempt_resource_record(
                            configuration.experiment_id,
                            attempt.repetition_attempt,
                            retained_measurement,
                        )
                        _verify_attempt_context(
                            root=root,
                            run_parent=run_parent,
                            run_directory=run_directory,
                            run_parent_identity=run_parent_identity,
                            run_directory_identity=run_directory_identity,
                            context=context,
                        )
                        _publish_resource_document(
                            context.resource_usage_path,
                            record,
                            "attempt resource usage",
                            context.attempt_identity,
                        )

                    return_code = _run_process(
                        process_factory,
                        context.command,
                        root,
                        stdout_log,
                        stderr_log,
                        PROCESS_TIMEOUT_SECONDS,
                        resource_capability=resource_capability,
                        resource_monitor_factory=resource_monitor_factory,
                        resource_callback=retain_attempt_resource,
                        deferred_resource_errors=deferred_resource_errors,
                        process_registry=process_registry,
                    )
                finally:
                    _verify_attempt_context(
                        root=root,
                        run_parent=run_parent,
                        run_directory=run_directory,
                        run_parent_identity=run_parent_identity,
                        run_directory_identity=run_directory_identity,
                        context=context,
                    )
                    _require_regular_file_no_follow(
                        context.stdout_path,
                        context.attempt_directory,
                        "attempt stdout log",
                        stdout_identity,
                        stdout_log.fileno(),
                    )
                    _require_regular_file_no_follow(
                        context.stderr_path,
                        context.attempt_directory,
                        "attempt stderr log",
                        stderr_identity,
                        stderr_log.fileno(),
                    )
        except RunnerError as error:
            return failure(error)

        _verify_attempt_context(
            root=root,
            run_parent=run_parent,
            run_directory=run_directory,
            run_parent_identity=run_parent_identity,
            run_directory_identity=run_directory_identity,
            context=context,
        )
        _require_regular_file_no_follow(
            context.stdout_path,
            context.attempt_directory,
            "attempt stdout log",
            stdout_identity,
        )
        _require_regular_file_no_follow(
            context.stderr_path,
            context.attempt_directory,
            "attempt stderr log",
            stderr_identity,
        )
        if return_code != 0:
            error = _child_failure(
                f"benchmark process failed with return code {return_code}",
                context.command,
                context.stdout_path,
                context.stderr_path,
            )
            _attach_deferred_resource_errors(error, deferred_resource_errors)
            return _WorkerOutcome(attempt, None, measurement, error)
        try:
            _require_regular_file_no_follow(
                context.output_path,
                context.attempt_directory,
                "benchmark output",
            )
        except RunnerError as error:
            return failure(error)

        expected_configuration = _expected_configuration(
            loaded,
            configuration,
            attempt.repetition_attempt,
            context.attempt_directory,
        )
        try:
            rows = load_output_document(
                context.output_path,
                configuration,
                attempt.repetition_attempt,
                expected_configuration=expected_configuration,
            )
        except OutputValidationError as error:
            return failure(
                RunnerError(f"benchmark output validation failed: {error}")
            )
        try:
            _raise_deferred_resource_errors(
                deferred_resource_errors,
                context.command,
                context.stdout_path,
                context.stderr_path,
            )
        except RunnerError as error:
            return _WorkerOutcome(attempt, None, measurement, error)
        _verify_attempt_context(
            root=root,
            run_parent=run_parent,
            run_directory=run_directory,
            run_parent_identity=run_parent_identity,
            run_directory_identity=run_directory_identity,
            context=context,
        )
        _require_regular_file_no_follow(
            context.stdout_path,
            context.attempt_directory,
            "attempt stdout log",
            stdout_identity,
        )
        _require_regular_file_no_follow(
            context.stderr_path,
            context.attempt_directory,
            "attempt stderr log",
            stderr_identity,
        )
        return _WorkerOutcome(attempt, rows, measurement, None)
    except BaseException as error:
        return failure(error)


def run_benchmark(
    *,
    ns3_root: str | Path,
    config_path: str | Path,
    timestamp: str | None = None,
    configurations: Iterable[ExperimentConfiguration] | None = None,
    experiment_ids: Iterable[int] | None = None,
    traffic_warmup_seconds: int | None = None,
    jobs: int = 0,
    memory_reserve_percent: int = DEFAULT_MEMORY_RESERVE_PERCENT,
    process_factory: Callable[..., object] = subprocess.Popen,
    build_runner: Callable[[Sequence[str], Path], int] = _default_build_runner,
    resource_capability: ResourceCapability | None = None,
    resource_monitor_factory: Callable[..., object] = ProcessTreeResourceMonitor,
    memory_snapshot_reader: Callable[[], MemorySnapshot] | None = None,
    active_rss_reader: Callable[[tuple[int, ...]], Iterable[int]] | None = None,
    logical_cpu_count: int | None = None,
    output: TextIO | None = None,
) -> Path:
    """Build once, supervise bounded workers, and publish canonical CSV batches."""
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
    selection = _select_run(configurations, experiment_ids)
    if traffic_warmup_seconds is not None:
        if (
            type(traffic_warmup_seconds) is not int
            or not 0 <= traffic_warmup_seconds <= _UINT32_MAX
        ):
            raise RunnerError("traffic_warmup_seconds must be a uint32 integer")
        selected_warmup_seconds = traffic_warmup_seconds
    elif configurations is None:
        selected_warmup_seconds = loaded.effective_configuration["benchmark"][
            "traffic_warmup_seconds"
        ]
    else:
        selected_warmup_seconds = None
    if selected_warmup_seconds is not None:
        selection = replace(
            selection,
            configurations=tuple(
                replace(
                    configuration,
                    traffic_warmup_seconds=selected_warmup_seconds,
                )
                for configuration in selection.configurations
            ),
        )
    try:
        ResourceScheduler(1, memory_reserve_percent)
    except SchedulerError as error:
        raise RunnerError(f"invalid benchmark scheduler configuration: {error}") from error
    resource_capability = (
        resource_capability
        if resource_capability is not None
        else detect_resource_capability()
    )
    try:
        maximum_workers = calculate_max_workers(
            logical_cpu_count,
            jobs=jobs,
            sequential_only=resource_capability.sequential_only,
        )
    except SchedulerError as error:
        raise RunnerError(f"invalid benchmark scheduler configuration: {error}") from error
    if resource_capability.sequential_only:
        print(f"resource: {resource_capability.diagnostic}", file=output, flush=True)

    build_command = build_ns3_build_command(ns3)
    try:
        build_return_code = build_runner(build_command, root)
    except RunnerError:
        raise
    except BaseException as error:
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        raise RunnerError(f"cannot run saturated benchmark build: {error}") from error
    if (
        isinstance(build_return_code, bool)
        or not isinstance(build_return_code, int)
    ):
        raise RunnerError("saturated benchmark build returned an invalid exit code")
    if build_return_code != 0:
        raise RunnerError(
            f"saturated benchmark build failed with return code {build_return_code}"
        )

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
        run_directory,
        run_parent,
        "benchmark timestamp directory",
    )

    attempts = tuple(
        iter_experiment_attempts(selection.configurations, loaded.repetitions)
    )
    try:
        execution_plan = plan_execution(
            attempts,
            complete_matrix=selection.complete_matrix,
        )
    except SchedulerError as error:
        raise RunnerError(f"cannot plan benchmark execution: {error}") from error
    state = _ResourceSummaryState(
        requested_experiment_ids=selection.requested_experiment_ids,
        executed_experiment_ids=selection.executed_experiment_ids,
        auto_included_baseline_ids=selection.auto_included_baseline_ids,
        complete_matrix=selection.complete_matrix,
        memory_reserve_percent=memory_reserve_percent,
    )
    try:
        csv_output = ExcelCsvWriter(run_directory / "results.csv")
    except FileExistsError as error:
        raise RunnerError(
            f"benchmark CSV already exists: {run_directory / 'results.csv'}"
        ) from error
    except OSError as error:
        raise RunnerError(f"cannot create benchmark CSV: {error}") from error

    results_path = run_directory / "results.csv"
    results_identity = _ObjectIdentity(*csv_output.identity())
    experiment_identities: dict[int, _ObjectIdentity] = {}
    baselines: dict[BaselineKey, float] = {}
    completion_buffer: dict[tuple[int, int], tuple[BssCsvRow, ...]] = {}
    canonical_keys = tuple(
        (
            attempt.configuration.experiment_id,
            attempt.repetition_attempt,
        )
        for attempt in execution_plan.canonical_attempts
    )
    next_publication_index = 0
    recorded_resource_keys: set[tuple[int, int]] = set()
    process_registry = _ActiveProcessRegistry()
    completion_queue: queue.Queue[Future[_WorkerOutcome]] = queue.Queue()
    active_futures: set[Future[_WorkerOutcome]] = set()
    executor = ThreadPoolExecutor(
        max_workers=maximum_workers,
        thread_name_prefix="saturated-tcp-worker",
    )
    scheduler_policy: ResourceScheduler | None = None
    admitted_count = 0

    if not resource_capability.sequential_only:
        if memory_snapshot_reader is None:
            assert resource_capability.meminfo_path is not None
            memory_path = resource_capability.meminfo_path

            def default_memory_snapshot_reader() -> MemorySnapshot:
                return read_memory_snapshot(memory_path)

            memory_snapshot_reader = default_memory_snapshot_reader
        if active_rss_reader is None:
            assert resource_capability.proc_root is not None
            process_root = resource_capability.proc_root

            def default_active_rss_reader(
                process_ids: tuple[int, ...],
            ) -> Iterable[int]:
                return tuple(
                    process_tree_rss_bytes(process_id, process_root)
                    for process_id in process_ids
                )

            active_rss_reader = default_active_rss_reader

    def read_snapshot() -> MemorySnapshot:
        assert memory_snapshot_reader is not None
        try:
            snapshot = memory_snapshot_reader()
        except (OSError, ResourceError, SchedulerError) as error:
            raise RunnerError(f"cannot read scheduler memory snapshot: {error}") from error
        return snapshot

    def note_memory_snapshot(snapshot: MemorySnapshot) -> None:
        try:
            policy = scheduler_policy or ResourceScheduler(
                1,
                memory_reserve_percent,
            )
            breached = policy.acceptance_floor_breached(snapshot)
            available_percent = (
                snapshot.mem_available_bytes * 100.0 / snapshot.mem_total_bytes
            )
            if (
                state.controller_minimum_mem_available_bytes is None
                or snapshot.mem_available_bytes
                < state.controller_minimum_mem_available_bytes
            ):
                state.controller_minimum_mem_available_bytes = (
                    snapshot.mem_available_bytes
                )
            if (
                state.controller_minimum_mem_available_percent is None
                or available_percent
                < state.controller_minimum_mem_available_percent
            ):
                state.controller_minimum_mem_available_percent = available_percent
            if breached:
                state.acceptance_floor_breached = True
        except SchedulerError as error:
            raise RunnerError(f"invalid scheduler memory snapshot: {error}") from error

    def record_resource(outcome: _WorkerOutcome) -> None:
        key = (
            outcome.attempt.configuration.experiment_id,
            outcome.attempt.repetition_attempt,
        )
        if outcome.measurement is None or key in recorded_resource_keys:
            return
        recorded_resource_keys.add(key)
        record = build_attempt_resource_record(
            key[0],
            key[1],
            outcome.measurement,
        )
        state.attempt_records.append(record)
        minimum_percent = outcome.measurement.minimum_mem_available_percent
        if minimum_percent is not None and minimum_percent < MIN_MEMORY_RESERVE_PERCENT:
            state.acceptance_floor_breached = True

    def verify_csv() -> None:
        _require_regular_file_no_follow(
            results_path,
            run_directory,
            "benchmark CSV",
            results_identity,
            csv_output.fileno(),
        )

    def publish_contiguous_results() -> None:
        nonlocal next_publication_index
        verify_csv()
        while next_publication_index < len(canonical_keys):
            key = canonical_keys[next_publication_index]
            if key not in completion_buffer:
                break
            rows = completion_buffer[key]
            try:
                published_rows = apply_matching_baseline(rows, baselines)
            except ValueError as error:
                raise RunnerError(f"cannot apply matching baseline: {error}") from error
            try:
                csv_output.append_attempt(published_rows)
            except (OSError, ValueError) as error:
                raise RunnerError(f"cannot append benchmark CSV attempt {key}: {error}") from error
            verify_csv()
            del completion_buffer[key]
            next_publication_index += 1

    def accept_outcome(outcome: _WorkerOutcome) -> None:
        nonlocal scheduler_policy
        record_resource(outcome)
        if outcome.error is not None:
            raise outcome.error
        if outcome.rows is None:
            raise RunnerError("benchmark worker returned no rows and no error")
        measurement = outcome.measurement
        if scheduler_policy is not None and measurement is not None:
            if measurement.peak_rss_bytes is not None:
                try:
                    scheduler_policy = scheduler_policy.observe_peak(
                        measurement.peak_rss_bytes
                    )
                except SchedulerError as error:
                    raise RunnerError(
                        f"invalid observed worker peak: {error}"
                    ) from error
                state.worker_peak_estimate_bytes = (
                    scheduler_policy.worker_peak_estimate_bytes
                )
        configuration = outcome.attempt.configuration
        if configuration.sta_count_per_bss == 1:
            for row in outcome.rows:
                baseline_key: BaselineKey = (
                    configuration.rssi_range,
                    configuration.interference_mode,
                    configuration.traffic_mode,
                    configuration.mimo_mode,
                    outcome.attempt.repetition_attempt,
                    row.bss_id,
                )
                if baseline_key in baselines:
                    raise RunnerError(
                        f"duplicate matching baseline for {baseline_key!r}"
                    )
                baselines[baseline_key] = (
                    row.aggregate_data_tx_rate_over_interval_mbps
                )
        outcome_key = (
            configuration.experiment_id,
            outcome.attempt.repetition_attempt,
        )
        if outcome_key in completion_buffer:
            raise RunnerError(f"duplicate completed attempt {outcome_key}")
        completion_buffer[outcome_key] = outcome.rows
        publish_contiguous_results()

    def submit_attempt(attempt: ExperimentAttempt) -> Future[_WorkerOutcome]:
        nonlocal admitted_count
        context = _prepare_attempt_context(
            root=root,
            run_parent=run_parent,
            run_directory=run_directory,
            run_parent_identity=run_parent_identity,
            run_directory_identity=run_directory_identity,
            experiment_identities=experiment_identities,
            ns3=ns3,
            resolved_config=resolved_config,
            attempt=attempt,
        )
        admitted_count += 1
        print(
            f"[{admitted_count}/{len(attempts)}] experiment "
            f"{attempt.configuration.experiment_id:03d}, "
            f"attempt {attempt.repetition_attempt}",
            file=output,
            flush=True,
        )
        future = executor.submit(
            _run_attempt_worker,
            root=root,
            run_parent=run_parent,
            run_directory=run_directory,
            run_parent_identity=run_parent_identity,
            run_directory_identity=run_directory_identity,
            loaded=loaded,
            context=context,
            process_factory=process_factory,
            resource_capability=resource_capability,
            resource_monitor_factory=resource_monitor_factory,
            process_registry=process_registry,
        )
        active_futures.add(future)
        future.add_done_callback(completion_queue.put)
        return future

    def process_completion(
        future: Future[_WorkerOutcome],
        phase_outcomes: list[_WorkerOutcome],
    ) -> None:
        if future not in active_futures:
            return
        active_futures.remove(future)
        if future.cancelled():
            return
        outcome = future.result()
        phase_outcomes.append(outcome)
        accept_outcome(outcome)

    def drain_ready_completions(
        phase_outcomes: list[_WorkerOutcome],
    ) -> bool:
        consumed = False
        while True:
            try:
                completed_future = completion_queue.get_nowait()
            except queue.Empty:
                return consumed
            process_completion(completed_future, phase_outcomes)
            consumed = True

    def execute_phase(
        phase_attempts: Iterable[ExperimentAttempt],
        *,
        calibration: bool = False,
    ) -> tuple[_WorkerOutcome, ...]:
        pending = deque(phase_attempts)
        phase_outcomes: list[_WorkerOutcome] = []
        while pending or active_futures:
            drain_ready_completions(phase_outcomes)

            admission_blocked = False
            while pending and len(active_futures) < maximum_workers:
                if resource_capability.sequential_only:
                    can_admit = True
                else:
                    if calibration:
                        snapshot = read_snapshot()
                        note_memory_snapshot(snapshot)
                        try:
                            calibration_policy = ResourceScheduler(
                                1,
                                memory_reserve_percent,
                            )
                            can_admit = (
                                snapshot.mem_available_bytes
                                >= calibration_policy.target_reserve_bytes(snapshot)
                            )
                        except (AttributeError, SchedulerError) as error:
                            raise RunnerError(
                                f"invalid calibration memory snapshot: {error}"
                            ) from error
                    else:
                        if scheduler_policy is None:
                            raise RunnerError(
                                "worker scheduler was not calibrated before admission"
                            )
                        assert active_rss_reader is not None
                        process_ids = process_registry.process_ids()
                        try:
                            active_rss = tuple(active_rss_reader(process_ids))
                        except (OSError, ResourceError, SchedulerError) as error:
                            raise RunnerError(
                                f"cannot read active worker RSS: {error}"
                            ) from error
                        if len(active_rss) != len(process_ids):
                            raise RunnerError(
                                "active worker RSS reader returned the wrong number of values"
                            )
                        if len(active_rss) > len(active_futures):
                            raise RunnerError(
                                "active process registry exceeds submitted worker count"
                            )
                        active_rss += (0,) * (
                            len(active_futures) - len(active_rss)
                        )
                        snapshot = read_snapshot()
                        note_memory_snapshot(snapshot)
                        try:
                            can_admit = scheduler_policy.can_admit(
                                snapshot,
                                active_rss,
                            )
                        except SchedulerError as error:
                            raise RunnerError(
                                f"cannot make worker admission decision: {error}"
                            ) from error
                if not can_admit:
                    admission_blocked = True
                    break
                if drain_ready_completions(phase_outcomes):
                    continue
                submit_attempt(pending.popleft())
                try:
                    completed_future = completion_queue.get_nowait()
                except queue.Empty:
                    pass
                else:
                    process_completion(completed_future, phase_outcomes)
                if calibration:
                    break

            if pending and not active_futures and admission_blocked:
                raise RunnerError(
                    "insufficient available memory to admit any benchmark worker"
                )
            if active_futures:
                try:
                    completed_future = completion_queue.get(timeout=0.1)
                except queue.Empty:
                    continue
                process_completion(completed_future, phase_outcomes)
            elif pending:
                continue
        return tuple(phase_outcomes)

    def drain_after_failure(primary_error: BaseException) -> None:
        try:
            process_registry.stop_admissions()
        except BaseException as cleanup_error:
            primary_error.add_note(
                f"secondary worker admission-stop failure: {cleanup_error}"
            )
        for future in tuple(active_futures):
            future.cancel()
        try:
            process_registry.terminate_active()
        except BaseException as cleanup_error:
            primary_error.add_note(
                f"secondary active process-group termination failure: {cleanup_error}"
            )
        while active_futures:
            try:
                completed_future = completion_queue.get(timeout=0.1)
            except queue.Empty:
                continue
            if completed_future not in active_futures:
                continue
            active_futures.remove(completed_future)
            if completed_future.cancelled():
                continue
            try:
                outcome = completed_future.result()
                record_resource(outcome)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"secondary worker drain failure: {cleanup_error}"
                )

    primary_error: BaseException | None = None
    with (
        _close_csv_after_summary(csv_output),
        _retain_resource_summary(
            root=root,
            run_parent=run_parent,
            run_directory=run_directory,
            run_parent_identity=run_parent_identity,
            run_directory_identity=run_directory_identity,
            state=state,
        ),
    ):
        verify_csv()
        try:
            calibration_outcomes = execute_phase(
                (execution_plan.calibration,),
                calibration=True,
            )
            if len(calibration_outcomes) != 1:
                raise RunnerError("calibration did not produce exactly one outcome")
            calibration_measurement = calibration_outcomes[0].measurement
            if not resource_capability.sequential_only:
                if (
                    calibration_measurement is None
                    or calibration_measurement.peak_rss_bytes is None
                ):
                    raise RunnerError("calibration did not retain a worker peak RSS")
                state.calibrated_peak_rss_bytes = (
                    calibration_measurement.peak_rss_bytes
                )
                try:
                    initial_estimate = calculate_worker_peak_estimate(
                        calibration_measurement.peak_rss_bytes
                    )
                    scheduler_policy = ResourceScheduler(
                        initial_estimate,
                        memory_reserve_percent,
                    )
                except SchedulerError as error:
                    raise RunnerError(f"invalid calibration result: {error}") from error
                state.worker_peak_estimate_bytes = initial_estimate
            execute_phase(execution_plan.baseline_wave)
            execute_phase(execution_plan.remaining_attempts)
            publish_contiguous_results()
            if next_publication_index != len(canonical_keys):
                raise RunnerError("benchmark completed with a non-contiguous result buffer")
            if state.complete_matrix and state.acceptance_floor_breached:
                raise RunnerError(
                    "complete benchmark breached the 15 percent available-memory "
                    "acceptance floor"
                )
        except BaseException as error:
            primary_error = error
            drain_after_failure(error)
            raise
        finally:
            state.maximum_parallel_workers = process_registry.maximum_processes()
            try:
                executor.shutdown(wait=True, cancel_futures=True)
            except BaseException as cleanup_error:
                if primary_error is not None:
                    primary_error.add_note(
                        f"secondary worker executor shutdown failure: {cleanup_error}"
                    )
                else:
                    raise

    print(
        f"Completed {len(attempts)} attempts: {run_directory}",
        file=output,
        flush=True,
    )
    return run_directory


def parse_experiment_ids(value: str) -> tuple[int, ...]:
    """Parse one nonempty comma-separated explicit matrix subset."""
    if not isinstance(value, str) or not value.strip():
        raise ValueError("experiment ID list must not be empty")
    fields = value.split(",")
    if any(not field.strip() for field in fields):
        raise ValueError("experiment ID list contains an empty field")
    try:
        requested = tuple(int(field.strip(), 10) for field in fields)
    except ValueError as error:
        raise ValueError("experiment IDs must be decimal integers") from error
    try:
        expand_experiment_ids(requested)
    except ValueError as error:
        raise ValueError(str(error)) from error
    return requested


def _parse_jobs(value: str) -> int:
    try:
        jobs = int(value, 10)
    except ValueError as error:
        raise ValueError("jobs must be a non-negative integer") from error
    if jobs < 0:
        raise ValueError("jobs must be a non-negative integer")
    return jobs


def _parse_memory_reserve_percent(value: str) -> int:
    try:
        reserve = int(value, 10)
        ResourceScheduler(1, reserve)
    except (ValueError, SchedulerError) as error:
        raise ValueError(
            "memory reserve percent must be an integer in [15, 50]"
        ) from error
    return reserve


def _parse_traffic_warmup_seconds(value: str) -> int:
    try:
        warmup_seconds = int(value, 10)
    except ValueError as error:
        raise ValueError("traffic warm-up seconds must be a uint32 integer") from error
    if not 0 <= warmup_seconds <= _UINT32_MAX:
        raise ValueError("traffic warm-up seconds must be a uint32 integer")
    return warmup_seconds


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the resource-aware saturated TCP benchmark matrix.",
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
    parser.add_argument(
        "--jobs",
        type=_parse_jobs,
        default=0,
        help="maximum parallel workers; 0 selects the automatic CPU cap",
    )
    parser.add_argument(
        "--memory-reserve-percent",
        type=_parse_memory_reserve_percent,
        default=DEFAULT_MEMORY_RESERVE_PERCENT,
        help="MemAvailable target percentage in the inclusive range 15 through 50",
    )
    parser.add_argument(
        "--experiment-ids",
        type=parse_experiment_ids,
        default=None,
        metavar="LIST",
        help="comma-separated development subset; matching one-STA baselines are automatic",
    )
    parser.add_argument(
        "--traffic-warmup-seconds",
        type=_parse_traffic_warmup_seconds,
        default=None,
        help="fixed saturated traffic warm-up for every selected configuration",
    )
    return parser


def main(
    argv: Sequence[str] | None = None,
    *,
    process_factory: Callable[..., object] = subprocess.Popen,
    build_runner: Callable[[Sequence[str], Path], int] = _default_build_runner,
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
            experiment_ids=arguments.experiment_ids,
            traffic_warmup_seconds=arguments.traffic_warmup_seconds,
            jobs=arguments.jobs,
            memory_reserve_percent=arguments.memory_reserve_percent,
            process_factory=process_factory,
            build_runner=build_runner,
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
