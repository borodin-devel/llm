"""Fixed saturated TCP benchmark matrix and repetition attempts."""

from __future__ import annotations

from collections.abc import Iterable, Iterator
from dataclasses import dataclass


STA_COUNTS = (1, 5, 10, 15, 20, 25, 30)
RSSI_RANGES = ("high", "medium", "low")
INTERFERENCE_MODES = ("isolated", "ap_only_cochannel")
TRAFFIC_MODES = ("ul", "dl", "ul_dl")
MIMO_MODES = ("su",)

_TARGET_RSSI_DBM = {"high": -41.5, "medium": -50.0, "low": -60.0}


@dataclass(frozen=True)
class ExperimentConfiguration:
    """One stable matrix coordinate shared across attempts and BSS rows."""

    experiment_id: int
    sta_count_per_bss: int
    rssi_range: str
    interference_mode: str
    traffic_mode: str
    mimo_mode: str


@dataclass(frozen=True)
class ExperimentAttempt:
    """One subprocess attempt for a matrix configuration."""

    configuration: ExperimentConfiguration
    repetition_attempt: int
    rng_run: int


def target_rssi_dbm(rssi_range: str) -> float:
    """Return the exact station target for a configured RSSI range."""
    try:
        return _TARGET_RSSI_DBM[rssi_range]
    except KeyError as error:
        raise ValueError(f"invalid benchmark RSSI range: {rssi_range!r}") from error


def build_matrix(*, mimo_modes: Iterable[str] = MIMO_MODES) -> tuple[ExperimentConfiguration, ...]:
    """Build the exact ordered SU-only 126-configuration product."""
    requested_mimo_modes = tuple(mimo_modes)
    if requested_mimo_modes != MIMO_MODES:
        requested = ", ".join(requested_mimo_modes) if requested_mimo_modes else "<empty>"
        raise ValueError(f"unsupported benchmark MIMO mode list {requested}: only su is supported")

    configurations = []
    experiment_id = 1
    for sta_count_per_bss in STA_COUNTS:
        for rssi_range in RSSI_RANGES:
            for interference_mode in INTERFERENCE_MODES:
                for traffic_mode in TRAFFIC_MODES:
                    for mimo_mode in requested_mimo_modes:
                        configurations.append(
                            ExperimentConfiguration(
                                experiment_id=experiment_id,
                                sta_count_per_bss=sta_count_per_bss,
                                rssi_range=rssi_range,
                                interference_mode=interference_mode,
                                traffic_mode=traffic_mode,
                                mimo_mode=mimo_mode,
                            )
                        )
                        experiment_id += 1
    return tuple(configurations)


def expand_experiment_ids(
    requested_experiment_ids: Iterable[int],
) -> tuple[tuple[int, ...], tuple[int, ...], tuple[int, ...]]:
    """Add matching one-STA dependencies and return requested/executed/automatic IDs."""
    requested = tuple(requested_experiment_ids)
    matrix = build_matrix()
    by_id = {configuration.experiment_id: configuration for configuration in matrix}
    if not requested:
        raise ValueError("requested experiment IDs must not be empty")
    if len(set(requested)) != len(requested):
        raise ValueError("requested experiment IDs must be unique")
    for experiment_id in requested:
        if type(experiment_id) is not int or experiment_id not in by_id:
            raise ValueError(f"unknown experiment ID: {experiment_id!r}")

    baseline_by_coordinate = {
        (
            configuration.rssi_range,
            configuration.interference_mode,
            configuration.traffic_mode,
            configuration.mimo_mode,
        ): configuration.experiment_id
        for configuration in matrix
        if configuration.sta_count_per_bss == 1
    }
    automatic = set()
    for experiment_id in requested:
        configuration = by_id[experiment_id]
        if configuration.sta_count_per_bss == 1:
            continue
        baseline_id = baseline_by_coordinate[
            (
                configuration.rssi_range,
                configuration.interference_mode,
                configuration.traffic_mode,
                configuration.mimo_mode,
            )
        ]
        if baseline_id not in requested:
            automatic.add(baseline_id)
    executed = tuple(sorted(set(requested) | automatic))
    return requested, executed, tuple(sorted(automatic))


def iter_experiment_attempts(
    configurations: Iterable[ExperimentConfiguration], repetitions: int = 1
) -> Iterator[ExperimentAttempt]:
    """Yield attempts in configuration-major order with RNG run equal to attempt."""
    if isinstance(repetitions, bool) or not isinstance(repetitions, int) or repetitions <= 0:
        raise ValueError("script.repetitions must be a positive integer")
    for configuration in configurations:
        if configuration.mimo_mode != "su":
            raise ValueError(
                f"unsupported benchmark MIMO mode {configuration.mimo_mode!r}: only su is supported"
            )
        for repetition_attempt in range(1, repetitions + 1):
            yield ExperimentAttempt(
                configuration=configuration,
                repetition_attempt=repetition_attempt,
                rng_run=repetition_attempt,
            )
