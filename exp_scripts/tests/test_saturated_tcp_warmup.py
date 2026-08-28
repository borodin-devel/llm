"""Baseline traffic warm-up sweep and combined CSV tests."""

from __future__ import annotations

import csv
import importlib
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest import mock

from saturated_tcp_benchmark.csv_output import CSV_HEADER


def _write_source_csv(path: Path) -> None:
    """Write one valid-shape 18-experiment baseline CSV fixture."""
    with path.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.writer(output, delimiter=";", lineterminator="\r\n")
        writer.writerow(CSV_HEADER)
        for experiment_id in range(1, 19):
            for bss_id in range(3):
                row = [""] * len(CSV_HEADER)
                row[0] = str(experiment_id)
                row[1] = "1"
                row[2] = "1"
                row[8] = str(bss_id)
                writer.writerow(row)


class SaturatedTcpWarmupTest(unittest.TestCase):
    """Protect the fixed sweep and its standalone Excel CSV."""

    def setUp(self) -> None:
        try:
            self.module = importlib.import_module("saturated_tcp_benchmark.warmup")
        except ModuleNotFoundError:
            self.module = None

    def test_fixed_sweep_is_warmup_major_and_contains_only_18_baselines(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        configurations = self.module.build_warmup_sweep()

        self.assertEqual(self.module.WARMUP_SECONDS, (0, 1, 5, 10))
        self.assertEqual(len(configurations), 72)
        self.assertEqual(
            [configuration.traffic_warmup_seconds for configuration in configurations],
            [0] * 18 + [1] * 18 + [5] * 18 + [10] * 18,
        )
        for offset in range(0, 72, 18):
            group = configurations[offset : offset + 18]
            self.assertEqual([item.experiment_id for item in group], list(range(1, 19)))
            self.assertEqual({item.sta_count_per_bss for item in group}, {1})
            self.assertEqual({item.mimo_mode for item in group}, {"su"})

    def test_combiner_adds_warmup_identity_and_preserves_excel_transport(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = []
            for warmup_seconds in (0, 1, 5, 10):
                source = root / f"w{warmup_seconds}.csv"
                _write_source_csv(source)
                inputs.append((warmup_seconds, source))
            destination = root / "combined.csv"

            self.module.combine_warmup_csvs(inputs, destination)

            raw = destination.read_bytes()
            self.assertTrue(raw.startswith(b"\xef\xbb\xbf"))
            payload = raw[3:]
            self.assertTrue(payload.endswith(b"\r\n"))
            self.assertNotIn(b"\n", payload.replace(b"\r\n", b""))
            rows = list(
                csv.reader(
                    StringIO(payload.decode("utf-8"), newline=""),
                    delimiter=";",
                )
            )
            expected_header = list(CSV_HEADER)
            expected_header.insert(2, "traffic_warmup_seconds")
            self.assertEqual(rows[0], expected_header)
            self.assertEqual(len(rows), 217)
            self.assertEqual({len(row) for row in rows}, {194})
            self.assertEqual([rows[index][2] for index in (1, 55, 109, 163)], ["0", "1", "5", "10"])

            with self.assertRaises(FileExistsError):
                self.module.combine_warmup_csvs(inputs, destination)

    def test_runner_executes_four_18_baseline_groups_and_combines_them(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        run_sweep = getattr(self.module, "run_warmup_sweep", None)
        self.assertIsNotNone(run_sweep, "baseline warm-up runner is missing")
        if run_sweep is None:
            return

        with TemporaryDirectory() as directory:
            root = Path(directory)
            config_path = root / "config.toml"
            config_path.write_text("[script]\nrepetitions = 1\n", encoding="utf-8")
            calls = []
            audited = []

            def fake_benchmark_runner(**kwargs):
                configurations = tuple(kwargs["configurations"])
                warmups = {item.traffic_warmup_seconds for item in configurations}
                self.assertEqual(len(configurations), 18)
                self.assertEqual([item.experiment_id for item in configurations], list(range(1, 19)))
                self.assertEqual(len(warmups), 1)
                warmup_seconds = warmups.pop()
                run_directory = root / "run" / kwargs["timestamp"]
                run_directory.mkdir(parents=True)
                _write_source_csv(run_directory / "results.csv")
                calls.append((warmup_seconds, kwargs["timestamp"], kwargs["jobs"]))
                return run_directory

            def fake_auditor(run_directory):
                audited.append(run_directory)
                warmup_seconds = int(run_directory.name.rsplit("w", 1)[1])
                return SimpleNamespace(
                    ok=True,
                    discrepancies=(),
                    traffic_warmup_seconds=warmup_seconds,
                )

            output_path = root / "traces" / "baseline_warmup_results.csv"
            output_path.parent.mkdir()
            result = run_sweep(
                ns3_root=root,
                config_path=config_path,
                output_path=output_path,
                timestamp="20260828_120000_000000",
                jobs=3,
                benchmark_runner=fake_benchmark_runner,
                audit_runner=fake_auditor,
            )

            self.assertEqual(
                calls,
                [
                    (0, "baseline_warmup_20260828_120000_000000_w0", 3),
                    (1, "baseline_warmup_20260828_120000_000000_w1", 3),
                    (5, "baseline_warmup_20260828_120000_000000_w5", 3),
                    (10, "baseline_warmup_20260828_120000_000000_w10", 3),
                ],
            )
            self.assertEqual(result.csv_path, output_path)
            self.assertEqual(len(result.run_directories), 4)
            self.assertEqual(audited, list(result.run_directories))
            self.assertTrue(output_path.is_file())

    def test_runner_rejects_an_audited_warmup_identity_mismatch(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            config_path = root / "config.toml"
            config_path.write_text("[script]\nrepetitions = 1\n", encoding="utf-8")

            def fake_benchmark_runner(**kwargs):
                run_directory = root / "run" / kwargs["timestamp"]
                run_directory.mkdir(parents=True)
                _write_source_csv(run_directory / "results.csv")
                return run_directory

            def mismatched_auditor(run_directory):
                return SimpleNamespace(
                    ok=True,
                    discrepancies=(),
                    traffic_warmup_seconds=99,
                )

            output_path = root / "combined.csv"
            with self.assertRaisesRegex(ValueError, "warm-up.*expected 0.*observed 99"):
                self.module.run_warmup_sweep(
                    ns3_root=root,
                    config_path=config_path,
                    output_path=output_path,
                    benchmark_runner=fake_benchmark_runner,
                    audit_runner=mismatched_auditor,
                )
            self.assertFalse(output_path.exists())

    def test_combiner_removes_temporary_file_when_atomic_publish_fails(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = []
            for warmup_seconds in (0, 1, 5, 10):
                source = root / f"w{warmup_seconds}.csv"
                _write_source_csv(source)
                inputs.append((warmup_seconds, source))
            destination = root / "combined.csv"

            with mock.patch.object(
                self.module.os,
                "link",
                side_effect=OSError("injected atomic publication failure"),
            ):
                with self.assertRaisesRegex(OSError, "injected atomic"):
                    self.module.combine_warmup_csvs(inputs, destination)

            self.assertFalse(destination.exists())
            self.assertEqual(
                {path.name for path in root.iterdir()},
                {"w0.csv", "w1.csv", "w5.csv", "w10.csv"},
            )

    def test_preflight_rejects_repetitions_output_collision_and_missing_parent(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            calls = []

            def forbidden_runner(**kwargs):
                calls.append(kwargs)
                self.fail("preflight launched a benchmark process")

            output_path = root / "results.csv"
            repetitions_path = root / "repetitions.toml"
            repetitions_path.write_text("[script]\nrepetitions = 2\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "exactly one repetition"):
                self.module.run_warmup_sweep(
                    ns3_root=root,
                    config_path=repetitions_path,
                    output_path=output_path,
                    benchmark_runner=forbidden_runner,
                )

            single_path = root / "single.toml"
            single_path.write_text("[script]\nrepetitions = 1\n", encoding="utf-8")
            output_path.write_text("existing", encoding="ascii")
            with self.assertRaises(FileExistsError):
                self.module.run_warmup_sweep(
                    ns3_root=root,
                    config_path=single_path,
                    output_path=output_path,
                    benchmark_runner=forbidden_runner,
                )

            with self.assertRaises(FileNotFoundError):
                self.module.run_warmup_sweep(
                    ns3_root=root,
                    config_path=single_path,
                    output_path=root / "missing" / "results.csv",
                    benchmark_runner=forbidden_runner,
                )
            self.assertEqual(calls, [])

    def test_cli_creates_the_timestamped_default_output_parent(self) -> None:
        self.assertIsNotNone(self.module, "baseline warm-up module is missing")
        with TemporaryDirectory() as directory:
            root = Path(directory)
            config_path = root / "config.toml"
            config_path.write_text("[script]\nrepetitions = 1\n", encoding="utf-8")
            captured = []

            def fake_sweep(**kwargs):
                output_path = Path(kwargs["output_path"])
                if not output_path.parent.is_dir():
                    raise FileNotFoundError(output_path.parent)
                captured.append(output_path)
                return self.module.WarmupSweepResult((), output_path)

            output = StringIO()
            error = StringIO()
            with mock.patch.object(self.module, "run_warmup_sweep", side_effect=fake_sweep):
                status = self.module.main(
                    ["--ns3-root", str(root), "--config", str(config_path)],
                    output=output,
                    error=error,
                )

            self.assertEqual(status, 0, error.getvalue())
            self.assertEqual(len(captured), 1)
            self.assertEqual(captured[0].parent, root / "run")
            self.assertRegex(captured[0].name, r"^baseline_warmup_results_.*\.csv$")


if __name__ == "__main__":
    unittest.main()
