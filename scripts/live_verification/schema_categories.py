"""Validators for general, APP, TCP, MAC, and PHY entity categories."""

from __future__ import annotations

from live_verification.common import (
    APP_AGENT_KEYS, APP_KEYS, APP_PEER_KEYS, DATA_TX_PROFILE_KEYS, DIRECTIONS, GENERAL_KEYS,
    MAC_KEYS, MAC_PEER_KEYS, MAC_REASON_KEYS, PHY_DIRECTION_KEYS, PHY_KEYS, PHY_PEER_KEYS,
    SAMPLE_KEYS, TCP_CONNECTION_KEYS, TCP_KEYS, expect_finite_number, expect_list,
    expect_nonnegative_integer, expect_object_keys, expect_optional_finite_number,
    expect_optional_nonnegative_integer, expect_ordered_object_keys, expect_string, fail,
    is_nonnegative_integer, validate_integer_fields, validate_optional_number_fields,
    validate_ordered_unique,
)


STATION_PHY_RATE_KEYS = (
    "dominant_data_phy_rate_mbps", "effective_phy_rate_mbps",
    "data_tx_rate_over_interval_mbps",
)
BSS_PHY_RATE_KEYS = (
    "mean_dominant_data_phy_rate_mbps", "mean_effective_phy_rate_mbps",
    "aggregate_data_tx_rate_over_interval_mbps",
)
STATION_PHY_FIELDS = STATION_PHY_RATE_KEYS + (
    "dominant_data_profile_share", "data_tx_opportunity_gap_fraction",
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


def _validate_data_tx_profile(profiles, source_path, json_path):
    expect_list(profiles, source_path, json_path)
    for index, profile in enumerate(profiles):
        profile_path = f"{json_path}[{index}]"
        expect_ordered_object_keys(profile, DATA_TX_PROFILE_KEYS, source_path, profile_path)
        expect_nonnegative_integer(
            profile["channel_width_mhz"],
            source_path,
            f"{profile_path}.channel_width_mhz",
            positive=True,
        )
        if profile["channel_width_mhz"] not in {20, 40, 80}:
            fail(
                source_path,
                f"{profile_path}.channel_width_mhz",
                "expected 20, 40, or 80",
            )
        expect_nonnegative_integer(profile["nss"], source_path, f"{profile_path}.nss", positive=True)
        expect_nonnegative_integer(
            profile["mcs"], source_path, f"{profile_path}.mcs", maximum=11
        )
        expect_finite_number(
            profile["transmitted_psdu_bytes"],
            source_path,
            f"{profile_path}.transmitted_psdu_bytes",
        )
        expect_nonnegative_integer(
            profile["ppdu_attempt_count"], source_path, f"{profile_path}.ppdu_attempt_count"
        )
        expect_finite_number(
            profile["ppdu_airtime_us"], source_path, f"{profile_path}.ppdu_airtime_us"
        )
    validate_ordered_unique(
        profiles,
        lambda profile: (
            profile["channel_width_mhz"], profile["nss"], profile["mcs"]
        ),
        source_path,
        json_path,
        "width/NSS/MCS profile",
    )


def _validate_data_tx_roles(phy, kind, source_path, phy_path):
    for field in STATION_PHY_RATE_KEYS + BSS_PHY_RATE_KEYS:
        expect_optional_finite_number(phy[field], source_path, f"{phy_path}.{field}")
    share = phy["dominant_data_profile_share"]
    if share is not None:
        expect_finite_number(share, source_path, f"{phy_path}.dominant_data_profile_share", positive=True, maximum=1.0)
    gap = phy["data_tx_opportunity_gap_fraction"]
    expect_optional_finite_number(
        gap, source_path, f"{phy_path}.data_tx_opportunity_gap_fraction", maximum=1.0
    )
    _validate_data_tx_profile(phy["data_tx_profile"], source_path, f"{phy_path}.data_tx_profile")

    if kind == "station":
        for field in BSS_PHY_RATE_KEYS:
            if phy[field] is not None:
                fail(source_path, f"{phy_path}.{field}", "BSS aggregate is populated on station")
        if phy["data_tx_profile"]:
            if any(phy[field] is None for field in STATION_PHY_FIELDS):
                fail(source_path, phy_path, "populated station profile has undefined data metrics")
        else:
            for field in (
                "dominant_data_phy_rate_mbps", "dominant_data_profile_share",
                "effective_phy_rate_mbps", "data_tx_opportunity_gap_fraction",
            ):
                if phy[field] is not None:
                    fail(source_path, f"{phy_path}.{field}", "station metric exists without profile")
            interval_rate = phy["data_tx_rate_over_interval_mbps"]
            if interval_rate is not None and interval_rate != 0:
                fail(
                    source_path,
                    f"{phy_path}.data_tx_rate_over_interval_mbps",
                    "profile-free station interval rate is not zero",
                )
        return

    for field in STATION_PHY_FIELDS:
        if phy[field] is not None:
            fail(source_path, f"{phy_path}.{field}", "station metric is populated on BSS")
    if phy["data_tx_profile"]:
        fail(source_path, f"{phy_path}.data_tx_profile", "BSS contains a station profile")
    aggregate = phy["aggregate_data_tx_rate_over_interval_mbps"]
    populated_means = sum(phy[field] is not None for field in BSS_PHY_RATE_KEYS[:2])
    if aggregate is None:
        if populated_means:
            fail(source_path, phy_path, "ordinary BSS has populated means")
    elif aggregate == 0:
        if populated_means:
            fail(source_path, phy_path, "idle BSS has populated means")
    elif populated_means != 2:
        fail(source_path, phy_path, "active BSS requires both means")


def validate_entity_categories(record, kind, known_nodes, source_path, json_path):
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
    expect_ordered_object_keys(phy, PHY_KEYS, source_path, phy_path)
    _validate_data_tx_roles(phy, kind, source_path, phy_path)
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
