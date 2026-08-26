"""Self-tests for root, scalar, configuration, window, and inventory validation."""

from __future__ import annotations

import copy
from pathlib import Path
import tempfile
import unittest

from live_verification.common import LiveTraceError
from live_verification.schema import load_output_document, validate_output_document
from tests.live_verification.fixtures import add_second_bss, entity, valid_document


class LiveTraceSchemaRootTest(unittest.TestCase):
    def setUp(self):
        self.trace = "contrib/llm/traces/1W_high_load_1s.json"
        self.source = Path("/tmp/fake/output.json")

    def assert_path_error(self, function, *args, text):
        with self.assertRaisesRegex(LiveTraceError, str(self.source)) as context:
            function(*args)
        self.assertIn(text, str(context.exception))

    def assert_document_error(self, document, json_path):
        self.assert_path_error(
            validate_output_document, document, self.source, self.trace, text=json_path
        )

    def test_validates_exact_document(self):
        metrics = validate_output_document(
            valid_document(self.trace), self.source, self.trace,
            Path("/tmp/llm-trace-live.test.random"),
        )
        self.assertEqual(metrics["window_count"], 1)
        self.assertEqual(metrics["ap_inventory_count"], 1)
        self.assertEqual(metrics["sta_inventory_count"], 1)

    def test_accepts_null_optional_averages(self):
        document = valid_document(self.trace)
        general = document["windows"][0]["access_points"][0]["general_stats"]["downlink"]
        self.assertIsNone(general["average_transmission_duration_us"])
        self.assertIsNone(general["effective_throughput_mbps"])
        distribution = general["application_to_phy_delay"]
        self.assertEqual(distribution["sample_count"], 0)
        self.assertIsNone(distribution["average_us"])
        validate_output_document(document, self.source, self.trace)

    def test_accepts_null_benchmark_phy_fields(self):
        document = valid_document(self.trace)
        phy = document["windows"][0]["access_points"][0]["phy_stats"]
        for field in (
            "average_theoretical_phy_rate_mbps",
            "average_practical_phy_rate_mbps",
            "channel_efficiency",
            "contention_fraction",
        ):
            phy[field] = None
        validate_output_document(document, self.source, self.trace)

    def test_rejects_missing_benchmark_phy_fields(self):
        for field in (
            "average_theoretical_phy_rate_mbps",
            "average_practical_phy_rate_mbps",
            "channel_efficiency",
            "contention_fraction",
        ):
            with self.subTest(field=field):
                document = valid_document(self.trace)
                del document["windows"][0]["access_points"][0]["phy_stats"][field]
                self.assert_document_error(document, ".phy_stats")

    def test_rejects_invalid_benchmark_phy_field_types(self):
        mutations = (
            ("average_theoretical_phy_rate_mbps", {}),
            ("average_practical_phy_rate_mbps", "720.6"),
            ("channel_efficiency", {}),
            ("contention_fraction", "0.2"),
        )
        for field, invalid in mutations:
            with self.subTest(field=field, invalid=invalid):
                document = valid_document(self.trace)
                document["windows"][0]["access_points"][0]["phy_stats"][field] = invalid
                self.assert_document_error(document, f".phy_stats.{field}")

    def test_rejects_nonfinite_benchmark_phy_fields(self):
        fields = (
            "average_theoretical_phy_rate_mbps",
            "average_practical_phy_rate_mbps",
            "channel_efficiency",
            "contention_fraction",
        )
        for field in fields:
            for invalid in (float("inf"), float("-inf"), float("nan")):
                with self.subTest(field=field, invalid=invalid):
                    document = valid_document(self.trace)
                    document["windows"][0]["access_points"][0]["phy_stats"][field] = invalid
                    self.assert_document_error(document, f".phy_stats.{field}")

    def test_rejects_out_of_range_benchmark_phy_fractions(self):
        for field in ("channel_efficiency", "contention_fraction"):
            for invalid in (-0.01, 1.01):
                with self.subTest(field=field, invalid=invalid):
                    document = valid_document(self.trace)
                    document["windows"][0]["access_points"][0]["phy_stats"][field] = invalid
                    self.assert_document_error(document, f".phy_stats.{field}")

    def test_accepts_sparse_full_windows_and_last_partial_window(self):
        document = valid_document(self.trace)
        full_sparse = copy.deepcopy(document["windows"][0])
        full_sparse["window_index"] = 2
        full_sparse["window_start_ms"] = 20.0
        last_partial = copy.deepcopy(document["windows"][0])
        last_partial["window_index"] = 5
        last_partial["window_start_ms"] = 50.0
        last_partial["window_duration_ms"] = 4.0
        document["windows"].extend((full_sparse, last_partial))
        validate_output_document(document, self.source, self.trace)

    def test_rejects_early_partial_window(self):
        document = valid_document(self.trace)
        document["windows"][0]["window_duration_ms"] = 5.0
        later = copy.deepcopy(document["windows"][0])
        later["window_index"] = 2
        later["window_start_ms"] = 20.0
        later["window_duration_ms"] = 10.0
        document["windows"].append(later)
        self.assert_document_error(document, "$.windows[0].window_duration_ms")

    def test_rejects_invalid_window_duration_index_start_and_order(self):
        mutations = (
            ("zero duration", lambda windows: windows[0].__setitem__("window_duration_ms", 0.0)),
            ("wide duration", lambda windows: windows[0].__setitem__("window_duration_ms", 10.1)),
            ("wrong start", lambda windows: windows[0].__setitem__("window_start_ms", 1.0)),
            (
                "wrong order",
                lambda windows: windows.append({
                    **copy.deepcopy(windows[0]),
                    "window_index": 0,
                    "window_start_ms": 0.0,
                }),
            ),
        )
        for name, mutate in mutations:
            with self.subTest(name=name):
                document = valid_document(self.trace)
                mutate(document["windows"])
                self.assert_document_error(document, "$.windows")

    def test_rejects_non_integer_schema_versions(self):
        for invalid in (True, 1.0):
            with self.subTest(invalid=invalid):
                document = valid_document(self.trace)
                document["schema_version"] = invalid
                self.assert_document_error(document, "$.schema_version")

    def test_rejects_wrong_measurement_semantics_types_and_values(self):
        for invalid in (7, "physical AP only"):
            with self.subTest(invalid=invalid):
                document = valid_document(self.trace)
                document["measurement_semantics"]["access_point_role"] = invalid
                self.assert_document_error(
                    document, "$.measurement_semantics.access_point_role"
                )

    def test_rejects_invalid_required_scalar_types(self):
        document = valid_document(self.trace)
        app = document["windows"][0]["access_points"][0]["app_stats"]["uplink"]
        app["accepted_payload_bytes"] = "100"
        self.assert_document_error(document, ".accepted_payload_bytes")

        document = valid_document(self.trace)
        document["windows"][0]["access_points"][0]["app_stats"]["uplink"][
            "accepted_send_count"
        ] = True
        self.assert_document_error(document, ".accepted_send_count")

        document = valid_document(self.trace)
        document["windows"][0]["access_points"][0]["general_stats"]["uplink"][
            "matched_packet_count"
        ] = None
        self.assert_document_error(document, ".matched_packet_count")

        document = valid_document(self.trace)
        document["validation"]["overall_matches_windows"] = 1
        self.assert_document_error(document, "$.validation.overall_matches_windows")

    def test_rejects_nonfinite_numbers(self):
        for invalid in (float("inf"), float("-inf"), float("nan")):
            with self.subTest(invalid=invalid):
                document = valid_document(self.trace)
                document["windows"][0]["window_start_ms"] = invalid
                self.assert_document_error(document, "$.windows[0].window_start_ms")

        document = valid_document(self.trace)
        document["windows"][0]["access_points"][0]["phy_stats"]["uplink"][
            "average_data_rate_mbps"
        ] = float("inf")
        self.assert_document_error(document, ".average_data_rate_mbps")

    def test_rejects_invalid_nested_and_configuration_types(self):
        document = valid_document(self.trace)
        document["windows"][0]["access_points"][0]["app_stats"]["uplink"]["agents"][0][
            "agent_key"
        ] = 1
        self.assert_document_error(document, ".agent_key")

        document = valid_document(self.trace)
        document["experiment_metadata"]["configuration"]["simulation"]["rng_seed"] = True
        self.assert_document_error(
            document, "$.experiment_metadata.configuration.simulation.rng_seed"
        )

        document = valid_document(self.trace)
        document["experiment_metadata"]["configuration"]["general"]["trace_file"] = None
        self.assert_document_error(
            document, "$.experiment_metadata.configuration.general.trace_file"
        )

    def test_rejects_malformed_json_and_hierarchy_with_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output.json"
            output.write_text("{broken", encoding="utf-8")
            with self.assertRaisesRegex(LiveTraceError, str(output)):
                load_output_document(output, self.trace)
        self.assert_path_error(
            validate_output_document, [], self.source, self.trace, text="expected object"
        )

    def test_rejects_removed_root_bad_entity_category_and_validation(self):
        document = valid_document(self.trace)
        document["wifi_windows"] = []
        self.assert_document_error(document, "wifi_windows")

        document = valid_document(self.trace)
        document["windows"][0]["stations"][0]["node_id"] = 999
        self.assert_document_error(document, "inventory")

        document = valid_document(self.trace)
        del document["overall"]["access_points"][0]["tcp_stats"]
        self.assert_document_error(document, "tcp_stats")

        document = valid_document(self.trace)
        del document["validation"]["overall_matches_windows"]
        self.assert_document_error(document, "overall_matches_windows")

    def test_rejects_duplicate_and_unsorted_inventory_identities(self):
        duplicate_ap = valid_document(self.trace)
        duplicate_ap["experiment_metadata"]["entity_inventory"]["access_points"].append({
            "access_point_id": 0,
            "node_id": 3,
            "node_label": "node-3",
            "ipv4": "10.2.0.1",
        })
        duplicate_ap["overall"]["access_points"].append(
            entity(access_point_id=0, node_id=3, ipv4="10.2.0.1")
        )
        self.assert_document_error(duplicate_ap, "duplicate access_point_id")

        unsorted_ap = valid_document(self.trace)
        add_second_bss(unsorted_ap)
        unsorted_ap["experiment_metadata"]["entity_inventory"]["access_points"].reverse()
        self.assert_document_error(
            unsorted_ap, "$.experiment_metadata.entity_inventory.access_points[1]"
        )

        duplicate_station = valid_document(self.trace)
        duplicate_station["experiment_metadata"]["entity_inventory"]["stations"].append({
            "access_point_id": 0,
            "station_index": 0,
            "node_id": 3,
            "node_label": "node-3",
            "ipv4": "10.1.0.3",
        })
        duplicate_station["overall"]["stations"].append(
            entity(station_index=0, node_id=3, ipv4="10.1.0.3")
        )
        self.assert_document_error(duplicate_station, "duplicate station identity")

        unsorted_station = valid_document(self.trace)
        unsorted_station["experiment_metadata"]["entity_inventory"]["stations"].append({
            "access_point_id": 0,
            "station_index": 1,
            "node_id": 3,
            "node_label": "node-3",
            "ipv4": "10.1.0.3",
        })
        unsorted_station["overall"]["stations"].append(
            entity(station_index=1, node_id=3, ipv4="10.1.0.3")
        )
        unsorted_station["experiment_metadata"]["entity_inventory"]["stations"].reverse()
        self.assert_document_error(
            unsorted_station, "$.experiment_metadata.entity_inventory.stations[1]"
        )

    def test_rejects_duplicate_node_and_ip_independently_across_inventory(self):
        duplicate_node = valid_document(self.trace)
        duplicate_node["experiment_metadata"]["entity_inventory"]["stations"][0][
            "node_id"
        ] = 1
        duplicate_node["windows"][0]["stations"][0]["node_id"] = 1
        duplicate_node["overall"]["stations"][0]["node_id"] = 1
        self.assert_document_error(duplicate_node, "duplicate node_id")

        duplicate_ip = valid_document(self.trace)
        duplicate_ip["experiment_metadata"]["entity_inventory"]["stations"][0]["ipv4"] = (
            "10.1.0.1"
        )
        duplicate_ip["windows"][0]["stations"][0]["ipv4"] = "10.1.0.1"
        duplicate_ip["overall"]["stations"][0]["ipv4"] = "10.1.0.1"
        for section in ("windows", "overall"):
            root = duplicate_ip[section]
            roots = root if section == "windows" else [root]
            for container in roots:
                for access_point in container["access_points"]:
                    peer_groups = (
                        access_point["app_stats"]["uplink"]["peers"],
                        access_point["tcp_stats"]["uplink"]["connections"],
                        access_point["mac_stats"]["uplink"]["peers"],
                        access_point["phy_stats"]["uplink"]["peers"],
                    )
                    for peers in peer_groups:
                        for peer in peers:
                            peer["peer_ipv4"] = "10.1.0.1"
        self.assert_document_error(duplicate_ip, "duplicate ipv4")
