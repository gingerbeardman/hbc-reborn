"""Run the idle keep-alive and gateway probe scheduling (netprobe.c) on the PC."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC = shutil.which("cc")


@unittest.skipUnless(CC, "no host C compiler")
class NetprobeTest(unittest.TestCase):
    def test_keepalive_probe_restart_relaunch(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc").mkdir()
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\n"
                "typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;\n"
                "typedef uint16_t u16; typedef uint8_t u8;\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <gctypes.h>\n"
                "#define ticks_to_millisecs(t) (t)\n#define millisecs_to_ticks(t) (t)\n")
            harness = tmp / "check.c"
            harness.write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "netprobe.h"
static int notes;
static char last_note[256];
void netdiag_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_note, sizeof(last_note), fmt, ap);
    va_end(ap);
    notes++;
}
#define IP 0xc0a801e0u   /* 192.168.1.224 */
#define GW 0xc0a80101u   /* 192.168.1.1 */
static u64 now;
static int fresh = 1;
/* Keep-alive modes below this one get -22 (EINVAL) from IOS, as in 1.14.8. */
static int einval_modes;
static int mode_changes;
static int keepalives, probes, restarts, relaunches;
static u64 last_ka, last_probe, restart_time, relaunch_time;
/* The worker's idle loop: one poll every 16 ms for ms, the gateway answering
   (or not) and sendto working (or not). Stops at the first restart or
   relaunch when stop is set. */
static netprobe_action run(u32 ms, int answers, int send_ok, int up, int busy, int stop) {
    u64 end = now + ms;
    netprobe_action last = NETPROBE_NONE;
    while (now < end) {
        now += 16;
        netprobe_action a = netprobe_poll(now, up, busy);
        switch (a) {
        case NETPROBE_KEEPALIVE:
            /* Every HBC_KEEPALIVE_PERIOD_MS (late at most by a probe's
               timeout), the first at once after the network comes up. */
            if (!fresh)
                assert(now - last_ka >= HBC_KEEPALIVE_PERIOD_MS &&
                       now - last_ka < HBC_KEEPALIVE_PERIOD_MS + HBC_PROBE_TIMEOUT_MS + 100);
            fresh = 0;
            last_ka = now;
            keepalives++;
            {
                s32 res = !send_ok ? -118 :
                    (int) netprobe_keepalive_how() < einval_modes ? -22 : 1;
                if (netprobe_keepalive_result(now, res)) {
                    mode_changes++;
                    fresh = 1;   /* the next mode is tried at once */
                }
            }
            break;
        case NETPROBE_PROBE:
            last_probe = now;
            probes++;
            /* An answer in 2 ms, or a miss at the probe's timeout. */
            if (answers) {
                now += 2;
                netprobe_probe_result(now, 1, -111);
            } else {
                now += HBC_PROBE_TIMEOUT_MS;
                netprobe_probe_result(now, 0, -116);
            }
            break;
        case NETPROBE_RESTART:
            restarts++;
            restart_time = now;
            last = a;
            if (stop) return a;
            break;
        case NETPROBE_RELAUNCH:
            relaunches++;
            relaunch_time = now;
            last = a;
            if (stop) return a;
            break;
        default:
            break;
        }
    }
    return last;
}
static void up(u32 gw, const char *how) {
    netprobe_up(now, IP, gw, how);
    fresh = 1;
}
static void zero(void) { keepalives = probes = restarts = relaunches = 0; }
int main(void) {
    /* Nothing before the network is up. */
    assert(netprobe_poll(1000, 1, 0) == NETPROBE_NONE);
    assert(!netprobe_keepalive_sent() && !netprobe_restarts());

    /* Up: the start is logged with the gateway and where it came from.
       Nothing goes to the gateway while the link settles (1.16.1: a probe
       1.2 s after an app returned got -ENETRESET, and so did the
       listener); then a probe and a keep-alive go at once. */
    netprobe_up(now, IP, GW, "NCD static config");
    fresh = 1;
    run(HBC_NETPROBE_SETTLE_MS - 100, 1, 1, 1, 0, 0);
    assert(!probes && !keepalives);
    assert(strstr(last_note, "up at 192.168.1.224, gateway 192.168.1.1 (NCD static config)"));
    assert(!strcmp(netprobe_source(), "NCD static config") && netprobe_port() == 80);
    assert(!netprobe_trusted());
    assert(netprobe_keepalive_how() == KEEPALIVE_BOUND_SENDTO);
    run(100, 1, 1, 1, 0, 0);
    assert(probes == 1 && keepalives == 1 && netprobe_trusted());
    assert(strstr(last_note, "Keep-alive: sending (bound UDP sendto)"));
    assert(!mode_changes && netprobe_keepalive_sent() == 1);

    /* IOS refusing a way of sending with EINVAL (1.14.8 on the Wii, an
       unbound socket's sendto): the next way is tried at once, until one
       works, and that one stays. Shown on a second module state below; here
       the mode found first is kept even if IOS says EINVAL later. */
    einval_modes = 3;
    zero();
    run(HBC_KEEPALIVE_PERIOD_MS * 3 + 1000, 1, 1, 1, 0, 0);
    assert(!mode_changes && netprobe_keepalive_how() == KEEPALIVE_BOUND_SENDTO);
    /* Three EINVALs in a row, the probe answering: no restart. */
    assert(!restarts && netprobe_keepalive_errors() >= HBC_KEEPALIVE_ERRORS);
    einval_modes = 0;
    run(HBC_KEEPALIVE_PERIOD_MS, 1, 1, 1, 0, 0);
    assert(strstr(last_note, "sending again after"));

    /* A healthy idle hour: a keep-alive every 30 s, a probe every 15 s,
       nothing logged, no restart. */
    zero(); notes = 0;
    u32 hour_errors = netprobe_keepalive_errors(), hour_sent = netprobe_keepalive_sent();
    run(3600000, 1, 1, 1, 0, 0);
    assert(keepalives >= 119 && keepalives <= 121);
    assert(probes >= 239 && probes <= 241);
    assert(!restarts && !relaunches && notes == 0);
    assert(netprobe_keepalive_sent() - hour_sent >= 119 && netprobe_keepalive_errors() == hour_errors);

    /* The access point stops forwarding: each miss logged, Restart network
       after HBC_PROBE_MISSES misses in a row (~45 s). */
    zero(); notes = 0;
    u64 silent = now;
    assert(run(120000, 0, 1, 1, 0, 1) == NETPROBE_RESTART);
    assert(probes == HBC_PROBE_MISSES && notes == HBC_PROBE_MISSES + 1);
    assert(strstr(last_note, "automatic Restart network 1"));
    assert(now - silent >= 2 * HBC_PROBE_PERIOD_MS + HBC_PROBE_TIMEOUT_MS);
    assert(now - silent <= 3 * HBC_PROBE_PERIOD_MS + HBC_PROBE_TIMEOUT_MS);
    assert(netprobe_probes_failed() == HBC_PROBE_MISSES && netprobe_restarts() == 1);
    /* Nothing sent until the network is up again. */
    zero();
    assert(run(5000, 0, 1, 1, 0, 0) == NETPROBE_NONE && !probes && !keepalives);

    /* The restart brings the gateway back: no relaunch. */
    netprobe_down();
    up(GW, "NCD static config");
    assert(strstr(last_note, "gateway known to answer"));
    zero();
    run(200000, 1, 1, 1, 0, 0);
    assert(!relaunches && !restarts);

    /* The restart does not: the network comes up, the gateway stays silent,
       and HBC_PROBE_RESTART_MS after the restart the relaunch is asked for
       once (a second restart in between keeps the first deadline). */
    zero();
    assert(run(120000, 0, 1, 1, 0, 1) == NETPROBE_RESTART);
    u64 restarted = restart_time;
    netprobe_down();
    run(50, 0, 1, 0, 0, 0);
    up(GW, "NCD static config");
    assert(run(120000, 0, 1, 1, 0, 1) == NETPROBE_RESTART);
    netprobe_down();
    up(GW, "NCD static config");
    assert(run(120000, 0, 1, 1, 0, 1) == NETPROBE_RELAUNCH);
    assert(relaunch_time - restarted >= HBC_PROBE_RESTART_MS);
    /* Late by at most one missed probe: run() jumps the clock by the
       probe's timeout, where HBC polls the probe without blocking. */
    assert(relaunch_time - restarted < HBC_PROBE_RESTART_MS + HBC_PROBE_TIMEOUT_MS + 100);
    assert(netprobe_relaunches() == 1 && strstr(last_note, "fresh-IOS relaunch 1"));
    /* Declined (limit, or a DOL): the network restarts carry on, each with
       its own deadline (the caller declines again while over the limit). */
    netprobe_relaunch_declined();
    assert(strstr(last_note, "restarts carry on"));
    zero();
    for (int i = 0; i < 3; i++) {
        netprobe_down();
        up(GW, "NCD static config");
        netprobe_action a = run(120000, 0, 1, 1, 0, 1);
        if (a == NETPROBE_RELAUNCH)
            netprobe_relaunch_declined();
    }
    assert(restarts >= 2 && relaunches >= 1);
    netprobe_down();
    up(GW, "NCD static config");
    run(HBC_NETPROBE_SETTLE_MS + 1000, 1, 1, 1, 0, 0);    /* back: no deadline left */
    zero();
    run(200000, 1, 1, 1, 0, 0);
    assert(!relaunches && !restarts);

    /* A restart whose start keeps failing (network never up): the deadline
       still runs with up false. */
    up(GW, "NCD static config");
    run(HBC_NETPROBE_SETTLE_MS + 1000, 1, 1, 1, 0, 0);
    zero();
    assert(run(120000, 0, 1, 1, 0, 1) == NETPROBE_RESTART);
    restarted = restart_time;
    netprobe_down();
    assert(run(120000, 0, 1, 0, 0, 1) == NETPROBE_RELAUNCH);
    assert(relaunch_time - restarted >= HBC_PROBE_RESTART_MS &&
           relaunch_time - restarted < HBC_PROBE_RESTART_MS + 100);

    /* A transfer clears the misses: never a restart while busy. */
    up(GW, "NCD static config");
    run(HBC_NETPROBE_SETTLE_MS + 1000, 1, 1, 1, 0, 0);
    zero();
    run(2 * HBC_PROBE_PERIOD_MS, 0, 1, 1, 0, 0);
    assert(probes == 2 && !restarts);
    run(600000, 0, 1, 1, 1, 0);
    assert(probes == 2 && keepalives <= 1 && !restarts);
    fresh = 1;   /* the keep-alive held off by the transfer goes at once */
    run(2 * HBC_PROBE_PERIOD_MS - 1000, 0, 1, 1, 0, 0);
    assert(!restarts);
    run(HBC_PROBE_PERIOD_MS + HBC_PROBE_TIMEOUT_MS + 100, 0, 1, 1, 0, 1);
    assert(restarts == 1);

    /* A gap in the worker's loop (a Wiiload transfer) clears them too. */
    netprobe_down();
    up(GW, "NCD static config");
    run(HBC_NETPROBE_SETTLE_MS + 1000, 1, 1, 1, 0, 0);
    zero();
    run(2 * HBC_PROBE_PERIOD_MS, 0, 1, 1, 0, 0);
    assert(probes == 2);
    now += HBC_NETPROBE_GAP_MS + 1;
    fresh = 1;
    run(2 * HBC_PROBE_PERIOD_MS - 1000, 0, 1, 1, 0, 0);
    assert(!restarts);

    /* Keep-alive sends failing after they worked, the probe answering: only
       logged (at most once a minute), never a restart. */
    netprobe_down();
    up(GW, "NCD static config");
    run(HBC_NETPROBE_SETTLE_MS + 1000, 1, 1, 1, 0, 0);
    zero(); notes = 0;
    run(600000, 1, 0, 1, 0, 0);
    assert(!restarts && keepalives >= 19 && notes <= 11);
    /* With a gateway that never answers the probe, they restart the network
       after HBC_KEEPALIVE_ERRORS in a row. */
    netprobe_down();
    up(0xc0a80401u, "derived .1, DHCP");
    zero();
    run(HBC_NETPROBE_SETTLE_MS + 5000, 0, 1, 1, 0, 0);
    assert(keepalives == 1);
    zero(); notes = 0;
    u32 errors = netprobe_keepalive_errors();
    assert(run(200000, 0, 0, 1, 0, 1) == NETPROBE_RESTART);
    assert(netprobe_keepalive_errors() - errors == HBC_KEEPALIVE_ERRORS);
    assert(strstr(last_note, "keep-alive sends failing"));
    /* Failing from the start (never worked): no restart, throttled log. */
    netprobe_down();
    up(GW, "NCD static config");
    zero(); notes = 0;
    run(600000, 1, 0, 1, 0, 0);
    assert(!restarts && keepalives >= 19 && notes <= 11);

    /* A gateway that never answers (derived, no host there) is probed on
       80, 53, 443 in turn, but never acted on; noted once. */
    netprobe_down();
    up(0xc0a80201u, "derived .1, DHCP");
    assert(!netprobe_trusted());
    zero(); notes = 0;
    run(3600000, 0, 1, 1, 0, 0);
    assert(!restarts && !relaunches && probes >= 230);
    assert(notes == HBC_PROBE_PORTS + 1);
    assert(strstr(last_note, "no reachability check, keep-alive only"));
    /* It starts answering later on 53: trusted from then, on that port. */
    zero();
    run(HBC_PROBE_PERIOD_MS * 2, 1, 1, 1, 0, 0);
    assert(netprobe_trusted() && strstr(last_note, "answers on port"));

    /* A gateway known from before a relaunch counts from the start. */
    netprobe_down();
    netprobe_trust(0xc0a80301u);
    up(0xc0a80301u, "NCD static config");
    assert(netprobe_trusted());
    zero();
    assert(run(120000, 0, 1, 1, 0, 1) == NETPROBE_RESTART);
    return 0;
}
''')
            exe = tmp / "check"
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netprobe.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_keepalive_send_modes(self):
        """IOS answering -22 (EINVAL), as every keep-alive got in 1.14.8 on the
        Wii from an unbound socket: the next way of sending is tried at once
        and the first that works stays; the probe answering means no restart."""
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc").mkdir()
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\n"
                "typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;\n"
                "typedef uint16_t u16; typedef uint8_t u8;\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <gctypes.h>\n"
                "#define ticks_to_millisecs(t) (t)\n#define millisecs_to_ticks(t) (t)\n")
            harness = tmp / "modes.c"
            harness.write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "netprobe.h"
static char last_note[256], all_notes[4096];
void netdiag_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_note, sizeof(last_note), fmt, ap);
    va_end(ap);
    strncat(all_notes, last_note, sizeof(all_notes) - strlen(all_notes) - 2);
    strcat(all_notes, "\n");
}
static u64 now;
/* Polls for ms; keep-alives get -22 in modes below einval, else res. */
static int run(u32 ms, int einval, s32 res, int *sends, int *changes) {
    u64 end = now + ms;
    int restarts = 0;
    while (now < end) {
        now += 16;
        switch (netprobe_poll(now, 1, 0)) {
        case NETPROBE_PROBE:
            netprobe_probe_result(now + 2, 1, -127);   /* as the Wii: answered */
            now += 2;
            break;
        case NETPROBE_KEEPALIVE:
            (*sends)++;
            if (netprobe_keepalive_result(now,
                    (int) netprobe_keepalive_how() < einval ? -22 : res))
                (*changes)++;
            break;
        case NETPROBE_RESTART:
            restarts++;
            break;
        default:
            break;
        }
    }
    return restarts;
}
int main(void) {
    int sends = 0, changes = 0;
    netprobe_up(now, 0xc0a801e0u, 0xc0a80101u, "NCD static config");
    now += HBC_NETPROBE_SETTLE_MS;
    assert(netprobe_keepalive_how() == KEEPALIVE_BOUND_SENDTO);
    /* Bound sendto and connected send refused, the discovery socket works:
       found within a few polls, not a period apart. */
    assert(run(1000, 2, 1, &sends, &changes) == 0);
    assert(changes == 2 && sends == 3);
    assert(netprobe_keepalive_how() == KEEPALIVE_DISCOVER_SENDTO);
    assert(strstr(all_notes, "bound UDP sendto gave -22, trying connected UDP send"));
    assert(strstr(all_notes, "connected UDP send gave -22, trying discovery socket sendto"));
    assert(strstr(all_notes, "Keep-alive: sending (discovery socket sendto)"));
    assert(netprobe_keepalive_sent() == 1 && netprobe_keepalive_errors() == 2);
    /* Kept from then on, across a restart, even if IOS says EINVAL later. */
    netprobe_down();
    netprobe_up(now, 0xc0a801e0u, 0xc0a80101u, "NCD static config");
    now += HBC_NETPROBE_SETTLE_MS;
    changes = 0;
    assert(run(200000, 3, 1, &sends, &changes) == 0);
    assert(!changes && netprobe_keepalive_how() == KEEPALIVE_DISCOVER_SENDTO);
    return 0;
}
''')
            exe = tmp / "modes"
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netprobe.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_every_mode_refused(self):
        """1.14.8's case if no way works: every send EINVAL, the probe
        answering; errors counted and logged, never a restart."""
        with tempfile.TemporaryDirectory() as directory:
            tmp = pathlib.Path(directory)
            (tmp / "ogc").mkdir()
            (tmp / "gctypes.h").write_text(
                "#pragma once\n#include <stdint.h>\n"
                "typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32;\n"
                "typedef uint16_t u16; typedef uint8_t u8;\n")
            (tmp / "ogc/lwp_watchdog.h").write_text(
                "#include <gctypes.h>\n"
                "#define ticks_to_millisecs(t) (t)\n#define millisecs_to_ticks(t) (t)\n")
            harness = tmp / "refused.c"
            harness.write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include "netprobe.h"
static int notes;
void netdiag_event(const char *fmt, ...) { (void) fmt; notes++; }
int main(void) {
    u64 now = 0;
    int restarts = 0;
    netprobe_up(now, 0xc0a801e0u, 0xc0a80101u, "NCD static config");
    while (now < 3600000) {
        now += 16;
        switch (netprobe_poll(now, 1, 0)) {
        case NETPROBE_PROBE: netprobe_probe_result(now, 1, -127); break;
        case NETPROBE_KEEPALIVE: netprobe_keepalive_result(now, -22); break;
        case NETPROBE_RESTART: restarts++; break;
        default: break;
        }
    }
    assert(!restarts && !netprobe_keepalive_sent());
    assert(netprobe_keepalive_errors() >= 120);
    assert(netprobe_keepalive_how() == KEEPALIVE_DISCOVER_SENDTO);
    assert(notes < 80);   /* at most one error line a minute */
    return 0;
}
''')
            exe = tmp / "refused"
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-I", str(tmp), "-I", str(ROOT / "channel/channelapp/source"),
                            str(harness), str(ROOT / "channel/channelapp/source/netprobe.c"),
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
