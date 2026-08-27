#!/usr/bin/env python3
"""Read-only command-line audit for retained saturated TCP benchmark runs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Sequence, TextIO

from saturated_tcp_benchmark.audit import audit_run_directory


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Independently audit a retained saturated TCP benchmark run.",
    )
    parser.add_argument("run_directory", type=Path)
    return parser


def main(
    argv: Sequence[str] | None = None,
    *,
    output: TextIO | None = None,
    error: TextIO | None = None,
) -> int:
    """Print an ordered JSON report and return nonzero for discrepancies."""
    output = output if output is not None else sys.stdout
    error = error if error is not None else sys.stderr
    arguments = _argument_parser().parse_args(argv)
    try:
        report = audit_run_directory(arguments.run_directory)
    except (OSError, ValueError) as audit_error:
        print(f"error: cannot audit retained run: {audit_error}", file=error)
        return 2
    json.dump(report.as_dict(), output, indent=2, ensure_ascii=True)
    output.write("\n")
    return 0 if report.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
