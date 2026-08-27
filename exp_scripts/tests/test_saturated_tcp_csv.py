"""Excel-compatible schema-v2 saturated benchmark CSV tests."""

from __future__ import annotations

import codecs
import csv
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from saturated_tcp_benchmark.csv_output import (
    BssCsvRow,
    CSV_HEADER,
    ExcelCsvWriter,
    StationCsvMetrics,
    apply_matching_baseline,
)
from saturated_tcp_benchmark.matrix import ExperimentConfiguration


IDENTITY_COLUMNS = (
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
BSS_COLUMNS = (
    "bss_mean_dominant_data_phy_rate_mbps",
    "bss_mean_effective_phy_rate_mbps",
    "bss_aggregate_data_tx_rate_over_interval_mbps",
    "bss_competition_overhead_vs_single_sta",
)
STATION_SUFFIXES = (
    "dominant_data_phy_rate_mbps",
    "dominant_data_profile_share",
    "effective_phy_rate_mbps",
    "data_tx_rate_over_interval_mbps",
    "data_tx_opportunity_gap_fraction",
    "tx_profile",
)


def approved_header() -> tuple[str, ...]:
    """Return the independently specified public column sequence."""
    return IDENTITY_COLUMNS + BSS_COLUMNS + tuple(
        f"sta_{station_index}_{suffix}"
        for station_index in range(30)
        for suffix in STATION_SUFFIXES
    )


def make_row(
    bss_id: int,
    *,
    station_count: int = 5,
    aggregate: float = 8.0,
    special_text: bool = False,
) -> BssCsvRow:
    """Return one direct-run row whose cross-run field is deliberately empty."""
    configuration = ExperimentConfiguration(
        experiment_id=20 if station_count == 5 else 2,
        sta_count_per_bss=station_count,
        rssi_range="high;range" if special_text else "high",
        interference_mode="isolated",
        traffic_mode='ul"dl' if special_text else "ul",
        mimo_mode="su",
    )
    stations = tuple(
        StationCsvMetrics(
            dominant_data_phy_rate_mbps=97.5 + station_index,
            dominant_data_profile_share=0.75,
            effective_phy_rate_mbps=20.0 + station_index,
            data_tx_rate_over_interval_mbps=0.32 + station_index / 100.0,
            data_tx_opportunity_gap_fraction=0.984,
            tx_profile=(
                "W20_NSS1_MCS9:bytes=100,ppdus=2,airtime_us=40|"
                "W80_NSS2_MCS11:bytes=300,ppdus=4,airtime_us=120"
            ),
        )
        if station_index < station_count
        else None
        for station_index in range(30)
    )
    return BssCsvRow(
        configuration=configuration,
        repetition_attempt=2,
        target_rssi_dbm=-41.5,
        bss_id=bss_id,
        mean_dominant_data_phy_rate_mbps=99.5,
        mean_effective_phy_rate_mbps=22.0,
        aggregate_data_tx_rate_over_interval_mbps=aggregate,
        competition_overhead_vs_single_sta=None,
        stations=stations,
    )


class SaturatedTcpCsvTest(unittest.TestCase):
    """Protect baseline math, fixed columns, bytes, and atomic publication."""

    def test_matching_baseline_is_attempt_and_bss_specific_and_signed(self) -> None:
        rows = tuple(
            make_row(bss_id, aggregate=aggregate)
            for bss_id, aggregate in enumerate((8.0, 12.0, 10.0))
        )
        baselines = {
            ("high", "isolated", "ul", "su", 2, 0): 10.0,
            ("high", "isolated", "ul", "su", 2, 1): 10.0,
            ("high", "isolated", "ul", "su", 2, 2): 10.0,
        }
        applied = apply_matching_baseline(rows, baselines)
        self.assertAlmostEqual(applied[0].competition_overhead_vs_single_sta, 0.2)
        self.assertAlmostEqual(applied[1].competition_overhead_vs_single_sta, -0.2)
        self.assertEqual(applied[2].competition_overhead_vs_single_sta, 0.0)
        self.assertTrue(all(row is not source for row, source in zip(applied, rows)))

        zero = dict(baselines)
        zero[("high", "isolated", "ul", "su", 2, 0)] = 0.0
        self.assertIsNone(
            apply_matching_baseline(rows, zero)[0].competition_overhead_vs_single_sta
        )

    def test_baseline_self_is_zero_and_missing_or_mismatched_values_are_rejected(self) -> None:
        rows = tuple(make_row(bss_id, station_count=1, aggregate=10.0) for bss_id in range(3))
        baselines = {
            ("high", "isolated", "ul", "su", 2, bss_id): 10.0
            for bss_id in range(3)
        }
        self.assertEqual(
            tuple(
                row.competition_overhead_vs_single_sta
                for row in apply_matching_baseline(rows, baselines)
            ),
            (0.0, 0.0, 0.0),
        )
        missing = dict(baselines)
        del missing[("high", "isolated", "ul", "su", 2, 1)]
        with self.assertRaisesRegex(ValueError, "missing.*baseline"):
            apply_matching_baseline(rows, missing)
        mismatched = dict(baselines)
        mismatched[("high", "isolated", "ul", "su", 2, 1)] = 9.0
        with self.assertRaisesRegex(ValueError, "mismatch"):
            apply_matching_baseline(rows, mismatched)
        mismatched_rows = (rows[0], replace(rows[1], repetition_attempt=3), rows[2])
        with self.assertRaisesRegex(ValueError, "share one repetition_attempt"):
            apply_matching_baseline(mismatched_rows, baselines)

    def test_header_and_station_block_boundaries_are_exact(self) -> None:
        expected = approved_header()
        self.assertEqual(CSV_HEADER, expected)
        self.assertEqual(len(CSV_HEADER), 193)
        self.assertEqual(CSV_HEADER[:13], IDENTITY_COLUMNS + BSS_COLUMNS)
        self.assertEqual(CSV_HEADER[13:19], tuple(f"sta_0_{x}" for x in STATION_SUFFIXES))
        self.assertEqual(CSV_HEADER[187:193], tuple(f"sta_29_{x}" for x in STATION_SUFFIXES))
        expected_bytes = codecs.BOM_UTF8 + (";".join(expected) + "\r\n").encode("utf-8")

        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path):
                pass
            self.assertEqual(output_path.read_bytes(), expected_bytes)

    def test_attempt_rows_use_bom_semicolon_crlf_decimal_dot_and_profile_text(self) -> None:
        fsync_calls: list[int] = []
        direct = tuple(make_row(bss_id, special_text=True) for bss_id in range(3))
        baselines = {
            ("high;range", "isolated", 'ul"dl', "su", 2, bss_id): 10.0
            for bss_id in range(3)
        }
        rows = apply_matching_baseline(direct, baselines)
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path, fsync=fsync_calls.append) as output:
                fsync_calls.clear()
                output.append_attempt(rows)
                self.assertEqual(len(fsync_calls), 1)
            data = output_path.read_bytes()
            with output_path.open("r", encoding="utf-8-sig", newline="") as input_file:
                parsed = list(csv.reader(input_file, delimiter=";"))

        self.assertTrue(data.startswith(codecs.BOM_UTF8))
        without_bom = data[len(codecs.BOM_UTF8) :]
        self.assertEqual(without_bom.count(b"\r\n"), 4)
        self.assertNotIn(b"\n", without_bom.replace(b"\r\n", b""))
        self.assertIn(b'"high;range"', without_bom)
        self.assertIn(b'"ul""dl"', without_bom)
        self.assertIn(b"97.5", without_bom)
        self.assertNotIn(b"97,5", without_bom)
        self.assertEqual(len(parsed), 4)
        self.assertTrue(all(len(row) == 193 for row in parsed))
        first = parsed[1]
        self.assertEqual(
            first[:9],
            ["20", "2", "5", "high;range", "-41.5", "isolated", 'ul"dl', "su", "0"],
        )
        self.assertEqual(first[9:13], ["99.5", "22.0", "8.0", "0.19999999999999996"])
        self.assertEqual(
            first[13:19],
            [
                "97.5",
                "0.75",
                "20.0",
                "0.32",
                "0.984",
                "W20_NSS1_MCS9:bytes=100,ppdus=2,airtime_us=40|W80_NSS2_MCS11:bytes=300,ppdus=4,airtime_us=120",
            ],
        )
        self.assertEqual(first[13 + 5 * 6 :], [""] * (25 * 6))

    def test_inactive_existing_station_keeps_only_numeric_interval_zero(self) -> None:
        inactive = StationCsvMetrics(None, None, None, 0.0, None, "")
        rows = [make_row(bss_id) for bss_id in range(3)]
        rows[0] = replace(rows[0], stations=(inactive,) + rows[0].stations[1:])
        rows[1] = replace(
            rows[1],
            mean_dominant_data_phy_rate_mbps=None,
            mean_effective_phy_rate_mbps=None,
            aggregate_data_tx_rate_over_interval_mbps=0.0,
            stations=(inactive,) * 5 + (None,) * 25,
        )
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path) as output:
                output.append_attempt(rows)
            with output_path.open("r", encoding="utf-8-sig", newline="") as input_file:
                parsed = list(csv.reader(input_file, delimiter=";"))

        self.assertEqual(parsed[1][13:19], ["", "", "", "0.0", "", ""])
        self.assertEqual(parsed[2][9:13], ["", "", "0.0", ""])
        self.assertEqual(parsed[2][13:19], ["", "", "", "0.0", "", ""])
        self.assertEqual(parsed[2][13 + 5 * 6 : 13 + 6 * 6], [""] * 6)

    def test_invalid_attempt_batch_adds_no_partial_rows(self) -> None:
        valid = tuple(make_row(bss_id) for bss_id in range(3))
        invalid = (valid[0], replace(valid[1], stations=valid[1].stations[:-1]), valid[2])
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path) as output:
                before = output_path.read_bytes()
                with self.assertRaisesRegex(ValueError, "30"):
                    output.append_attempt(invalid)
                self.assertEqual(output_path.read_bytes(), before)

                bad = replace(
                    valid[2],
                    stations=valid[2].stations[:29] + (valid[2].stations[0],),
                )
                with self.assertRaisesRegex(ValueError, "nonexistent station"):
                    output.append_attempt((valid[0], valid[1], bad))
                self.assertEqual(output_path.read_bytes(), before)

    def test_existing_csv_is_never_overwritten(self) -> None:
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            output_path.write_bytes(b"sentinel\r\n")
            with self.assertRaises(FileExistsError):
                ExcelCsvWriter(output_path)
            self.assertEqual(output_path.read_bytes(), b"sentinel\r\n")

    def test_csv_boundaries_are_frozen(self) -> None:
        metric = StationCsvMetrics(97.5, 0.75, 20.0, 0.32, 0.984, "profile")
        row = make_row(0)
        with self.assertRaises((AttributeError, TypeError)):
            metric.dominant_data_profile_share = 0.4  # type: ignore[misc]
        with self.assertRaises((AttributeError, TypeError)):
            row.bss_id = 1  # type: ignore[misc]


if __name__ == "__main__":
    unittest.main()
