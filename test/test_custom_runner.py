"""
Custom PlatformIO test runner for SimpleLibAprs.

The test program (test.cpp) is a self-contained binary: it prints one
``ok``/``FAIL`` line per check and returns a non-zero exit code if any check
fails. This runner turns those lines into PlatformIO test cases so that
``pio test`` reports them individually, while the exit code still drives the
overall verdict.
"""

import re

from platformio.test.runners.base import TestRunnerBase
from platformio.test.result import TestCase, TestStatus

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


class CustomTestRunner(TestRunnerBase):
    def on_testing_line_output(self, line):
        # Echo the original (coloured) output to the console.
        super().on_testing_line_output(line)

        text = _ANSI.sub("", line).strip()
        if text.startswith("ok "):
            self.test_suite.add_case(
                TestCase(name=text[3:].strip(), status=TestStatus.PASSED)
            )
        elif text.startswith("FAIL "):
            self.test_suite.add_case(
                TestCase(name=text[5:].strip(), status=TestStatus.FAILED)
            )
