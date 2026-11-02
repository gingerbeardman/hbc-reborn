#!/usr/bin/env python3
"""A failed network start is cleaned up, then retried without a reload.

Requires an HBC_TEST_INIT_FAILURE build (DOL or WAD), which fails the first
start that would have succeeded. Never run that build on hardware. With no
button pressed, HBC must clean up the failed start, come back on the network
through the TCP thread's own retry, and boot only once: a failed start whose
cleanup returns never leads to the stuck-cleanup reload countdown.
Usage: python3 tests/dolphin_network_retry.py TEST-IMAGE
"""
import pathlib
import shutil
import subprocess
import sys
import time

import dolphin_smoke as smoke
from dolphin_hbc_reload import log


def main():
    image = pathlib.Path(sys.argv[1]).resolve()
    run = smoke.Dolphin(image, sd=True)
    passed = False
    try:
        # The injected failure, cleanup and a 2 s retry all come first.
        smoke.wait_version(run, 90)
        deadline = time.monotonic() + 30
        while smoke.hbc.status(run.address)["device"] != "sd":
            assert time.monotonic() < deadline, "SD did not mount"
            time.sleep(1)
        time.sleep(3)
        text = log(run.address)
        assert "TEST: injected INIT timeout" in text, text
        assert "Failed INIT: net_deinit begin" in text, text
        assert "Failed INIT: net_deinit complete" in text, text
        assert "Network up at " in text, text
        assert "reload countdown" not in text, text
        assert text.count(f"HBC {smoke.expected} boot:") == 1, text
        assert smoke.hbc.status(run.address)["net_starts"] == 1
        assert not run.faults(), run.faults()[:10]
        print("PASS: failed start cleaned up and retried with no input and no reload", flush=True)
        passed = True
    finally:
        if run.dolphin.endswith(".app"):
            subprocess.run(["pkill", "-KILL", "-f", str(run.profile)], check=False)
        run.stop()
        if passed:
            shutil.rmtree(run.profile, ignore_errors=True)
        else:
            print(f"Retained profile: {run.profile}", flush=True)


if __name__ == "__main__":
    main()
