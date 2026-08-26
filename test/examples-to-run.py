#! /usr/bin/env python3

cpp_examples = [
    (
        "llm-scenario --config ../../contrib/llm/config/llm_config.toml "
        "--general-trace-file ../../contrib/llm/test/data/minimal-trace.json "
        "--general-run-folder . "
        "--general-output-name llm-smoke-stats.json "
        "--simulation-duration-mode fixed "
        "--simulation-fixed-duration-seconds 0.2",
        "True",
        "False",
    ),
    (
        "saturated-tcp-scenario --config ../../contrib/llm/config/saturated_tcp_config.toml "
        "--general-run-folder . "
        "--general-output-name saturated-tcp-smoke-output.json "
        "--benchmark-sta-count-per-bss 1 "
        "--benchmark-rssi-range high "
        "--benchmark-interference-mode isolated "
        "--benchmark-traffic-mode ul "
        "--benchmark-mimo-mode su "
        "--statistics-window-ms 10 "
        "&& python3 -c \"import json,pathlib; p=pathlib.Path('saturated-tcp-smoke-output.json'); "
        "d=json.loads(p.read_text()); "
        "assert tuple(d)==('schema_version','measurement_semantics','statistics_window_ms',"
        "'windows','overall','validation','experiment_metadata'); "
        "assert d.get('schema_version')==1 and d.get('statistics_window_ms')==10 and "
        "len(d.get('windows'))==100; "
        "i=d.get('experiment_metadata').get('entity_inventory'); "
        "assert len(i.get('access_points'))==3 and len(i.get('stations'))==3; "
        "o=d.get('overall'); "
        "assert len(o.get('access_points'))==3 and len(o.get('stations'))==3; "
        "assert all(all(e.get('phy_stats').get(k) is not None for k in "
        "('average_theoretical_phy_rate_mbps','average_practical_phy_rate_mbps',"
        "'channel_efficiency','contention_fraction')) for group in "
        "('access_points','stations') for e in o.get(group)); "
        "b=d.get('experiment_metadata').get('configuration').get('benchmark'); "
        "assert b.get('sta_count_per_bss')==1 and b.get('rssi_range')=='high' and "
        "b.get('interference_mode')=='isolated' and b.get('traffic_mode')=='ul' and "
        "b.get('mimo_mode')=='su'; "
        "v=d.get('validation'); assert len(v)==8 and all(v.values()); "
        "p.unlink(); assert not p.exists()\"",
        "True",
        "False",
    ),
]

python_examples = []
