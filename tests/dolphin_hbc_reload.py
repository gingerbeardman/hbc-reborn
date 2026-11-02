#!/usr/bin/env python3
"""Verify SD network diagnostics and HOME > HBC > Reload HBC in an isolated NAND."""
import pathlib
import shutil
import sys
import subprocess
import time

import dolphin_smoke as smoke

hbc = smoke.hbc
ROOT = pathlib.Path(__file__).resolve().parents[1]


def key(wii, buttons):
    hbc.send_keys(wii, buttons)
    time.sleep(0.7)


def log(wii):
    return hbc.get_file(wii, "sd:/hbc-network.log").decode()


def main():
    # Run one image per invocation: abruptly stopping Dolphin can leave the
    # host TCP port in TIME_WAIT, unlike a real IOS title launch.
    images = sys.argv[1:] or [ROOT / "channel/title/channel_retail.wad"]
    for image in map(lambda p: pathlib.Path(p).resolve(), images):
        run = smoke.Dolphin(image, sd=True)
        passed = False
        try:
            smoke.wait_version(run, 60)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    if hbc.status(run.address)["device"] == "sd":
                        break
                except (OSError, hbc.HBCError):
                    pass
                time.sleep(1)
            else:
                raise AssertionError("SD did not mount")
            time.sleep(2)
            text = log(run.address)
            assert f"HBC {smoke.expected} boot:" in text, text
            assert "Network up at " in text and "tcp_listen " in text, text
            print(f"PASS: {image.suffix} boot and SD diagnostics", flush=True)
            starts = hbc.status(run.address)["net_starts"]
            for buttons in ("h", "l", "a", "d", "d"):
                key(run.address, buttons)
            state = hbc.status(run.address)["overlay_ui"]
            assert state["menu"] == 4 and state["focus"] == 207, state
            hbc.send_keys(run.address, "a")
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                try:
                    if hbc.status(run.address)["net_starts"] > starts:
                        break
                except (OSError, hbc.HBCError):
                    pass
                time.sleep(1)
            else:
                raise AssertionError("Restart network did not restore access")
            time.sleep(2)
            text = log(run.address)
            assert "Restart network applied by the worker" in text, text
            assert "Restart: net_deinit complete" in text, text
            print("PASS: Restart network restored remote access and logged cleanup", flush=True)
            if image.suffix == ".wad":
                before = text.count(f"HBC {smoke.expected} boot:")
                for buttons in ("h", "l", "a", "d", "d", "r"):
                    key(run.address, buttons)
                state = hbc.status(run.address)["overlay_ui"]
                assert state["menu"] == 4 and state["focus"] == 208, state
                # Only press A after verifying Reload HBC's exact focus id.
                hbc.send_keys(run.address, "a")
                time.sleep(3)
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    try:
                        text = log(run.address)
                        if text.count(f"HBC {smoke.expected} boot:") > before:
                            break
                    except (OSError, hbc.HBCError):
                        pass
                    time.sleep(1)
                else:
                    raise AssertionError("no second boot recorded after Reload HBC")
                assert "Reload HBC requested" in text, text
                assert hbc.version(run.address) == smoke.expected
                print("PASS: Reload HBC relaunched installed channel; SD log survived", flush=True)
            assert not run.faults(), run.faults()[:10]
            passed = True
        finally:
            # HBC may handle SIGTERM as a Wii power event and leave the
            # emulator alive. Kill only this test's unique profile process.
            if run.dolphin.endswith(".app"):
                subprocess.run(["pkill", "-KILL", "-f", str(run.profile)], check=False)
            run.stop()
            if passed:
                shutil.rmtree(run.profile, ignore_errors=True)
            else:
                print(f"Retained profile: {run.profile}", flush=True)


if __name__ == "__main__":
    main()
