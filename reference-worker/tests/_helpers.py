"""Offloadable helpers used by the test suite (resolved as entry points)."""

import os
import time


def sleep_and_pid(seconds):
    """Sleep, then return the worker process's pid (to show separate processes)."""
    time.sleep(seconds)
    return os.getpid()
