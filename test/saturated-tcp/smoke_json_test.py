#!/usr/bin/env python3

import pathlib
import tempfile
import unittest

from smoke_json import validate_and_cleanup


class SmokeJsonCleanupTest(unittest.TestCase):
    def test_failed_validation_removes_output(self):
        with tempfile.TemporaryDirectory() as temp_directory:
            output_path = pathlib.Path(temp_directory) / "invalid-output.json"
            output_path.write_text("{}", encoding="utf-8")

            with self.assertRaises(AssertionError):
                validate_and_cleanup(output_path)

            self.assertFalse(output_path.exists())


if __name__ == "__main__":
    unittest.main()
