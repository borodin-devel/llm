"""Linux process-tree memory measurement and durable resource JSON files."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import errno
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
_PROC_DIAGNOSTIC_CHARACTERS = 1024


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
class _ProcessIdentity:
    """One PID instance with its current state and direct parent."""

    process_id: int
    parent_pid: int
    start_time_ticks: int


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


def _is_process_disappearance(error: OSError) -> bool:
    return isinstance(error, (FileNotFoundError, ProcessLookupError)) or error.errno in (
        errno.ENOENT,
        errno.ESRCH,
    )


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


def _bounded_proc_diagnostic(contents: str) -> str:
    excerpt = contents[:_PROC_DIAGNOSTIC_CHARACTERS]
    if len(contents) > len(excerpt):
        excerpt += "<truncated>"
    return repr(excerpt)


def _read_statm_resident_pages(
    process_id: int,
    proc_root: Path,
    pinned_identity: _ProcessIdentity,
) -> int | None:
    statm_path = proc_root / str(process_id) / "statm"
    try:
        contents = statm_path.read_text(encoding="ascii")
    except OSError as error:
        if _is_process_disappearance(error):
            return None
        current_identity = _read_process_identity(process_id, proc_root)
        if current_identity is None or current_identity != pinned_identity:
            return None
        raise ResourceError(
            f"cannot read process statm for PID {process_id} at "
            f"{statm_path}: {error}"
        ) from error
    except UnicodeError as error:
        current_identity = _read_process_identity(process_id, proc_root)
        if current_identity is None or current_identity != pinned_identity:
            return None
        raise ResourceError(
            f"malformed process statm for PID {process_id} at "
            f"{statm_path}: non-ASCII data"
        ) from error

    current_identity = _read_process_identity(process_id, proc_root)
    if current_identity is None or current_identity != pinned_identity:
        return None

    fields = contents.split()
    if len(fields) < 2 or not fields[1] or any(
        character < "0" or character > "9" for character in fields[1]
    ):
        raise ResourceError(
            f"malformed process statm for PID {process_id} at {statm_path}: "
            f"statm={_bounded_proc_diagnostic(contents)}"
        )
    return int(fields[1], 10)


def _read_process_rss_bytes(
    process_id: int,
    proc_root: Path,
    *,
    page_size: int | None = None,
    expected_identity: _ProcessIdentity | None = None,
) -> int | None:
    status_path = proc_root / str(process_id) / "status"
    try:
        contents = status_path.read_text(encoding="ascii")
    except OSError as error:
        if _is_process_disappearance(error):
            return None
        raise ResourceError(
            f"cannot read process status for PID {process_id} at "
            f"{status_path}: {error}"
        ) from error
    try:
        return _parse_kibibyte_field(contents, "VmRSS", status_path)
    except ResourceError:
        if any(line.startswith("VmRSS:") for line in contents.splitlines()):
            raise

    pinned_identity = _read_process_identity(process_id, proc_root)
    if pinned_identity is None:
        return None
    if expected_identity is not None and pinned_identity != expected_identity:
        return None
    resident_pages = _read_statm_resident_pages(
        process_id,
        proc_root,
        pinned_identity,
    )
    if resident_pages is None:
        return None

    if page_size is None:
        try:
            page_size = os.sysconf("SC_PAGE_SIZE")
        except (AttributeError, OSError, ValueError) as error:
            raise ResourceError(f"host page size is unavailable: {error}") from error
    if isinstance(page_size, bool) or not isinstance(page_size, int) or page_size <= 0:
        raise ResourceError("host page size must be a positive integer")
    return resident_pages * page_size


def _read_process_identity(
    process_id: int,
    proc_root: Path,
) -> _ProcessIdentity | None:
    stat_path = proc_root / str(process_id) / "stat"
    try:
        contents = stat_path.read_text(encoding="ascii")
    except OSError as error:
        if _is_process_disappearance(error):
            return None
        raise ResourceError(
            f"cannot read process identity for PID {process_id} at {stat_path}: {error}"
        ) from error
    prefix, separator, remaining = contents.strip().rpartition(") ")
    fields = remaining.split()
    if (
        not separator
        or not prefix.startswith(f"{process_id} (")
        or len(fields) <= 19
    ):
        raise ResourceError(f"malformed process identity for PID {process_id} at {stat_path}")
    try:
        parent_pid = int(fields[1], 10)
        start_time_ticks = int(fields[19], 10)
    except ValueError as error:
        raise ResourceError(
            f"malformed process identity for PID {process_id} at {stat_path}"
        ) from error
    if parent_pid < 0 or start_time_ticks < 0:
        raise ResourceError(f"malformed process identity for PID {process_id} at {stat_path}")
    return _ProcessIdentity(process_id, parent_pid, start_time_ticks)


def _read_process_children(process_id: int, proc_root: Path) -> tuple[int, ...]:
    task_root = proc_root / str(process_id) / "task"
    try:
        task_directories = tuple(task_root.iterdir())
    except OSError as error:
        if _is_process_disappearance(error):
            return ()
        raise ResourceError(
            f"cannot enumerate threads for PID {process_id} at {task_root}: {error}"
        ) from error

    children = []
    for task_directory in task_directories:
        children_path = task_directory / "children"
        try:
            fields = children_path.read_text(encoding="ascii").split()
        except OSError as error:
            if _is_process_disappearance(error):
                continue
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
    root_identity = _read_process_identity(root_pid, root)
    if root_identity is None:
        return 0
    pending = [(root_identity, None)]
    observed: set[tuple[int, int]] = set()
    total = 0
    while pending:
        expected_identity, expected_parent = pending.pop()
        identity_key = (
            expected_identity.process_id,
            expected_identity.start_time_ticks,
        )
        if identity_key in observed:
            continue
        current_identity = _read_process_identity(expected_identity.process_id, root)
        if current_identity != expected_identity:
            continue
        if (
            expected_parent is not None
            and current_identity.parent_pid != expected_parent.process_id
        ):
            continue
        rss_bytes = _read_process_rss_bytes(
            current_identity.process_id,
            root,
            expected_identity=current_identity,
        )
        if rss_bytes is None:
            continue
        child_pids = _read_process_children(current_identity.process_id, root)
        if _read_process_identity(current_identity.process_id, root) != current_identity:
            continue
        observed.add(identity_key)
        total += rss_bytes
        for child_pid in child_pids:
            child_identity = _read_process_identity(child_pid, root)
            if child_identity is None or child_identity.parent_pid != current_identity.process_id:
                continue
            pending.append((child_identity, current_identity))
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
        self._thread_started = False

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
            while not self._stop.wait(self._sample_interval_ms / 1000.0):
                self._sample()
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
        try:
            self._sample()
        except BaseException as error:
            self._error = error
            self._stop.set()
            raise
        self._thread = threading.Thread(
            target=self._run,
            name=f"resource-monitor-{self._root_pid}",
            daemon=True,
        )
        self._thread_started = True
        self._thread.start()

    def finish(self, exit_code: int) -> ResourceMeasurement:
        """Stop measurement and return the retained result exactly once."""
        if self._started_at is None:
            raise ResourceError("resource monitor was not started")
        if isinstance(exit_code, bool) or not isinstance(exit_code, int):
            raise ResourceError("resource monitor exit code must be an integer")
        self._stop.set()
        if self._thread is not None and self._thread_started:
            try:
                self._thread.join()
            except RuntimeError:
                pass
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


def publish_json_exclusive(
    path: Path,
    document: Mapping[str, object],
    *,
    expected_parent_identity: tuple[int, int] | None = None,
) -> None:
    """Atomically publish ordered JSON without following or replacing a destination."""
    output_path = Path(path)
    parent_path = output_path.parent
    parent_descriptor = os.open(
        parent_path,
        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
    )
    parent_status = os.fstat(parent_descriptor)
    parent_identity = (parent_status.st_dev, parent_status.st_ino)
    if not stat.S_ISDIR(parent_status.st_mode):
        os.close(parent_descriptor)
        raise ResourceError(f"resource parent is not a directory: {parent_path}")
    if expected_parent_identity is not None and parent_identity != expected_parent_identity:
        os.close(parent_descriptor)
        raise ResourceError(f"resource parent identity changed: {parent_path}")
    temporary_name = (
        f".{output_path.name}.{os.getpid()}.{secrets.token_hex(8)}.tmp"
    )
    temporary_descriptor: int | None = None
    destination_descriptor: int | None = None
    temporary_identity: tuple[int, int] | None = None
    publication_created = False
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
        temporary_status = os.fstat(temporary_descriptor)
        temporary_identity = (temporary_status.st_dev, temporary_status.st_ino)
        if not stat.S_ISREG(temporary_status.st_mode):
            raise ResourceError(f"resource temporary file is not regular: {output_path}")
        os.link(
            temporary_name,
            output_path.name,
            src_dir_fd=parent_descriptor,
            dst_dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        publication_created = True
        try:
            destination_descriptor = os.open(
                output_path.name,
                os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC,
                dir_fd=parent_descriptor,
            )
        except OSError as error:
            raise ResourceError(
                f"resource temporary identity changed during publication: {output_path}"
            ) from error
        destination_status = os.fstat(destination_descriptor)
        if (
            not stat.S_ISREG(destination_status.st_mode)
            or (destination_status.st_dev, destination_status.st_ino)
            != temporary_identity
        ):
            raise ResourceError(
                f"resource temporary identity changed during publication: {output_path}"
            )
        os.fsync(parent_descriptor)
    except BaseException:
        if publication_created:
            try:
                os.unlink(output_path.name, dir_fd=parent_descriptor)
            except FileNotFoundError:
                pass
            else:
                os.fsync(parent_descriptor)
        raise
    finally:
        if destination_descriptor is not None:
            os.close(destination_descriptor)
        if temporary_descriptor is not None:
            os.close(temporary_descriptor)
        try:
            current_temporary = os.stat(
                temporary_name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            )
            if temporary_identity is not None and (
                current_temporary.st_dev,
                current_temporary.st_ino,
            ) == temporary_identity:
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
