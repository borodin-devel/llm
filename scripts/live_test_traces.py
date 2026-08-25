#!/usr/bin/env python3
"""Run deterministic shape checks or the complete live trace matrix."""

from __future__ import annotations

import argparse
import sys

from live_trace_cleanup import OwnedTemporaryRun, cleanup_run_directory
from live_trace_common import (
    POLICY,
    LiveTraceError,
    build_llm_command,
    reject_legacy_console,
    validate_policy_coverage,
)
from live_trace_runner import _run_captured, run_live_matrix, run_one_trace
from live_trace_schema import load_output_document, validate_output_document


def run_self_tests():
    """Aggregate every deterministic verifier self-test module."""
    import unittest

    from test_live_trace_cleanup import LiveTraceCleanupTest
    from test_live_trace_runner import LiveTraceRunnerTest
    from test_live_trace_schema_ordering import LiveTraceSchemaOrderingTest
    from test_live_trace_schema_root import LiveTraceSchemaRootTest

    test_cases = (
        LiveTraceSchemaRootTest,
        LiveTraceSchemaOrderingTest,
        LiveTraceCleanupTest,
        LiveTraceRunnerTest,
    )
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite(
        loader.loadTestsFromTestCase(test_case) for test_case in test_cases
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run deterministic script tests")
    arguments = parser.parse_args()
    try:
        return run_self_tests() if arguments.self_test else run_live_matrix()
    except LiveTraceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
