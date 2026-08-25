#! /usr/bin/env python3

cpp_examples = [
    (
        "llm_sample --config ../../contrib/llm/config/basic_config.toml "
        "--general-trace-file ../../contrib/llm/test/data/minimal-trace.json "
        "--general-run-folder . "
        "--general-output-name llm-smoke-stats.json "
        "--simulation-duration-mode fixed "
        "--simulation-fixed-duration-seconds 0.2",
        "True",
        "False",
    ),
]

python_examples = []
