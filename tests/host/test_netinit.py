"""Run the actual network wait and fresh-IOS relaunch policy (netinit.c) on the PC."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC = shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class NetinitTest(unittest.TestCase):
    def test_deadline_cancellation_and_relaunches(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc").mkdir()
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\ntypedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;\n")
            (tmp / "network.h").write_text("#include <gctypes.h>\ns32 net_get_status(void);\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <gctypes.h>\nextern u64 now;\n"
                "static inline u64 gettime(void) { return now; }\n"
                "#define ticks_to_millisecs(t) (t)\n")
            harness = tmp / "check.c"
            harness.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "netinit.h"
u64 now;
static volatile bool stop, restart;
static int polls, sleeps, mode, notes;
static char last_note[192];
void netdiag_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_note, sizeof(last_note), fmt, ap);
    va_end(ap);
    notes++;
}
s32 net_get_status(void) {
    polls++;
    if (mode == 1 && now >= 12100) return 0;
    if (mode == 2) return -EIO;
    if (mode == 5 && now >= 13300) return 0;   // the slowest start seen
    return -EBUSY;
}
int usleep(useconds_t us) {
    now += us / 1000;
    sleeps++;
    if (mode == 3 && now >= 150) restart = true;
    if (mode == 4 && now >= 150) stop = true;
    return 0;
}
static void reset(int m) {
    now = 0; stop = restart = false;
    polls = sleeps = notes = 0; mode = m;
}
int main(void) {
    // A perpetually busy IOS cannot keep HBC polling forever.
    reset(0);
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 1, &stop, &restart) == -ETIMEDOUT);
    assert(now == HBC_NET_INIT_TIMEOUT_MS);
    assert(sleeps == 600 && polls == 600);
    assert(notes > 1 && strstr(last_note, "deadline reached"));
    // The observed 12.1-second successful connection remains allowed.
    reset(1);
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 1, &stop, &restart) == 0);
    assert(now == 12100);
    reset(2);
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 1, &stop, &restart) == -EIO);
    assert(!sleeps);
    // Restart/exit interrupt an already-busy wait at the next poll.
    reset(3);
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 1, &stop, &restart) == -ECANCELED);
    assert(now == 150 && restart && !stop);
    assert(strstr(last_note, "stop=0 restart=1"));
    reset(4);
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 1, &stop, &restart) == -ECANCELED);
    assert(now == 150 && stop && !restart);
    // Pending cancellation wins even before querying IOS.
    reset(0); restart = true;
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 1, &stop, &restart) == -ECANCELED);
    assert(!polls && !sleeps);
    // Retry attempts share the original deadline, rather than extending it.
    reset(0); now = 29950;
    assert(netinit_wait(0, HBC_NET_INIT_TIMEOUT_MS, 12, &stop, &restart) == -ETIMEDOUT);
    assert(now == 30000 && sleeps == 1);
    // Fast (the installed channel's first relaunches): a start IOS keeps
    // busy ends at the shorter budget, and the slowest start seen still fits.
    reset(0);
    assert(netinit_wait(0, HBC_NET_INIT_FAST_MS, 1, &stop, &restart) == -ETIMEDOUT);
    assert(now == HBC_NET_INIT_FAST_MS);
    reset(5);
    assert(netinit_wait(0, HBC_NET_INIT_FAST_MS, 1, &stop, &restart) == 0 && now == 13300);
    // Fast: libogc's timeout goes to the relaunch; EAGAIN still retries.
    assert(netinit_retry(-EAGAIN, true) && netinit_retry(-EAGAIN, false));
    assert(netinit_retry(-ETIMEDOUT, false) && !netinit_retry(-ETIMEDOUT, true));
    assert(!netinit_retry(-EIO, false) && !netinit_retry(0, false));
    // Relaunches in a row: a checked record, the first few fast.
    netinit_record rec = {0, 0, 0, 0};
    netinit_boot(&rec);   // an app's return, Reload HBC or a cold boot
    assert(netinit_relaunches() == 0 && netinit_fast());
    for (unsigned i = 1; i <= HBC_NET_FAST_RELAUNCHES; i++) {
        netinit_next(&rec);
        assert(rec.relaunches == i && rec.magic == HBC_NETINIT_MAGIC);
        netinit_boot(&rec);
        assert(netinit_relaunches() == i);
        assert(netinit_fast() == (i < HBC_NET_FAST_RELAUNCHES));
    }
    netinit_next(&rec);
    netinit_boot(&rec);
    assert(!netinit_fast());     // the backoff retries are the last resort
    // Once the network comes up, a later failure may relaunch again, while
    // HBCS still reports the relaunches that led to this boot.
    netinit_up();
    assert(netinit_fast() && netinit_relaunches() == HBC_NET_FAST_RELAUNCHES + 1);
    netinit_next(&rec);
    assert(rec.relaunches == 1);
    rec.relaunches = 2;          // an app's heap over the record: check fails
    netinit_boot(&rec);
    assert(netinit_relaunches() == 0 && netinit_fast());
    rec = (netinit_record) {HBC_NETINIT_MAGIC, 5000, HBC_NETINIT_MAGIC ^ ~5000u, 0};
    netinit_boot(&rec);          // implausible count
    assert(netinit_relaunches() == 0);
    // A gateway silent after an automatic restart (netprobe.h): at most
    // HBC_NET_FAST_RELAUNCHES relaunches in a row without an answer, though
    // the network comes up each time; the gateway rides along in the record.
    rec = (netinit_record) {0, 0, 0, 0};
    netinit_boot(&rec);
    assert(!netinit_known_gateway());
    for (unsigned i = 1; i <= HBC_NET_FAST_RELAUNCHES; i++) {
        netinit_up();
        assert(netinit_gateway_relaunch(0xc0a80101u));
        netinit_next(&rec);
        assert(rec.relaunches == i && rec.reserved == 0xc0a80101u);
        netinit_boot(&rec);
        assert(netinit_known_gateway() == 0xc0a80101u);
    }
    netinit_up();
    assert(!netinit_gateway_relaunch(0xc0a80101u));
    netinit_gateway_ok();        // it answered: a new run may start
    assert(netinit_gateway_relaunch(0xc0a80101u));
    netinit_next(&rec);
    assert(rec.relaunches == 1);
    // A failed start's relaunch carries no gateway.
    rec = (netinit_record) {0, 0, 0, 0};
    netinit_boot(&rec);
    netinit_next(&rec);
    assert(rec.reserved == 0);
    return 0;
}
''')
            exe = tmp / "check"
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netinit.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
