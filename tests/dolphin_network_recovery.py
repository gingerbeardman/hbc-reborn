#!/usr/bin/env python3
"""Check a WAD built with HBC_TEST_STUCK_NET_CLEANUP; never run on hardware.

Restart network deliberately wedges the TCP worker inside its cleanup path.
The menu must detect it, reset IOS, and relaunch the installed channel.
Usage: python3 tests/dolphin_network_recovery.py TEST-WAD
"""
import pathlib
import shutil
import subprocess
import sys
import time

import dolphin_smoke as smoke
from dolphin_hbc_reload import key, log


def main():
    image = pathlib.Path(sys.argv[1]).resolve()
    assert image.suffix == ".wad", "requires an installed channel"
    run = smoke.Dolphin(image, sd=True)
    passed = False
    try:
        smoke.wait_version(run, 60)
        deadline = time.monotonic() + 30
        while smoke.hbc.status(run.address)["device"] != "sd":
            assert time.monotonic() < deadline, "SD did not mount"
            time.sleep(1)
        time.sleep(2)
        before = log(run.address).count(f"HBC {smoke.expected} boot:")
        for buttons in ("h", "l", "a", "d", "d"):
            key(run.address, buttons)
        state = smoke.hbc.status(run.address)["overlay_ui"]
        assert state["menu"] == 4 and state["focus"] == 207, state
        smoke.hbc.send_keys(run.address, "a")
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                text = log(run.address)
                if text.count(f"HBC {smoke.expected} boot:") > before:
                    break
            except (OSError, smoke.hbc.HBCError):
                pass
            time.sleep(1)
        else:
            raise AssertionError("stuck cleanup did not recover by channel relaunch")
        assert "Restart: net_deinit begin" in text, text
        assert "Restart: net_deinit complete" not in text, text
        assert "Network cleanup stuck: reload countdown" in text, text
        assert "TCP worker parked for the IOS reload: 0" in text, text
        assert "Network recovery: IOS58 reloads before the channel" in text, text
        assert smoke.hbc.version(run.address) == smoke.expected
        time.sleep(6)
        assert log(run.address).count(f"HBC {smoke.expected} boot:") == before + 1
        assert not run.faults(), run.faults()[:10]
        print("PASS: wedged cleanup recovered through fresh IOS and channel relaunch; no loop", flush=True)
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
