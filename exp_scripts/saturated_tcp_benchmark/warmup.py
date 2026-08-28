"""Fixed single-STA traffic warm-up sweep and combined CSV publication."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, replace
from datetime import datetime
import os
from pathlib import Path
import tempfile
import sys
from typing import Callable, Iterable, Sequence, TextIO

from .csv_output import CSV_HEADER
from .matrix import ExperimentConfiguration, build_matrix


WARMUP_SECONDS = (0, 1, 5, 10)
DEFAULT_CONFIG_RELATIVE = Path("contrib/llm/config/saturated_tcp_config.toml")


@dataclass(frozen=True)
class WarmupSweepResult:
    """Retained raw run directories and the combined diagnostic CSV."""

    run_directories: tuple[Path, ...]
    csv_path: Path


def build_warmup_sweep() -> tuple[ExperimentConfiguration, ...]:
    """Return 18 single-STA configurations for each fixed warm-up period."""
    baselines = build_matrix()[:18]
    return tuple(
        replace(configuration, traffic_warmup_seconds=warmup_seconds)
        for warmup_seconds in WARMUP_SECONDS
        for configuration in baselines
    )


def _read_baseline_csv(path: Path) -> list[list[str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as input_file:
        rows = list(csv.reader(input_file, delimiter=";"))
    if not rows or tuple(rows[0]) != CSV_HEADER:
        raise ValueError(f"unexpected saturated benchmark CSV header: {path}")
    data_rows = rows[1:]
    if len(data_rows) != 54:
        raise ValueError(f"expected 54 baseline BSS rows in {path}, found {len(data_rows)}")
    expected_identities = [
        (str(experiment_id), "1", "1", str(bss_id))
        for experiment_id in range(1, 19)
        for bss_id in range(3)
    ]
    for row, expected in zip(data_rows, expected_identities, strict=True):
        if len(row) != len(CSV_HEADER):
            raise ValueError(f"unexpected saturated benchmark CSV row width in {path}")
        identity = (row[0], row[1], row[2], row[8])
        if identity != expected:
            raise ValueError(f"unexpected baseline row identity in {path}: {identity!r}")
    return data_rows


def combine_warmup_csvs(
    inputs: Iterable[tuple[int, Path]], destination: str | Path
) -> Path:
    """Combine four canonical baseline CSVs with an explicit warm-up identity."""
    sources = tuple(inputs)
    if tuple(warmup for warmup, _ in sources) != WARMUP_SECONDS:
        raise ValueError(f"warm-up CSV inputs must follow {WARMUP_SECONDS}")
    combined_rows = []
    for warmup_seconds, source in sources:
        if type(warmup_seconds) is not int:
            raise ValueError("warm-up seconds must be integers")
        for row in _read_baseline_csv(Path(source)):
            combined = list(row)
            combined.insert(2, str(warmup_seconds))
            combined_rows.append(combined)

    output_path = Path(destination)
    header = list(CSV_HEADER)
    header.insert(2, "traffic_warmup_seconds")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output_path.name}.",
        suffix=".tmp",
        dir=output_path.parent,
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8-sig", newline="") as output_file:
            writer = csv.writer(output_file, delimiter=";", lineterminator="\r\n")
            writer.writerow(header)
            writer.writerows(combined_rows)
            output_file.flush()
            os.fsync(output_file.fileno())
        os.link(temporary_path, output_path)
        temporary_path.unlink()
        directory_descriptor = os.open(
            output_path.parent,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0),
        )
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise
    return output_path


def run_warmup_sweep(
    *,
    ns3_root: str | Path,
    config_path: str | Path,
    output_path: str | Path,
    timestamp: str | None = None,
    jobs: int = 0,
    memory_reserve_percent: int = 20,
    benchmark_runner: Callable[..., Path] | None = None,
    audit_runner: Callable[[Path], object] | None = None,
    output: TextIO | None = None,
) -> WarmupSweepResult:
    """Run four resource-aware baseline groups and combine their validated CSVs."""
    from .runner import load_runner_configuration

    loaded = load_runner_configuration(config_path)
    if loaded.repetitions != 1:
        raise ValueError("baseline warm-up sweep requires exactly one repetition")
    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise FileExistsError(output_path)
    if not output_path.parent.is_dir():
        raise FileNotFoundError(output_path.parent)
    if benchmark_runner is None:
        from .runner import run_benchmark

        benchmark_runner = run_benchmark
    if audit_runner is None:
        from .audit import audit_run_directory

        audit_runner = audit_run_directory
    timestamp = timestamp or datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    sweep = build_warmup_sweep()
    run_directories = []
    sources = []
    for group_index, warmup_seconds in enumerate(WARMUP_SECONDS):
        start = group_index * 18
        configurations = sweep[start : start + 18]
        run_directory = benchmark_runner(
            ns3_root=ns3_root,
            config_path=config_path,
            timestamp=f"baseline_warmup_{timestamp}_w{warmup_seconds}",
            configurations=configurations,
            jobs=jobs,
            memory_reserve_percent=memory_reserve_percent,
            output=output,
        )
        run_directory = Path(run_directory)
        audit_report = audit_runner(run_directory)
        if not getattr(audit_report, "ok", False):
            discrepancies = getattr(audit_report, "discrepancies", ())
            raise ValueError(
                f"independent audit failed for {run_directory}: {tuple(discrepancies)!r}"
            )
        observed_warmup_seconds = getattr(
            audit_report,
            "traffic_warmup_seconds",
            None,
        )
        if observed_warmup_seconds != warmup_seconds:
            raise ValueError(
                "independent audit warm-up mismatch: "
                f"expected {warmup_seconds}, observed {observed_warmup_seconds}"
            )
        run_directories.append(run_directory)
        sources.append((warmup_seconds, run_directory / "results.csv"))
    csv_path = combine_warmup_csvs(sources, output_path)
    return WarmupSweepResult(tuple(run_directories), csv_path)


def main(
    argv: Sequence[str] | None = None,
    *,
    output: TextIO | None = None,
    error: TextIO | None = None,
) -> int:
    """Run the fixed baseline warm-up sweep from the command line."""
    from .runner import RunnerError, discover_ns3_root

    output = output if output is not None else sys.stdout
    error = error if error is not None else sys.stderr
    parser = argparse.ArgumentParser(
        description="Run 18 single-STA baselines at 0, 1, 5, and 10 second warm-ups."
    )
    parser.add_argument("--ns3-root", type=Path, default=None)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_RELATIVE)
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="exclusive combined CSV path (default: timestamped file under run/)",
    )
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--memory-reserve-percent", type=int, default=20)
    arguments = parser.parse_args(argv)
    root = arguments.ns3_root or discover_ns3_root()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    config_path = arguments.config
    if not config_path.is_absolute():
        config_path = root / config_path
    output_path = arguments.output
    if output_path is None:
        default_output_parent = root / "run"
        try:
            default_output_parent.mkdir(exist_ok=True)
        except OSError as failure:
            print(f"error: baseline warm-up sweep failed: {failure}", file=error)
            return 1
        output_path = default_output_parent / f"baseline_warmup_results_{timestamp}.csv"
    elif not output_path.is_absolute():
        output_path = root / output_path
    try:
        result = run_warmup_sweep(
            ns3_root=root,
            config_path=config_path,
            output_path=output_path,
            timestamp=timestamp,
            jobs=arguments.jobs,
            memory_reserve_percent=arguments.memory_reserve_percent,
            output=output,
        )
    except KeyboardInterrupt:
        print("error: baseline warm-up sweep interrupted; completed raw runs were retained", file=error)
        return 130
    except (FileExistsError, OSError, RunnerError, ValueError) as failure:
        print(f"error: baseline warm-up sweep failed: {failure}", file=error)
        return 1
    for run_directory in result.run_directories:
        print(f"Retained raw run: {run_directory}", file=output)
    print(f"Combined warm-up CSV: {result.csv_path}", file=output)
    return 0
