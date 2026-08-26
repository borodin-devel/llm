"""Excel-compatible saturated benchmark CSV tests."""

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
    "avg_all_sta_theoretical_phy_rate_mbps",
    "avg_all_sta_practical_phy_rate_mbps",
    "bss_channel_efficiency",
    "bss_channel_contention_fraction",
)


def approved_header() -> tuple[str, ...]:
    """Build the literal approved sequence independently of production code."""
    columns = list(IDENTITY_COLUMNS + BSS_COLUMNS)
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


def make_row(bss_id: int, *, special_text: bool = False) -> BssCsvRow:
    """Return one fixed-width five-station row."""
    configuration = ExperimentConfiguration(
        experiment_id=7,
        sta_count_per_bss=5,
        rssi_range="high;range" if special_text else "high",
        interference_mode="isolated",
        traffic_mode='ul"dl' if special_text else "ul",
        mimo_mode="su",
    )
    stations = tuple(
        StationCsvMetrics(
            average_theoretical_phy_rate_mbps=41.5 + station_index,
            average_practical_phy_rate_mbps=20.75 + station_index,
            efficiency=0.5,
            contention_fraction=0.1 + station_index / 100.0,
        )
        if station_index < 5
        else None
        for station_index in range(30)
    )
    return BssCsvRow(
        configuration=configuration,
        repetition_attempt=2,
        target_rssi_dbm=-41.5,
        bss_id=bss_id,
        average_theoretical_phy_rate_mbps=43.5,
        average_practical_phy_rate_mbps=22.75,
        efficiency=22.75 / 43.5,
        contention_fraction=0.12,
        stations=stations,
    )


class SaturatedTcpCsvTest(unittest.TestCase):
    """Protect fixed columns, bytes, batch publication, and no-clobber."""

    def test_header_is_byte_exact_and_has_no_unapproved_columns(self) -> None:
        expected = approved_header()
        self.assertEqual(CSV_HEADER, expected)
        self.assertEqual(len(CSV_HEADER), 133)
        expected_header_bytes = codecs.BOM_UTF8 + (";".join(expected) + "\r\n").encode("utf-8")

        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path):
                pass
            data = output_path.read_bytes()

        self.assertEqual(data, expected_header_bytes)
        lowered = ";".join(CSV_HEADER).lower()
        for forbidden in (
            "goodput",
            "tcp",
            "retransmission",
            "failure",
            "drop",
            "busy_time",
            "airtime",
        ):
            self.assertNotIn(forbidden, lowered)

    def test_attempt_rows_use_bom_semicolon_crlf_decimal_dot_and_minimal_quotes(self) -> None:
        fsync_calls: list[int] = []
        rows = tuple(make_row(bss_id, special_text=True) for bss_id in range(3))
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path, fsync=fsync_calls.append) as output:
                fsync_calls.clear()
                output.append_attempt(rows)
                self.assertEqual(len(fsync_calls), 1)
            data = output_path.read_bytes()
            with output_path.open("r", encoding="utf-8-sig", newline="") as input_file:
                parsed = list(csv.reader(input_file, delimiter=";", quoting=csv.QUOTE_MINIMAL))

        self.assertTrue(data.startswith(codecs.BOM_UTF8))
        without_bom = data[len(codecs.BOM_UTF8) :]
        self.assertEqual(without_bom.count(b"\r\n"), 4)
        self.assertNotIn(b"\n", without_bom.replace(b"\r\n", b""))
        self.assertIn(b'"high;range"', without_bom)
        self.assertIn(b'"ul""dl"', without_bom)
        self.assertIn(b"41.5", without_bom)
        self.assertNotIn(b"41,5", without_bom)
        self.assertEqual(parsed[0], list(approved_header()))
        self.assertEqual(len(parsed), 4)
        first_row = parsed[1]
        self.assertEqual(len(first_row), 133)
        self.assertEqual(
            first_row[:9],
            ["7", "2", "5", "high;range", "-41.5", "isolated", 'ul"dl', "su", "0"],
        )
        self.assertEqual(first_row[13:17], ["41.5", "20.75", "0.5", "0.1"])
        self.assertEqual(first_row[13 + 5 * 4 :], [""] * (25 * 4))

    def test_invalid_attempt_batch_adds_no_partial_rows(self) -> None:
        valid_rows = tuple(make_row(bss_id) for bss_id in range(3))
        invalid_rows = (
            valid_rows[0],
            replace(valid_rows[1], stations=valid_rows[1].stations[:-1]),
            valid_rows[2],
        )
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            with ExcelCsvWriter(output_path) as output:
                before = output_path.read_bytes()
                with self.assertRaisesRegex(ValueError, "30"):
                    output.append_attempt(invalid_rows)
                self.assertEqual(output_path.read_bytes(), before)

                inactive_station = StationCsvMetrics(1.0, 0.5, 0.5, 0.1)
                bad_columns = replace(
                    valid_rows[2],
                    stations=valid_rows[2].stations[:29] + (inactive_station,),
                )
                with self.assertRaisesRegex(ValueError, "nonexistent station"):
                    output.append_attempt((valid_rows[0], valid_rows[1], bad_columns))
                self.assertEqual(output_path.read_bytes(), before)

    def test_existing_csv_is_never_overwritten(self) -> None:
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "results.csv"
            output_path.write_bytes(b"sentinel\r\n")
            with self.assertRaises(FileExistsError):
                ExcelCsvWriter(output_path)
            self.assertEqual(output_path.read_bytes(), b"sentinel\r\n")

    def test_csv_boundaries_are_frozen(self) -> None:
        metric = StationCsvMetrics(41.5, 20.75, 0.5, 0.1)
        row = make_row(0)
        with self.assertRaises((AttributeError, TypeError)):
            metric.efficiency = 0.4  # type: ignore[misc]
        with self.assertRaises((AttributeError, TypeError)):
            row.bss_id = 1  # type: ignore[misc]


if __name__ == "__main__":
    unittest.main()
