"""Fixed-width Excel-compatible CSV output for saturated benchmark rows."""

from __future__ import annotations

from collections.abc import Callable, Iterable
import csv
from dataclasses import dataclass
from io import StringIO
import math
import os
from pathlib import Path
import stat
from typing import Any, TextIO

from .matrix import ExperimentConfiguration


_IDENTITY_COLUMNS = (
    "experiment_id",
    "repetition_attempt",
    "sta_count_per_bss",
    "rssi_range",
    "target_rssi_dbm",
    "interference_mode",
    "traffic_mode",
    "mimo_mode",
    "bss_id",
)
_BSS_COLUMNS = (
    "avg_all_sta_theoretical_phy_rate_mbps",
    "avg_all_sta_practical_phy_rate_mbps",
    "bss_channel_efficiency",
    "bss_channel_contention_fraction",
)


def _build_header() -> tuple[str, ...]:
    columns = list(_IDENTITY_COLUMNS + _BSS_COLUMNS)
    for station_index in range(30):
        columns.extend(
            (
                f"sta_{station_index}_avg_theoretical_phy_rate_mbps",
                f"sta_{station_index}_avg_practical_phy_rate_mbps",
                f"sta_{station_index}_efficiency",
                f"sta_{station_index}_contention_fraction",
            )
        )
    return tuple(columns)


CSV_HEADER = _build_header()


def _exclusive_nofollow_opener(path: str, flags: int) -> int:
    return os.open(path, flags | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)


@dataclass(frozen=True)
class StationCsvMetrics:
    """Nullable rate-derived fields and numeric contention for one existing station."""

    average_theoretical_phy_rate_mbps: float | None
    average_practical_phy_rate_mbps: float | None
    efficiency: float | None
    contention_fraction: float


@dataclass(frozen=True)
class BssCsvRow:
    """One nullable-rate BSS result with fixed station columns through index 29."""

    configuration: ExperimentConfiguration
    repetition_attempt: int
    target_rssi_dbm: float
    bss_id: int
    average_theoretical_phy_rate_mbps: float | None
    average_practical_phy_rate_mbps: float | None
    efficiency: float | None
    contention_fraction: float
    stations: tuple[StationCsvMetrics | None, ...]


def _writer(destination: TextIO) -> Any:
    return csv.writer(
        destination,
        delimiter=";",
        lineterminator="\r\n",
        quoting=csv.QUOTE_MINIMAL,
    )


def _empty_if_none(value: object) -> object:
    return "" if value is None else value


def _require_finite(value: object, name: str) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{name} must be a finite number")


def _validate_rate_triplet(
    theoretical: object,
    practical: object,
    efficiency: object,
    name: str,
) -> None:
    for value, field in (
        (theoretical, "theoretical rate"),
        (practical, "practical rate"),
        (efficiency, "efficiency"),
    ):
        if value is not None:
            _require_finite(value, f"{name} {field}")
    if (theoretical is None) != (practical is None):
        raise ValueError(f"{name} theoretical and practical rate presence differs")
    if theoretical is None:
        if efficiency is not None:
            raise ValueError(f"{name} efficiency exists without rates")
    elif theoretical == 0.0:
        if practical != 0.0 or efficiency is not None:
            raise ValueError(f"{name} zero theoretical rate has invalid efficiency")
    elif efficiency is None:
        raise ValueError(f"{name} efficiency is missing for defined rates")


def _validate_metric(metric: StationCsvMetrics, name: str) -> None:
    if not isinstance(metric, StationCsvMetrics):
        raise ValueError(f"{name} must contain StationCsvMetrics")
    _validate_rate_triplet(
        metric.average_theoretical_phy_rate_mbps,
        metric.average_practical_phy_rate_mbps,
        metric.efficiency,
        name,
    )
    _require_finite(metric.contention_fraction, f"{name} contention")


def _validate_attempt_rows(rows: tuple[BssCsvRow, ...]) -> None:
    if len(rows) != 3:
        raise ValueError("one attempt must contain exactly three BSS rows")
    if tuple(row.bss_id for row in rows) != (0, 1, 2):
        raise ValueError("attempt BSS rows must be ordered with IDs 0, 1, and 2")

    first = rows[0]
    if isinstance(first.repetition_attempt, bool) or first.repetition_attempt <= 0:
        raise ValueError("repetition_attempt must be a positive integer")
    station_count = first.configuration.sta_count_per_bss
    if isinstance(station_count, bool) or not isinstance(station_count, int):
        raise ValueError("sta_count_per_bss must be an integer")
    if not 1 <= station_count <= 30:
        raise ValueError("sta_count_per_bss must be in [1, 30]")

    for row in rows:
        if not isinstance(row, BssCsvRow):
            raise ValueError("attempt entries must be BssCsvRow values")
        if row.configuration != first.configuration:
            raise ValueError("attempt rows must share one configuration")
        if row.repetition_attempt != first.repetition_attempt:
            raise ValueError("attempt rows must share one repetition_attempt")
        if row.target_rssi_dbm != first.target_rssi_dbm:
            raise ValueError("attempt rows must share one target_rssi_dbm")
        if not isinstance(row.stations, tuple) or len(row.stations) != 30:
            raise ValueError("each BSS row must contain exactly 30 station entries")
        _require_finite(row.target_rssi_dbm, "target_rssi_dbm")
        _validate_rate_triplet(
            row.average_theoretical_phy_rate_mbps,
            row.average_practical_phy_rate_mbps,
            row.efficiency,
            "BSS",
        )
        _require_finite(row.contention_fraction, "BSS contention")
        for station_index, metric in enumerate(row.stations):
            if station_index < station_count:
                if metric is None:
                    raise ValueError(f"existing station {station_index} has empty CSV metrics")
                _validate_metric(metric, f"station {station_index}")
            elif metric is not None:
                raise ValueError(
                    f"nonexistent station {station_index} has nonempty CSV metrics"
                )


def _row_values(row: BssCsvRow) -> list[object]:
    configuration = row.configuration
    values: list[object] = [
        configuration.experiment_id,
        row.repetition_attempt,
        configuration.sta_count_per_bss,
        configuration.rssi_range,
        row.target_rssi_dbm,
        configuration.interference_mode,
        configuration.traffic_mode,
        configuration.mimo_mode,
        row.bss_id,
        _empty_if_none(row.average_theoretical_phy_rate_mbps),
        _empty_if_none(row.average_practical_phy_rate_mbps),
        _empty_if_none(row.efficiency),
        row.contention_fraction,
    ]
    for metric in row.stations:
        if metric is None:
            values.extend(("", "", "", ""))
        else:
            values.extend(
                (
                    _empty_if_none(metric.average_theoretical_phy_rate_mbps),
                    _empty_if_none(metric.average_practical_phy_rate_mbps),
                    _empty_if_none(metric.efficiency),
                    metric.contention_fraction,
                )
            )
    return values


class ExcelCsvWriter:
    """Exclusive CSV owner that publishes one complete attempt per flush."""

    def __init__(
        self,
        path: str | Path,
        *,
        fsync: Callable[[int], object] = os.fsync,
    ) -> None:
        self.path = Path(path)
        self._fsync = fsync
        self._file = open(
            self.path,
            "x",
            encoding="utf-8-sig",
            newline="",
            opener=_exclusive_nofollow_opener,
        )
        try:
            descriptor_status = os.fstat(self._file.fileno())
            path_status = self.path.lstat()
            if not stat.S_ISREG(descriptor_status.st_mode):
                raise OSError(f"CSV descriptor is not regular: {self.path}")
            if not stat.S_ISREG(path_status.st_mode):
                raise OSError(f"CSV path is not regular: {self.path}")
            self._identity = (descriptor_status.st_dev, descriptor_status.st_ino)
            if self._identity != (path_status.st_dev, path_status.st_ino):
                raise OSError(f"CSV descriptor/path identity mismatch: {self.path}")
            _writer(self._file).writerow(CSV_HEADER)
            self._synchronize()
        except BaseException:
            self._file.close()
            raise

    def __enter__(self) -> ExcelCsvWriter:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()

    def close(self) -> None:
        """Close the retained CSV without removing any published data."""
        if not self._file.closed:
            self._file.close()

    def fileno(self) -> int:
        """Return the live CSV descriptor for identity validation."""
        return self._file.fileno()

    def identity(self) -> tuple[int, int]:
        """Return the CSV identity pinned immediately after exclusive open."""
        return self._identity

    def append_attempt(self, rows: Iterable[BssCsvRow]) -> None:
        """Append exactly three validated rows using one prepared text write."""
        attempt_rows = tuple(rows)
        _validate_attempt_rows(attempt_rows)
        prepared = StringIO(newline="")
        _writer(prepared).writerows(_row_values(row) for row in attempt_rows)
        self._file.write(prepared.getvalue())
        self._synchronize()

    def _synchronize(self) -> None:
        self._file.flush()
        self._fsync(self._file.fileno())
