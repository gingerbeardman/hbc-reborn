"""Run the idle network check's detection and persistence logic (netwatch.c) on the PC."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC = shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class NetwatchTest(unittest.TestCase):
    def test_detection_and_persistence(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc").mkdir()
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\ntypedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <gctypes.h>\n"
                "#define ticks_to_millisecs(t) (t)\n#define millisecs_to_ticks(t) (t)\n")
            harness = tmp / "check.c"
            harness.write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "netwatch.h"
static int notes;
static char last_note[192];
void netdiag_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_note, sizeof(last_note), fmt, ap);
    va_end(ap);
    notes++;
}
#define IP 0xc0a808d5u   /* 192.168.8.213 */
static u64 now;
/* One check every period while the state stays the same; the verdict of the last. */
static netwatch_verdict run(u32 ms, s32 link, u32 ip, int busy) {
    netwatch_verdict v = NETWATCH_OK;
    u64 end = now + ms;
    while (now < end) {
        now += HBC_NETWATCH_PERIOD_MS;
        assert(netwatch_due(now));
        v = netwatch_sample(now, link, ip, busy);
        assert(!netwatch_due(now));
        if (v == NETWATCH_LOST)
            break;
    }
    return v;
}
int main(void) {
    /* Nothing is checked before the network is up. */
    assert(!netwatch_due(1000000));
    assert(netwatch_link() == 0 && !netwatch_lost_count() && !strcmp(netwatch_last_reason(), ""));

    netwatch_up(now, IP, NCD_LINK_WIFI_UP);
    assert(strstr(last_note, "up at 192.168.8.213, link 5 (wireless up)"));
    assert(!netwatch_due(now + HBC_NETWATCH_PERIOD_MS - 1) && netwatch_due(now + HBC_NETWATCH_PERIOD_MS));
    /* A healthy idle network: an hour of checks, nothing logged. */
    notes = 0;
    assert(run(3600000, NCD_LINK_WIFI_UP, IP, 0) == NETWATCH_OK && notes == 0);

    /* A blip shorter than the persistence: logged, counted, no restart. */
    assert(run(8000, NCD_LINK_WIFI_DOWN, IP, 0) == NETWATCH_SUSPECT);
    assert(run(4000, NCD_LINK_WIFI_UP, IP, 0) == NETWATCH_OK);
    assert(strstr(last_note, "back by itself") && netwatch_blips() == 1 && !netwatch_lost_count());

    /* The Wi-Fi drops for good: lost once the bad state has lasted the persistence. */
    u64 drop = now;
    assert(run(HBC_NETWATCH_PERSIST_MS + 8000, NCD_LINK_WIFI_DOWN, IP, 0) == NETWATCH_LOST);
    assert(now - drop >= HBC_NETWATCH_PERSIST_MS + HBC_NETWATCH_PERIOD_MS);
    assert(now - drop <= HBC_NETWATCH_PERSIST_MS + 2 * HBC_NETWATCH_PERIOD_MS);
    assert(netwatch_lost_count() == 1 && !strcmp(netwatch_last_reason(), "link wireless down"));
    assert(netwatch_link() == NCD_LINK_WIFI_DOWN);
    /* Reported once: no checks until the network is up again. */
    assert(!netwatch_due(now + 100000));
    assert(netwatch_sample(now, NCD_LINK_WIFI_DOWN, IP, 0) == NETWATCH_OK);

    /* The address going is enough, whatever the link says. */
    netwatch_up(now, IP, NCD_LINK_WIRED);
    assert(run(HBC_NETWATCH_PERSIST_MS + 8000, NCD_LINK_WIRED, 0, 0) == NETWATCH_LOST);
    assert(!strcmp(netwatch_last_reason(), "no address") && netwatch_lost_count() == 2);
    netwatch_up(now, IP, NCD_LINK_WIFI_UP);
    assert(run(HBC_NETWATCH_PERSIST_MS + 8000, NCD_LINK_WIFI_UP, IP + 1, 0) == NETWATCH_LOST);
    assert(!strcmp(netwatch_last_reason(), "address changed to 192.168.8.214"));

    /* A transfer holds the wait off; the full persistence is needed after it. */
    netwatch_up(now, IP, NCD_LINK_WIFI_UP);
    assert(run(12000, NCD_LINK_NONE, IP, 0) == NETWATCH_SUSPECT);
    notes = 0;
    assert(run(60000, NCD_LINK_NONE, IP, 1) == NETWATCH_OK);
    assert(notes == 1 && strstr(last_note, "transfer running"));
    assert(run(12000, NCD_LINK_NONE, IP, 0) == NETWATCH_SUSPECT);
    assert(run(8000, NCD_LINK_NONE, IP, 0) == NETWATCH_LOST);
    assert(!strcmp(netwatch_last_reason(), "link none"));

    /* An IOS whose link status does not read up on a working network is not
       believed (Dolphin answers wired, so it never trips there); busy and
       errors never count. */
    netwatch_up(now, IP, NCD_LINK_NONE);
    assert(strstr(last_note, "link status not used"));
    assert(run(120000, NCD_LINK_NONE, IP, 0) == NETWATCH_OK);
    netwatch_up(now, IP, -6);
    assert(run(120000, NCD_LINK_WIFI_DOWN, IP, 0) == NETWATCH_OK);
    netwatch_up(now, IP, NCD_LINK_WIFI_UP);
    assert(run(120000, NCD_LINK_BUSY, IP, 0) == NETWATCH_OK);
    assert(run(120000, -6, IP, 0) == NETWATCH_OK);
    assert(strstr(last_note, "-> -6 (error)"));

    /* A network taken down another way stops the checks. */
    netwatch_up(now, IP, NCD_LINK_WIFI_UP);
    netwatch_down();
    assert(!netwatch_due(now + 1000000));
    assert(netwatch_lost_count() == 4);
    return 0;
}
''')
            exe = tmp / "check"
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netwatch.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
