"""Fixed-width Excel-compatible CSV output for saturated benchmark rows."""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping
import csv
from dataclasses import dataclass, replace
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
    "bss_mean_dominant_data_phy_rate_mbps",
    "bss_mean_effective_phy_rate_mbps",
    "bss_aggregate_data_tx_rate_over_interval_mbps",
    "bss_competition_overhead_vs_single_sta",
)


def _build_header() -> tuple[str, ...]:
    columns = list(_IDENTITY_COLUMNS + _BSS_COLUMNS)
    for station_index in range(30):
        columns.extend(
            (
                f"sta_{station_index}_dominant_data_phy_rate_mbps",
                f"sta_{station_index}_dominant_data_profile_share",
                f"sta_{station_index}_effective_phy_rate_mbps",
                f"sta_{station_index}_data_tx_rate_over_interval_mbps",
                f"sta_{station_index}_data_tx_opportunity_gap_fraction",
                f"sta_{station_index}_tx_profile",
            )
        )
    return tuple(columns)


CSV_HEADER = _build_header()


def _exclusive_nofollow_opener(path: str, flags: int) -> int:
    return os.open(path, flags | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)


@dataclass(frozen=True)
class StationCsvMetrics:
    """Six fixed CSV fields for one existing station."""

    dominant_data_phy_rate_mbps: float | None
    dominant_data_profile_share: float | None
    effective_phy_rate_mbps: float | None
    data_tx_rate_over_interval_mbps: float
    data_tx_opportunity_gap_fraction: float | None
    tx_profile: str


@dataclass(frozen=True)
class BssCsvRow:
    """One nullable-rate BSS result with fixed station columns through index 29."""

    configuration: ExperimentConfiguration
    repetition_attempt: int
    target_rssi_dbm: float
    bss_id: int
    mean_dominant_data_phy_rate_mbps: float | None
    mean_effective_phy_rate_mbps: float | None
    aggregate_data_tx_rate_over_interval_mbps: float
    competition_overhead_vs_single_sta: float | None
    stations: tuple[StationCsvMetrics | None, ...]


BaselineKey = tuple[str, str, str, str, int, int]


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


def _validate_metric(metric: StationCsvMetrics, name: str) -> None:
    if not isinstance(metric, StationCsvMetrics):
        raise ValueError(f"{name} must contain StationCsvMetrics")
    if not isinstance(metric.tx_profile, str):
        raise ValueError(f"{name} profile text must be a string")
    if metric.tx_profile:
        for value, field in (
            (metric.dominant_data_phy_rate_mbps, "dominant rate"),
            (metric.dominant_data_profile_share, "dominant share"),
            (metric.effective_phy_rate_mbps, "effective rate"),
            (metric.data_tx_rate_over_interval_mbps, "interval rate"),
            (metric.data_tx_opportunity_gap_fraction, "opportunity gap"),
        ):
            _require_finite(value, f"{name} {field}")
        if metric.dominant_data_phy_rate_mbps <= 0.0:
            raise ValueError(f"{name} dominant rate must be positive")
        if not 0.0 < metric.dominant_data_profile_share <= 1.0:
            raise ValueError(f"{name} dominant share must be in (0, 1]")
        if metric.effective_phy_rate_mbps <= 0.0:
            raise ValueError(f"{name} effective rate must be positive")
        if metric.data_tx_rate_over_interval_mbps <= 0.0:
            raise ValueError(f"{name} interval rate must be positive")
        if not 0.0 <= metric.data_tx_opportunity_gap_fraction <= 1.0:
            raise ValueError(f"{name} opportunity gap must be in [0, 1]")
    elif (
        metric.dominant_data_phy_rate_mbps is not None
        or metric.dominant_data_profile_share is not None
        or metric.effective_phy_rate_mbps is not None
        or metric.data_tx_rate_over_interval_mbps != 0.0
        or metric.data_tx_opportunity_gap_fraction is not None
    ):
        raise ValueError(f"{name} inactive profile must use null/null/null/zero/null")


def _validate_bss_metric(row: BssCsvRow) -> None:
    for value, field in (
        (row.mean_dominant_data_phy_rate_mbps, "dominant mean"),
        (row.mean_effective_phy_rate_mbps, "effective mean"),
        (row.aggregate_data_tx_rate_over_interval_mbps, "aggregate interval rate"),
        (row.competition_overhead_vs_single_sta, "competition overhead"),
    ):
        if value is not None:
            _require_finite(value, f"BSS {field}")
    if row.aggregate_data_tx_rate_over_interval_mbps < 0.0:
        raise ValueError("BSS aggregate interval rate must be non-negative")
    means = (
        row.mean_dominant_data_phy_rate_mbps,
        row.mean_effective_phy_rate_mbps,
    )
    if row.aggregate_data_tx_rate_over_interval_mbps == 0.0:
        if means != (None, None):
            raise ValueError("idle BSS must have empty means")
    elif any(value is None or value <= 0.0 for value in means):
        raise ValueError("active BSS must have positive means")


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
        _validate_bss_metric(row)
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
        _empty_if_none(row.mean_dominant_data_phy_rate_mbps),
        _empty_if_none(row.mean_effective_phy_rate_mbps),
        row.aggregate_data_tx_rate_over_interval_mbps,
        _empty_if_none(row.competition_overhead_vs_single_sta),
    ]
    for metric in row.stations:
        if metric is None:
            values.extend(("", "", "", "", "", ""))
        else:
            values.extend(
                (
                    _empty_if_none(metric.dominant_data_phy_rate_mbps),
                    _empty_if_none(metric.dominant_data_profile_share),
                    _empty_if_none(metric.effective_phy_rate_mbps),
                    metric.data_tx_rate_over_interval_mbps,
                    _empty_if_none(metric.data_tx_opportunity_gap_fraction),
                    metric.tx_profile,
                )
            )
    return values


def _baseline_key(row: BssCsvRow) -> BaselineKey:
    configuration = row.configuration
    return (
        configuration.rssi_range,
        configuration.interference_mode,
        configuration.traffic_mode,
        configuration.mimo_mode,
        row.repetition_attempt,
        row.bss_id,
    )


def apply_matching_baseline(
    rows: tuple[BssCsvRow, ...],
    baselines: Mapping[BaselineKey, float],
) -> tuple[BssCsvRow, ...]:
    """Return rows with signed competition overhead from matching single-STA runs."""
    if not isinstance(rows, tuple) or len(rows) != 3:
        raise ValueError("baseline application requires exactly three BSS rows")
    _validate_attempt_rows(rows)
    applied = []
    for row in rows:
        key = _baseline_key(row)
        try:
            baseline = baselines[key]
        except KeyError as error:
            raise ValueError(f"missing matching baseline for {key!r}") from error
        _require_finite(baseline, f"baseline {key!r}")
        if baseline < 0.0:
            raise ValueError(f"baseline {key!r} must be non-negative")
        current = row.aggregate_data_tx_rate_over_interval_mbps
        _require_finite(current, "BSS aggregate interval rate")
        if row.configuration.sta_count_per_bss == 1:
            scale = max(1.0, abs(current), abs(baseline))
            if abs(current - baseline) > 1e-9 * scale:
                raise ValueError(f"single-STA baseline mismatch for {key!r}")
            overhead = None if baseline == 0.0 else 0.0
        else:
            overhead = None if baseline == 0.0 else 1.0 - current / baseline
        applied.append(replace(row, competition_overhead_vs_single_sta=overhead))
    return tuple(applied)


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
