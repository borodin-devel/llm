"""Hand-checked JSON fixtures shared by live verifier self-tests."""

from __future__ import annotations

import copy

from live_verification.common import VALIDATION_KEYS


def sample_distribution():
    return {
        "sample_count": 0,
        "average_us": None,
        "standard_deviation_us": None,
        "minimum_us": None,
        "maximum_us": None,
    }


def entity(access_point_id=0, station_index=None, node_id=1, ipv4="10.1.0.1"):
    general = {
        "estimated_transmitted_tcp_payload_bytes": 0,
        "estimated_matched_tcp_payload_bytes": 0,
        "matched_packet_count": 0,
        "total_transmission_duration_us": 0,
        "average_transmission_duration_us": None,
        "transmission_duration_standard_deviation_us": None,
        "minimum_transmission_duration_us": None,
        "maximum_transmission_duration_us": None,
        "effective_throughput_mbps": None,
        "application_to_phy_delay": sample_distribution(),
    }
    app = {
        "accepted_send_count": 0,
        "accepted_payload_bytes": 0,
        "accepted_throughput_mbps": 0.0,
        "receive_event_count": 0,
        "received_payload_bytes": 0,
        "received_throughput_mbps": 0.0,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
        "receive_interarrival_time": sample_distribution(),
        "agents": [],
        "peers": [],
    }
    tcp = {"connections": []}
    mac = {
        "estimated_transmit_event_count": 0,
        "estimated_transmitted_tcp_payload_bytes": 0,
        "estimated_transmit_throughput_mbps": 0.0,
        "estimated_receive_event_count": 0,
        "estimated_received_tcp_payload_bytes": 0,
        "estimated_receive_throughput_mbps": 0.0,
        "transmit_drop_count": 0,
        "transmit_drop_packet_bytes": 0,
        "mpdu_drop_count": 0,
        "mpdu_drop_bytes": 0,
        "data_failure_count": 0,
        "final_data_failure_count": 0,
        "mpdu_drops_by_reason": [],
        "peers": [],
    }
    phy_direction = {
        "tagged_payload_bytes": 0,
        "unique_tagged_payload_bytes": 0,
        "tagged_mpdu_count": 0,
        "complete_tagged_mpdu_bytes": 0,
        "transmission_attempt_count": 0,
        "retransmission_count": 0,
        "transmission_airtime_us": 0.0,
        "average_data_rate_mbps": None,
        "throughput_mbps": 0.0,
        "peers": [],
    }
    result = {
        "access_point_id": access_point_id,
        "node_id": node_id,
        "node_label": f"node-{node_id}",
        "ipv4": ipv4,
        "general_stats": {"uplink": copy.deepcopy(general), "downlink": copy.deepcopy(general)},
        "app_stats": {"uplink": copy.deepcopy(app), "downlink": copy.deepcopy(app)},
        "tcp_stats": {"uplink": copy.deepcopy(tcp), "downlink": copy.deepcopy(tcp)},
        "mac_stats": {"uplink": copy.deepcopy(mac), "downlink": copy.deepcopy(mac)},
        "phy_stats": {
            "busy_time_us": 0,
            "channel_utilization_percent": 0.0,
            "uplink": copy.deepcopy(phy_direction),
            "downlink": copy.deepcopy(phy_direction),
        },
    }
    if station_index is not None:
        result["station_index"] = station_index
    return result


def populate_nested_entity(record, peer_node_id, peer_ipv4):
    app = record["app_stats"]["uplink"]
    app["agents"] = [{
        "agent_key": "agent-1",
        "accepted_send_count": 1,
        "accepted_payload_bytes": 100,
        "accepted_throughput_mbps": 0.08,
        "accepted_bandwidth_share_percent": 100.0,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
    }]
    app["peers"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "accepted_send_count": 1,
        "accepted_payload_bytes": 100,
        "accepted_throughput_mbps": 0.08,
        "accepted_bandwidth_share_percent": 100.0,
        "receive_event_count": 1,
        "received_payload_bytes": 90,
        "received_throughput_mbps": 0.072,
        "received_bandwidth_share_percent": 100.0,
        "drop_event_count": 0,
        "dropped_payload_bytes": 0,
    }]
    record["tcp_stats"]["uplink"]["connections"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "congestion_window_observation_duration_us": 10000,
        "average_congestion_window_bytes": 2048.0,
        "last_congestion_window_bytes": 4096,
        "round_trip_time": {
            "sample_count": 1,
            "average_us": 100.0,
            "standard_deviation_us": 0.0,
            "minimum_us": 100.0,
            "maximum_us": 100.0,
        },
    }]
    mac = record["mac_stats"]["uplink"]
    mac["mpdu_drops_by_reason"] = [{"reason_code": 1, "drop_count": 1}]
    mac["peers"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "estimated_transmit_event_count": 1,
        "estimated_transmitted_tcp_payload_bytes": 100,
        "estimated_transmit_throughput_mbps": 0.08,
        "estimated_receive_event_count": 1,
        "estimated_received_tcp_payload_bytes": 90,
        "estimated_receive_throughput_mbps": 0.072,
        "mpdu_drop_count": 1,
        "mpdu_drop_bytes": 100,
        "data_failure_count": 1,
        "final_data_failure_count": 0,
        "mpdu_drops_by_reason": [{"reason_code": 1, "drop_count": 1}],
    }]
    record["phy_stats"]["uplink"]["peers"] = [{
        "peer_node_id": peer_node_id,
        "peer_ipv4": peer_ipv4,
        "tagged_payload_bytes": 100,
        "unique_tagged_payload_bytes": 100,
        "transmission_attempt_count": 1,
        "retransmission_count": 0,
        "transmission_airtime_us": 10.0,
        "average_data_rate_mbps": 80.0,
        "throughput_mbps": 0.08,
    }]


def valid_document(trace_path, run_directory="/tmp/llm-trace-live.test.random"):
    access_point = entity()
    station = entity(station_index=0, node_id=2, ipv4="10.1.0.2")
    populate_nested_entity(access_point, 2, "10.1.0.2")
    populate_nested_entity(station, 1, "10.1.0.1")
    configuration = {
        "general": {
            "trace_file": trace_path,
            "run_folder": run_directory,
            "output_name": "output.json",
        },
        "simulation": {
            "duration_mode": "auto",
            "fixed_duration_seconds": 0.0,
            "auto_tail_seconds": 2.0,
            "rng_seed": 12345,
            "rng_run": 1,
        },
        "topology": {
            "bss_count": 1,
            "stations_per_bss": 1,
            "bss_spacing_m": 100.0,
            "station_radius_m": 5.0,
            "isolate_bss_channels": True,
            "ssid_prefix": "llm-ap-",
            "ap_sink_port": 10000,
            "station_sink_base_port": 9000,
            "generator_start_seconds": 1.0,
        },
        "distribution": {
            "max_agents_per_station": 832,
            "low_contention_priority": True,
            "slot_ms": 10,
        },
        "wifi": {
            "band": "5GHz",
            "channel_number": 0,
            "bandwidth_mhz": 20,
            "primary_20_index": 0,
            "rate_manager": "ns3::MinstrelHtWifiManager",
            "active_probing": True,
        },
        "tcp": {
            "congestion_control": "ns3::TcpHighSpeed",
            "segment_size_bytes": 1460,
            "send_buffer_bytes": 33554432,
            "receive_buffer_bytes": 33554432,
        },
        "statistics": {"window_ms": 10},
        "logging": {
            "sample_scenario_level": "info",
            "ap_generator_level": "warn",
            "sta_generator_level": "warn",
            "traffic_sink_level": "warn",
            "contention_distribution_level": "info",
        },
    }
    return {
        "schema_version": 1,
        "measurement_semantics": {
            "access_point_role": "BSS parent aggregate",
            "station_role": "per-station child detail",
            "parent_child_duplication": "intentional",
            "mac_tcp_payload_bytes": "header-based estimates",
            "phy_tagged_payload_bytes": "attempts and retransmissions included",
            "phy_unique_tagged_payload_bytes": "first tagged MPDU transmissions only",
            "phy_average_data_rate": "airtime-weighted",
            "congestion_window": "time-weighted per connection",
            "sample_distributions": "sample-weighted",
            "sparse_window_absence": "zero activity",
            "undefined_derived_values": None,
        },
        "statistics_window_ms": 10,
        "windows": [{
            "window_index": 0,
            "window_start_ms": 0.0,
            "window_duration_ms": 10.0,
            "access_points": [copy.deepcopy(access_point)],
            "stations": [copy.deepcopy(station)],
        }],
        "overall": {
            "access_points": [copy.deepcopy(access_point)],
            "stations": [copy.deepcopy(station)],
        },
        "validation": {key: True for key in VALIDATION_KEYS},
        "experiment_metadata": {
            "configuration": configuration,
            "entity_inventory": {
                "access_points": [{
                    "access_point_id": 0,
                    "node_id": 1,
                    "node_label": "node-1",
                    "ipv4": "10.1.0.1",
                }],
                "stations": [{
                    "access_point_id": 0,
                    "station_index": 0,
                    "node_id": 2,
                    "node_label": "node-2",
                    "ipv4": "10.1.0.2",
                }],
            },
        },
    }


def add_second_bss(document):
    """Add a hand-checked second BSS to inventory, windows, and overall."""
    access_point = entity(access_point_id=1, node_id=3, ipv4="10.2.0.1")
    station = entity(
        access_point_id=1, station_index=0, node_id=4, ipv4="10.2.0.2"
    )
    inventory = document["experiment_metadata"]["entity_inventory"]
    inventory["access_points"].append({
        "access_point_id": 1,
        "node_id": 3,
        "node_label": "node-3",
        "ipv4": "10.2.0.1",
    })
    inventory["stations"].append({
        "access_point_id": 1,
        "station_index": 0,
        "node_id": 4,
        "node_label": "node-4",
        "ipv4": "10.2.0.2",
    })
    document["windows"][0]["access_points"].append(copy.deepcopy(access_point))
    document["windows"][0]["stations"].append(copy.deepcopy(station))
    document["overall"]["access_points"].append(copy.deepcopy(access_point))
    document["overall"]["stations"].append(copy.deepcopy(station))
