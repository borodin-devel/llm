"""Self-tests for deterministic entity and nested-array ordering and uniqueness."""

from __future__ import annotations

import copy
from pathlib import Path
import unittest

from live_verification.common import LiveTraceError
from live_verification.schema import validate_output_document
from tests.live_verification.fixtures import add_second_bss, valid_document


class LiveTraceSchemaOrderingTest(unittest.TestCase):
    def setUp(self):
        self.trace = "contrib/llm/traces/1W_high_load_1s.json"
        self.source = Path("/tmp/fake/output.json")

    def assert_document_error(self, document, text):
        with self.assertRaisesRegex(LiveTraceError, str(self.source)) as context:
            validate_output_document(document, self.source, self.trace)
        self.assertIn(text, str(context.exception))

    def test_rejects_duplicate_and_unsorted_window_and_overall_entities(self):
        for root_name in ("windows", "overall"):
            for entity_name in ("access_points", "stations"):
                with self.subTest(root=root_name, entities=entity_name, case="order"):
                    document = valid_document(self.trace)
                    add_second_bss(document)
                    container = (
                        document[root_name][0]
                        if root_name == "windows"
                        else document[root_name]
                    )
                    container[entity_name].reverse()
                    self.assert_document_error(document, f"$.{root_name}")
                with self.subTest(root=root_name, entities=entity_name, case="duplicate"):
                    document = valid_document(self.trace)
                    container = (
                        document[root_name][0]
                        if root_name == "windows"
                        else document[root_name]
                    )
                    container[entity_name].append(copy.deepcopy(container[entity_name][0]))
                    self.assert_document_error(document, f"$.{root_name}")

    def test_rejects_duplicate_and_unsorted_agents(self):
        for case in ("duplicate", "order"):
            with self.subTest(case=case):
                document = valid_document(self.trace)
                agents = document["windows"][0]["access_points"][0]["app_stats"]["uplink"][
                    "agents"
                ]
                second = copy.deepcopy(agents[0])
                if case == "order":
                    second["agent_key"] = "agent-2"
                    agents.append(second)
                    agents.reverse()
                else:
                    agents.append(second)
                self.assert_document_error(document, ".agents[1]")

    def test_rejects_duplicate_and_unsorted_app_mac_and_phy_peers(self):
        selectors = (
            ("app", lambda entity: entity["app_stats"]["uplink"]["peers"]),
            ("mac", lambda entity: entity["mac_stats"]["uplink"]["peers"]),
            ("phy", lambda entity: entity["phy_stats"]["uplink"]["peers"]),
        )
        for category, select in selectors:
            for case in ("duplicate", "order"):
                with self.subTest(category=category, case=case):
                    document = valid_document(self.trace)
                    add_second_bss(document)
                    peers = select(document["windows"][0]["access_points"][0])
                    second = copy.deepcopy(peers[0])
                    if case == "order":
                        second["peer_node_id"] = 4
                        second["peer_ipv4"] = "10.2.0.2"
                        peers.append(second)
                        peers.reverse()
                    else:
                        peers.append(second)
                    self.assert_document_error(document, f".{category}_stats")

    def test_rejects_duplicate_and_unsorted_reason_arrays(self):
        selectors = (
            (
                "direction",
                lambda entity: entity["mac_stats"]["uplink"]["mpdu_drops_by_reason"],
            ),
            (
                "peer",
                lambda entity: entity["mac_stats"]["uplink"]["peers"][0][
                    "mpdu_drops_by_reason"
                ],
            ),
        )
        for location, select in selectors:
            for case in ("duplicate", "order"):
                with self.subTest(location=location, case=case):
                    document = valid_document(self.trace)
                    reasons = select(document["windows"][0]["access_points"][0])
                    second = copy.deepcopy(reasons[0])
                    if case == "order":
                        second["reason_code"] = 2
                        reasons.append(second)
                        reasons.reverse()
                    else:
                        reasons.append(second)
                    self.assert_document_error(document, "reason_code")

    def test_rejects_duplicate_and_unsorted_tcp_connections(self):
        for case in ("duplicate", "order"):
            with self.subTest(case=case):
                document = valid_document(self.trace)
                add_second_bss(document)
                connections = document["windows"][0]["access_points"][0]["tcp_stats"][
                    "uplink"
                ]["connections"]
                second = copy.deepcopy(connections[0])
                if case == "order":
                    second["peer_node_id"] = 4
                    second["peer_ipv4"] = "10.2.0.2"
                    connections.append(second)
                    connections.reverse()
                else:
                    connections.append(second)
                self.assert_document_error(document, "peer_node_id")
