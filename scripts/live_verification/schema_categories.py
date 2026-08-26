"""Validators for general, APP, TCP, MAC, and PHY entity categories."""

from __future__ import annotations

from live_verification.common import (
    APP_AGENT_KEYS, APP_KEYS, APP_PEER_KEYS, DIRECTIONS, GENERAL_KEYS, MAC_KEYS,
    MAC_PEER_KEYS, MAC_REASON_KEYS, PHY_DIRECTION_KEYS, PHY_KEYS, PHY_PEER_KEYS,
    SAMPLE_KEYS, TCP_CONNECTION_KEYS, TCP_KEYS, expect_finite_number, expect_list,
    expect_nonnegative_integer, expect_object_keys, expect_optional_finite_number,
    expect_optional_nonnegative_integer, expect_string, fail, is_nonnegative_integer,
    validate_integer_fields, validate_optional_number_fields, validate_ordered_unique,
)


def _validate_sample_distribution(value, source_path, json_path):
    expect_object_keys(value, SAMPLE_KEYS, source_path, json_path)
    expect_nonnegative_integer(value["sample_count"], source_path, f"{json_path}.sample_count")
    derived = {"average_us", "standard_deviation_us", "minimum_us", "maximum_us"}
    validate_optional_number_fields(value, derived, source_path, json_path)
    if value["sample_count"] == 0 and any(value[field] is not None for field in derived):
        fail(source_path, json_path, "zero-sample distribution has derived values")
    if value["sample_count"] > 0 and any(value[field] is None for field in derived):
        fail(source_path, json_path, "sampled distribution has null derived values")


def _validate_peer(peer, expected_keys, known_nodes, source_path, json_path):
    expect_object_keys(peer, expected_keys, source_path, json_path)
    peer_node_id = peer["peer_node_id"]
    if not is_nonnegative_integer(peer_node_id) or peer_node_id not in known_nodes:
        fail(source_path, f"{json_path}.peer_node_id", "does not reference entity inventory")
    expect_string(peer["peer_ipv4"], source_path, f"{json_path}.peer_ipv4")
    if peer["peer_ipv4"] != known_nodes[peer_node_id]["ipv4"]:
        fail(source_path, f"{json_path}.peer_ipv4", "does not match inventory")


def _validate_reason_array(reasons, source_path, json_path):
    expect_list(reasons, source_path, json_path)
    for index, reason in enumerate(reasons):
        reason_path = f"{json_path}[{index}]"
        expect_object_keys(reason, MAC_REASON_KEYS, source_path, reason_path)
        validate_integer_fields(reason, MAC_REASON_KEYS, source_path, reason_path)
    validate_ordered_unique(
        reasons, lambda reason: reason["reason_code"], source_path, json_path, "reason_code"
    )


def _validate_general_direction(value, source_path, json_path):
    validate_integer_fields(
        value,
        {
            "estimated_transmitted_tcp_payload_bytes", "estimated_matched_tcp_payload_bytes",
            "matched_packet_count", "total_transmission_duration_us",
        },
        source_path,
        json_path,
    )
    validate_optional_number_fields(
        value,
        {
            "average_transmission_duration_us", "transmission_duration_standard_deviation_us",
            "minimum_transmission_duration_us", "maximum_transmission_duration_us",
            "effective_throughput_mbps",
        },
        source_path,
        json_path,
    )
    _validate_sample_distribution(
        value["application_to_phy_delay"], source_path, f"{json_path}.application_to_phy_delay"
    )


def _validate_app_agent(agent, source_path, json_path):
    expect_object_keys(agent, APP_AGENT_KEYS, source_path, json_path)
    expect_string(agent["agent_key"], source_path, f"{json_path}.agent_key")
    validate_integer_fields(
        agent,
        {"accepted_send_count", "accepted_payload_bytes", "drop_event_count", "dropped_payload_bytes"},
        source_path,
        json_path,
    )
    validate_optional_number_fields(agent, {"accepted_throughput_mbps"}, source_path, json_path)
    validate_optional_number_fields(
        agent, {"accepted_bandwidth_share_percent"}, source_path, json_path, maximum=100.0
    )


def _validate_app_peer(peer, known_nodes, source_path, json_path):
    _validate_peer(peer, APP_PEER_KEYS, known_nodes, source_path, json_path)
    validate_integer_fields(
        peer,
        {
            "accepted_send_count", "accepted_payload_bytes", "receive_event_count",
            "received_payload_bytes", "drop_event_count", "dropped_payload_bytes",
        },
        source_path,
        json_path,
    )
    validate_optional_number_fields(
        peer, {"accepted_throughput_mbps", "received_throughput_mbps"}, source_path, json_path
    )
    validate_optional_number_fields(
        peer,
        {"accepted_bandwidth_share_percent", "received_bandwidth_share_percent"},
        source_path,
        json_path,
        maximum=100.0,
    )


def _validate_app_direction(value, known_nodes, source_path, json_path):
    validate_integer_fields(
        value,
        {
            "accepted_send_count", "accepted_payload_bytes", "receive_event_count",
            "received_payload_bytes", "drop_event_count", "dropped_payload_bytes",
        },
        source_path,
        json_path,
    )
    validate_optional_number_fields(
        value, {"accepted_throughput_mbps", "received_throughput_mbps"}, source_path, json_path
    )
    _validate_sample_distribution(
        value["receive_interarrival_time"], source_path, f"{json_path}.receive_interarrival_time"
    )
    agents_path = f"{json_path}.agents"
    expect_list(value["agents"], source_path, agents_path)
    for index, agent in enumerate(value["agents"]):
        _validate_app_agent(agent, source_path, f"{agents_path}[{index}]")
    validate_ordered_unique(
        value["agents"], lambda agent: agent["agent_key"], source_path, agents_path, "agent_key"
    )
    peers_path = f"{json_path}.peers"
    expect_list(value["peers"], source_path, peers_path)
    for index, peer in enumerate(value["peers"]):
        _validate_app_peer(peer, known_nodes, source_path, f"{peers_path}[{index}]")
    validate_ordered_unique(
        value["peers"], lambda peer: peer["peer_node_id"], source_path, peers_path, "peer_node_id"
    )


def _validate_tcp_direction(value, known_nodes, source_path, json_path):
    connections_path = f"{json_path}.connections"
    expect_list(value["connections"], source_path, connections_path)
    for index, connection in enumerate(value["connections"]):
        path = f"{connections_path}[{index}]"
        _validate_peer(connection, TCP_CONNECTION_KEYS, known_nodes, source_path, path)
        expect_nonnegative_integer(
            connection["congestion_window_observation_duration_us"],
            source_path,
            f"{path}.congestion_window_observation_duration_us",
        )
        expect_optional_finite_number(
            connection["average_congestion_window_bytes"],
            source_path,
            f"{path}.average_congestion_window_bytes",
        )
        expect_optional_nonnegative_integer(
            connection["last_congestion_window_bytes"],
            source_path,
            f"{path}.last_congestion_window_bytes",
        )
        _validate_sample_distribution(
            connection["round_trip_time"], source_path, f"{path}.round_trip_time"
        )
    validate_ordered_unique(
        value["connections"], lambda item: item["peer_node_id"], source_path,
        connections_path, "peer_node_id",
    )


def _validate_mac_peer(peer, known_nodes, source_path, json_path):
    _validate_peer(peer, MAC_PEER_KEYS, known_nodes, source_path, json_path)
    validate_integer_fields(
        peer,
        {
            "estimated_transmit_event_count", "estimated_transmitted_tcp_payload_bytes",
            "estimated_receive_event_count", "estimated_received_tcp_payload_bytes",
            "mpdu_drop_count", "mpdu_drop_bytes", "data_failure_count", "final_data_failure_count",
        },
        source_path,
        json_path,
    )
    validate_optional_number_fields(
        peer,
        {"estimated_transmit_throughput_mbps", "estimated_receive_throughput_mbps"},
        source_path,
        json_path,
    )
    _validate_reason_array(
        peer["mpdu_drops_by_reason"], source_path, f"{json_path}.mpdu_drops_by_reason"
    )


def _validate_mac_direction(value, known_nodes, source_path, json_path):
    validate_integer_fields(
        value,
        {
            "estimated_transmit_event_count", "estimated_transmitted_tcp_payload_bytes",
            "estimated_receive_event_count", "estimated_received_tcp_payload_bytes",
            "transmit_drop_count", "transmit_drop_packet_bytes", "mpdu_drop_count",
            "mpdu_drop_bytes", "data_failure_count", "final_data_failure_count",
        },
        source_path,
        json_path,
    )
    validate_optional_number_fields(
        value,
        {"estimated_transmit_throughput_mbps", "estimated_receive_throughput_mbps"},
        source_path,
        json_path,
    )
    _validate_reason_array(
        value["mpdu_drops_by_reason"], source_path, f"{json_path}.mpdu_drops_by_reason"
    )
    peers_path = f"{json_path}.peers"
    expect_list(value["peers"], source_path, peers_path)
    for index, peer in enumerate(value["peers"]):
        _validate_mac_peer(peer, known_nodes, source_path, f"{peers_path}[{index}]")
    validate_ordered_unique(
        value["peers"], lambda peer: peer["peer_node_id"], source_path, peers_path, "peer_node_id"
    )


def _validate_phy_peer(peer, known_nodes, source_path, json_path):
    _validate_peer(peer, PHY_PEER_KEYS, known_nodes, source_path, json_path)
    validate_integer_fields(
        peer,
        {
            "tagged_payload_bytes", "unique_tagged_payload_bytes", "transmission_attempt_count",
            "retransmission_count",
        },
        source_path,
        json_path,
    )
    expect_finite_number(
        peer["transmission_airtime_us"], source_path, f"{json_path}.transmission_airtime_us"
    )
    validate_optional_number_fields(
        peer, {"average_data_rate_mbps", "throughput_mbps"}, source_path, json_path
    )


def _validate_phy_direction(value, known_nodes, source_path, json_path):
    validate_integer_fields(
        value,
        {
            "tagged_payload_bytes", "unique_tagged_payload_bytes", "tagged_mpdu_count",
            "complete_tagged_mpdu_bytes", "transmission_attempt_count", "retransmission_count",
        },
        source_path,
        json_path,
    )
    expect_finite_number(
        value["transmission_airtime_us"], source_path, f"{json_path}.transmission_airtime_us"
    )
    validate_optional_number_fields(
        value, {"average_data_rate_mbps", "throughput_mbps"}, source_path, json_path
    )
    peers_path = f"{json_path}.peers"
    expect_list(value["peers"], source_path, peers_path)
    for index, peer in enumerate(value["peers"]):
        _validate_phy_peer(peer, known_nodes, source_path, f"{peers_path}[{index}]")
    validate_ordered_unique(
        value["peers"], lambda peer: peer["peer_node_id"], source_path, peers_path, "peer_node_id"
    )


def validate_entity_categories(record, known_nodes, source_path, json_path):
    """Validate all fixed category and direction records for one entity."""
    validators = {
        "general_stats": (GENERAL_KEYS, _validate_general_direction),
        "app_stats": (APP_KEYS, _validate_app_direction),
        "tcp_stats": (TCP_KEYS, _validate_tcp_direction),
        "mac_stats": (MAC_KEYS, _validate_mac_direction),
    }
    for category_name, (expected_keys, validator) in validators.items():
        category_path = f"{json_path}.{category_name}"
        category = record[category_name]
        expect_object_keys(category, DIRECTIONS, source_path, category_path)
        for direction in sorted(DIRECTIONS):
            direction_path = f"{category_path}.{direction}"
            value = category[direction]
            expect_object_keys(value, expected_keys, source_path, direction_path)
            if category_name == "general_stats":
                validator(value, source_path, direction_path)
            else:
                validator(value, known_nodes, source_path, direction_path)

    phy = record["phy_stats"]
    phy_path = f"{json_path}.phy_stats"
    expect_object_keys(phy, PHY_KEYS, source_path, phy_path)
    expect_nonnegative_integer(phy["busy_time_us"], source_path, f"{phy_path}.busy_time_us")
    expect_optional_finite_number(
        phy["channel_utilization_percent"], source_path,
        f"{phy_path}.channel_utilization_percent", maximum=100.0,
    )
    for direction in sorted(DIRECTIONS):
        direction_path = f"{phy_path}.{direction}"
        value = phy[direction]
        expect_object_keys(value, PHY_DIRECTION_KEYS, source_path, direction_path)
        _validate_phy_direction(value, known_nodes, source_path, direction_path)
