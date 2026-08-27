"""Linux process-tree memory measurement and durable resource JSON files."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import json
import os
from pathlib import Path
import secrets
import stat
import threading
import time


SAMPLE_INTERVAL_MS = 100
LINUX_PROC_MONITOR_MODE = "linux_proc"
SEQUENTIAL_FALLBACK_MONITOR_MODE = "sequential_fallback"
_KIBIBYTE = 1024


class ResourceError(RuntimeError):
    """A resource parser, monitor, or publication failure."""


@dataclass(frozen=True)
class MemorySnapshot:
    """Linux host memory values captured from one meminfo read."""

    mem_total_bytes: int
    mem_available_bytes: int


@dataclass(frozen=True)
class AttemptResourceUsage:
    """Measured Linux process-tree usage for one completed subprocess."""

    sample_interval_ms: int
    peak_rss_bytes: int
    wall_time_seconds: float
    exit_code: int


@dataclass(frozen=True)
class ResourceCapability:
    """Resource-monitor support fixed once at controller startup."""

    monitor_mode: str
    proc_root: Path | None
    meminfo_path: Path | None
    initial_memory_snapshot: MemorySnapshot | None
    diagnostic: str

    @property
    def sequential_only(self) -> bool:
        """Return whether process-tree memory measurements are unavailable."""
        return self.monitor_mode == SEQUENTIAL_FALLBACK_MONITOR_MODE


@dataclass(frozen=True)
class ResourceMeasurement:
    """One monitor result, including nullable fields in fallback mode."""

    sample_interval_ms: int
    peak_rss_bytes: int | None
    minimum_mem_available_bytes: int | None
    minimum_mem_available_percent: float | None
    wall_time_seconds: float
    exit_code: int
    monitor_mode: str

    @property
    def usage(self) -> AttemptResourceUsage:
        """Return the required Linux usage interface for a measured attempt."""
        if self.peak_rss_bytes is None:
            raise ResourceError("process-tree RSS is unavailable in sequential fallback mode")
        return AttemptResourceUsage(
            sample_interval_ms=self.sample_interval_ms,
            peak_rss_bytes=self.peak_rss_bytes,
            wall_time_seconds=self.wall_time_seconds,
            exit_code=self.exit_code,
        )


def _parse_kibibyte_field(contents: str, field: str, source: Path) -> int:
    prefix = f"{field}:"
    for line in contents.splitlines():
        if not line.startswith(prefix):
            continue
        parts = line[len(prefix) :].split()
        if len(parts) != 2 or parts[1] != "kB":
            raise ResourceError(f"malformed {field} field in {source}")
        try:
            value = int(parts[0], 10)
        except ValueError as error:
            raise ResourceError(f"malformed {field} field in {source}") from error
        if value < 0:
            raise ResourceError(f"malformed {field} field in {source}")
        return value * _KIBIBYTE
    raise ResourceError(f"missing {field} field in {source}")


def read_memory_snapshot(meminfo_path: Path = Path("/proc/meminfo")) -> MemorySnapshot:
    """Read required host memory fields and convert Linux kB to bytes."""
    path = Path(meminfo_path)
    try:
        contents = path.read_text(encoding="ascii")
    except OSError as error:
        raise ResourceError(f"memory information is unavailable at {path}: {error}") from error
    mem_total_bytes = _parse_kibibyte_field(contents, "MemTotal", path)
    mem_available_bytes = _parse_kibibyte_field(contents, "MemAvailable", path)
    if mem_total_bytes <= 0:
        raise ResourceError(f"malformed MemTotal field in {path}: expected positive memory")
    if mem_available_bytes > mem_total_bytes:
        raise ResourceError(
            f"malformed MemAvailable field in {path}: exceeds MemTotal"
        )
    return MemorySnapshot(mem_total_bytes, mem_available_bytes)


def _read_process_rss_bytes(process_id: int, proc_root: Path) -> int | None:
    status_path = proc_root / str(process_id) / "status"
    try:
        contents = status_path.read_text(encoding="ascii")
    except FileNotFoundError:
        return None
    except OSError as error:
        raise ResourceError(
            f"cannot read process status for PID {process_id} at {status_path}: {error}"
        ) from error
    try:
        return _parse_kibibyte_field(contents, "VmRSS", status_path)
    except ResourceError:
        if any(
            line.startswith("State:") and line[len("State:") :].lstrip().startswith("Z")
            for line in contents.splitlines()
        ):
            return None
        raise


def _read_process_children(process_id: int, proc_root: Path) -> tuple[int, ...]:
    task_root = proc_root / str(process_id) / "task"
    try:
        task_directories = tuple(task_root.iterdir())
    except FileNotFoundError:
        return ()
    except OSError as error:
        raise ResourceError(
            f"cannot enumerate threads for PID {process_id} at {task_root}: {error}"
        ) from error

    children = []
    for task_directory in task_directories:
        children_path = task_directory / "children"
        try:
            fields = children_path.read_text(encoding="ascii").split()
        except FileNotFoundError:
            continue
        except OSError as error:
            raise ResourceError(
                f"cannot read process children for PID {process_id} at "
                f"{children_path}: {error}"
            ) from error
        for field in fields:
            try:
                child = int(field, 10)
            except ValueError as error:
                raise ResourceError(
                    f"malformed child PID {field!r} in {children_path}"
                ) from error
            if child <= 0:
                raise ResourceError(f"malformed child PID {field!r} in {children_path}")
            children.append(child)
    return tuple(children)


def process_tree_rss_bytes(root_pid: int, proc_root: Path = Path("/proc")) -> int:
    """Conservatively sum current RSS for a root and all recursive descendants."""
    if isinstance(root_pid, bool) or not isinstance(root_pid, int) or root_pid <= 0:
        raise ResourceError("root process ID must be a positive integer")
    root = Path(proc_root)
    pending = [root_pid]
    observed: set[int] = set()
    total = 0
    while pending:
        process_id = pending.pop()
        if process_id in observed:
            continue
        observed.add(process_id)
        rss_bytes = _read_process_rss_bytes(process_id, root)
        if rss_bytes is None:
            continue
        total += rss_bytes
        pending.extend(_read_process_children(process_id, root))
    return total


def detect_resource_capability(
    proc_root: Path = Path("/proc"),
    meminfo_path: Path = Path("/proc/meminfo"),
) -> ResourceCapability:
    """Detect Linux proc support or return an explicit sequential-only fallback."""
    root = Path(proc_root)
    memory_path = Path(meminfo_path)
    try:
        root_status = root.lstat()
        if stat.S_ISLNK(root_status.st_mode) or not stat.S_ISDIR(root_status.st_mode):
            raise ResourceError(f"process information root is unavailable at {root}")
        snapshot = read_memory_snapshot(memory_path)
        self_rss = process_tree_rss_bytes(os.getpid(), root)
        if self_rss <= 0:
            raise ResourceError(f"process RSS is unavailable at {root}")
    except (OSError, ResourceError) as error:
        return ResourceCapability(
            monitor_mode=SEQUENTIAL_FALLBACK_MONITOR_MODE,
            proc_root=None,
            meminfo_path=None,
            initial_memory_snapshot=None,
            diagnostic=(
                "Linux /proc resource monitoring unavailable; using sequential "
                f"fallback: {error}"
            ),
        )
    return ResourceCapability(
        monitor_mode=LINUX_PROC_MONITOR_MODE,
        proc_root=root,
        meminfo_path=memory_path,
        initial_memory_snapshot=snapshot,
        diagnostic="Linux /proc process-tree resource monitoring enabled",
    )


class ProcessTreeResourceMonitor:
    """Sample one process tree in a bounded background thread."""

    def __init__(
        self,
        root_pid: int,
        capability: ResourceCapability,
        *,
        sample_interval_ms: int = SAMPLE_INTERVAL_MS,
    ) -> None:
        if isinstance(root_pid, bool) or not isinstance(root_pid, int) or root_pid <= 0:
            raise ResourceError("root process ID must be a positive integer")
        if sample_interval_ms != SAMPLE_INTERVAL_MS:
            raise ResourceError(f"resource sample interval must be {SAMPLE_INTERVAL_MS} ms")
        self._root_pid = root_pid
        self._capability = capability
        self._sample_interval_ms = sample_interval_ms
        self._started_at: float | None = None
        self._peak_rss_bytes = 0
        self._minimum_mem_available_bytes: int | None = None
        self._minimum_mem_available_percent: float | None = None
        self._error: BaseException | None = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def _sample(self) -> None:
        if self._capability.sequential_only:
            return
        assert self._capability.proc_root is not None
        assert self._capability.meminfo_path is not None
        rss_bytes = process_tree_rss_bytes(self._root_pid, self._capability.proc_root)
        snapshot = read_memory_snapshot(self._capability.meminfo_path)
        available_percent = (
            snapshot.mem_available_bytes * 100.0 / snapshot.mem_total_bytes
        )
        self._peak_rss_bytes = max(self._peak_rss_bytes, rss_bytes)
        if (
            self._minimum_mem_available_bytes is None
            or snapshot.mem_available_bytes < self._minimum_mem_available_bytes
        ):
            self._minimum_mem_available_bytes = snapshot.mem_available_bytes
        if (
            self._minimum_mem_available_percent is None
            or available_percent < self._minimum_mem_available_percent
        ):
            self._minimum_mem_available_percent = available_percent

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                self._sample()
                self._stop.wait(self._sample_interval_ms / 1000.0)
        except BaseException as error:
            self._error = error
            self._stop.set()

    def start(self) -> None:
        """Start measurement exactly once, with an immediate first sample."""
        if self._started_at is not None:
            raise ResourceError("resource monitor was already started")
        self._started_at = time.monotonic()
        if self._capability.sequential_only:
            return
        self._thread = threading.Thread(
            target=self._run,
            name=f"resource-monitor-{self._root_pid}",
            daemon=True,
        )
        self._thread.start()

    def finish(self, exit_code: int) -> ResourceMeasurement:
        """Stop measurement and return the retained result exactly once."""
        if self._started_at is None:
            raise ResourceError("resource monitor was not started")
        if isinstance(exit_code, bool) or not isinstance(exit_code, int):
            raise ResourceError("resource monitor exit code must be an integer")
        self._stop.set()
        if self._thread is not None:
            self._thread.join()
        wall_time_seconds = time.monotonic() - self._started_at
        self._started_at = None
        if self._error is not None:
            raise ResourceError(
                f"process-tree resource monitor failed: {self._error}"
            ) from self._error
        peak_rss_bytes = None if self._capability.sequential_only else self._peak_rss_bytes
        return ResourceMeasurement(
            sample_interval_ms=self._sample_interval_ms,
            peak_rss_bytes=peak_rss_bytes,
            minimum_mem_available_bytes=self._minimum_mem_available_bytes,
            minimum_mem_available_percent=self._minimum_mem_available_percent,
            wall_time_seconds=wall_time_seconds,
            exit_code=exit_code,
            monitor_mode=self._capability.monitor_mode,
        )


def publish_json_exclusive(path: Path, document: Mapping[str, object]) -> None:
    """Atomically publish ordered JSON without following or replacing a destination."""
    output_path = Path(path)
    parent_path = output_path.parent
    parent_descriptor = os.open(
        parent_path,
        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
    )
    temporary_name = (
        f".{output_path.name}.{os.getpid()}.{secrets.token_hex(8)}.tmp"
    )
    temporary_descriptor: int | None = None
    try:
        temporary_descriptor = os.open(
            temporary_name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
            0o644,
            dir_fd=parent_descriptor,
        )
        payload = (
            json.dumps(
                document,
                ensure_ascii=True,
                allow_nan=False,
                separators=(",", ":"),
            )
            + "\n"
        ).encode("utf-8")
        offset = 0
        while offset < len(payload):
            offset += os.write(temporary_descriptor, payload[offset:])
        os.fsync(temporary_descriptor)
        os.close(temporary_descriptor)
        temporary_descriptor = None
        os.link(
            temporary_name,
            output_path.name,
            src_dir_fd=parent_descriptor,
            dst_dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        os.fsync(parent_descriptor)
    finally:
        if temporary_descriptor is not None:
            os.close(temporary_descriptor)
        try:
            os.unlink(temporary_name, dir_fd=parent_descriptor)
        except FileNotFoundError:
            pass
        os.close(parent_descriptor)


def build_attempt_resource_record(
    experiment_id: int,
    repetition_attempt: int,
    measurement: ResourceMeasurement,
) -> dict[str, object]:
    """Build the exact ordered schema-1 record for one created attempt."""
    return {
        "schema_version": 1,
        "experiment_id": experiment_id,
        "repetition_attempt": repetition_attempt,
        "sample_interval_ms": measurement.sample_interval_ms,
        "peak_rss_bytes": measurement.peak_rss_bytes,
        "minimum_mem_available_bytes": measurement.minimum_mem_available_bytes,
        "minimum_mem_available_percent": measurement.minimum_mem_available_percent,
        "wall_time_seconds": measurement.wall_time_seconds,
        "exit_code": measurement.exit_code,
        "monitor_mode": measurement.monitor_mode,
    }


def build_sequential_resource_summary(
    requested_experiment_ids: tuple[int, ...],
    complete_matrix: bool,
    attempt_records: list[dict[str, object]],
) -> dict[str, object]:
    """Build the exact ordered schema-1 summary for the sequential runner."""
    ordered_attempts = sorted(
        attempt_records,
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
    return {
        "schema_version": 1,
        "complete_matrix": complete_matrix,
        "requested_experiment_ids": list(requested_experiment_ids),
        "executed_experiment_ids": list(requested_experiment_ids),
        "auto_included_baseline_ids": [],
        "memory_reserve_percent": 20,
        "calibrated_peak_rss_bytes": None,
        "worker_peak_estimate_bytes": None,
        "maximum_parallel_workers": 1,
        "minimum_mem_available_bytes": min(available_bytes, default=None),
        "minimum_mem_available_percent": min(available_percent, default=None),
        "attempts": ordered_attempts,
    }
