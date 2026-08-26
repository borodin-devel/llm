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
        "&& PYTHONDONTWRITEBYTECODE=1 python3 "
        "../../contrib/llm/test/saturated-tcp/smoke_json.py "
        "saturated-tcp-smoke-output.json "
        "&& PYTHONDONTWRITEBYTECODE=1 python3 "
        "../../contrib/llm/test/saturated-tcp/smoke_json_test.py",
        "True",
        "False",
    ),
]

python_examples = []
