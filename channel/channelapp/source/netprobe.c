#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

#include "netprobe.h"
#include "netdiag.h"

static const u16 probe_ports[HBC_PROBE_PORTS] = { 80, 53, 443 };

static bool active, trusted, probe_out, ka_ok_once, err_logged, untrusted_logged;
static bool restart_pending;
static u32 gw, misses, ka_err_run, untrusted_misses;
static u32 ka_sent, ka_errors, probes_ok, probes_failed, restarts, relaunches;
static u8 port_index;
static netprobe_keepalive_mode ka_mode;
static bool ka_mode_logged;
static u64 next_ka, next_probe, last_poll, last_err_log, restart_at, probe_at;
static char source[40];

static void ip_text(char *out, size_t size, u32 a) {
	snprintf(out, size, "%u.%u.%u.%u", (unsigned) (a >> 24), (unsigned) ((a >> 16) & 0xff),
		(unsigned) ((a >> 8) & 0xff), (unsigned) (a & 0xff));
}

static unsigned ms(u64 ticks) {
	return (unsigned) ticks_to_millisecs(ticks);
}

void netprobe_up(u64 now, u32 ip, u32 gateway, const char *how) {
	char a[16], g[16];

	if (gateway != gw) {
		trusted = false;
		port_index = 0;
		untrusted_misses = 0;
		untrusted_logged = false;
	}
	gw = gateway;
	snprintf(source, sizeof(source), "%s", how ? how : "");
	active = true;
	probe_out = false;
	ka_ok_once = false;
	ka_err_run = 0;
	misses = 0;
	last_poll = 0;
	// Nothing to the gateway while the link settles: a probe 1.2 s after
	// the network came up got -ENETRESET, and so did the listener.
	next_ka = now + millisecs_to_ticks(HBC_NETPROBE_SETTLE_MS);
	next_probe = next_ka;
	ip_text(a, sizeof(a), ip);
	ip_text(g, sizeof(g), gw);
	netdiag_event("Keep-alive: up at %s, gateway %s (%s); UDP to port %u every %u s, "
		"probe every %u s, restart after %u misses%s", a, g, source,
		(unsigned) HBC_KEEPALIVE_PORT, (unsigned) (HBC_KEEPALIVE_PERIOD_MS / 1000),
		(unsigned) (HBC_PROBE_PERIOD_MS / 1000), (unsigned) HBC_PROBE_MISSES,
		trusted ? ", gateway known to answer" : "");
}

void netprobe_trust(u32 gateway) {
	gw = gateway;
	trusted = gateway != 0;
}

void netprobe_down(void) {
	active = false;
	probe_out = false;
	misses = 0;
	last_poll = 0;
}

static netprobe_action restart(u64 now, const char *why) {
	char g[16];

	ip_text(g, sizeof(g), gw);
	restarts++;
	netdiag_event("Probe: %s (gateway %s): automatic Restart network %u%s", why, g,
		(unsigned) restarts, restart_pending ? ", relaunch deadline kept" : "");
	if (!restart_pending) {
		restart_pending = true;
		restart_at = now;
	}
	misses = 0;
	ka_err_run = 0;
	active = false;
	probe_out = false;
	return NETPROBE_RESTART;
}

netprobe_action netprobe_poll(u64 now, bool up, bool busy) {
	if (restart_pending && ms(now - restart_at) >= HBC_PROBE_RESTART_MS) {
		char g[16];

		ip_text(g, sizeof(g), gw);
		restart_pending = false;
		relaunches++;
		netdiag_event("Probe: gateway %s not back %u ms after the automatic restart: "
			"fresh-IOS relaunch %u", g, ms(now - restart_at), (unsigned) relaunches);
		return NETPROBE_RELAUNCH;
	}
	if (!up || !active) {
		last_poll = 0;
		return NETPROBE_NONE;
	}
	if (busy) {
		// A transfer is running, so the network works.
		if (restart_pending)
			netdiag_event("Probe: transfer after the automatic restart, network back");
		restart_pending = false;
		misses = 0;
		last_poll = now;
		return NETPROBE_NONE;
	}
	if (last_poll && ms(now - last_poll) > HBC_NETPROBE_GAP_MS)
		misses = 0;
	last_poll = now;

	// Failing keep-alives count only when the probe can't vouch for the
	// network: a gateway answering the probe means it is fine.
	if (ka_ok_once && ka_err_run >= HBC_KEEPALIVE_ERRORS && (!trusted || misses))
		return restart(now, "keep-alive sends failing");
	if (trusted && misses >= HBC_PROBE_MISSES)
		return restart(now, "no answer to the probes");
	if (!probe_out && now >= next_probe) {
		probe_out = true;
		probe_at = now;
		next_probe = now + millisecs_to_ticks(HBC_PROBE_PERIOD_MS);
		return NETPROBE_PROBE;
	}
	if (now >= next_ka) {
		next_ka = now + millisecs_to_ticks(HBC_KEEPALIVE_PERIOD_MS);
		return NETPROBE_KEEPALIVE;
	}
	return NETPROBE_NONE;
}

const char *netprobe_keepalive_name(netprobe_keepalive_mode mode) {
	switch (mode) {
	case KEEPALIVE_BOUND_SENDTO: return "bound UDP sendto";
	case KEEPALIVE_CONNECTED_SEND: return "connected UDP send";
	case KEEPALIVE_DISCOVER_SENDTO: return "discovery socket sendto";
	default: return "unknown";
	}
}

netprobe_keepalive_mode netprobe_keepalive_how(void) {
	return ka_mode;
}

bool netprobe_keepalive_result(u64 now, s32 res) {
	if (res >= 0) {
		ka_sent++;
		if (!ka_mode_logged) {
			ka_mode_logged = true;
			netdiag_event("Keep-alive: sending (%s)", netprobe_keepalive_name(ka_mode));
		}
		if (ka_err_run)
			netdiag_event("Keep-alive: sending again after %u errors", (unsigned) ka_err_run);
		ka_ok_once = true;
		ka_err_run = 0;
		return false;
	}
	ka_errors++;
	ka_err_run++;
	if (res == -EINVAL && !ka_mode_logged && ka_mode + 1 < KEEPALIVE_MODES) {
		// IOS refuses this way of sending: try the next, at once.
		netdiag_event("Keep-alive: %s gave %d, trying %s", netprobe_keepalive_name(ka_mode),
			(int) res, netprobe_keepalive_name(ka_mode + 1));
		ka_mode++;
		next_ka = now;
		return true;
	}
	if (!err_logged || ms(now - last_err_log) >= HBC_NETPROBE_ERROR_LOG_MS) {
		err_logged = true;
		last_err_log = now;
		netdiag_event("Keep-alive: %s %d (%u in a row, %u in all)",
			netprobe_keepalive_name(ka_mode), (int) res, (unsigned) ka_err_run,
			(unsigned) ka_errors);
	}
	return false;
}

void netprobe_probe_result(u64 now, bool answered, s32 res) {
	char g[16];

	ip_text(g, sizeof(g), gw);
	probe_out = false;
	if (answered) {
		probes_ok++;
		if (!trusted)
			netdiag_event("Probe: gateway %s answers on port %u (%d) in %u ms, "
				"reachability check on", g, (unsigned) probe_ports[port_index], (int) res,
				ms(now - probe_at));
		else if (misses)
			netdiag_event("Probe: gateway %s answered again after %u misses", g,
				(unsigned) misses);
		trusted = true;
		misses = 0;
		if (restart_pending)
			netdiag_event("Probe: gateway %s back %u ms after the automatic restart", g,
				ms(now - restart_at));
		restart_pending = false;
		return;
	}
	probes_failed++;
	if (trusted) {
		misses++;
		netdiag_event("Probe: gateway %s port %u missed (%d after %u ms), %u/%u", g,
			(unsigned) probe_ports[port_index], (int) res, ms(now - probe_at),
			(unsigned) misses, (unsigned) HBC_PROBE_MISSES);
		return;
	}
	// Not yet known to answer: try the next port; never acted on.
	untrusted_misses++;
	if (untrusted_misses <= HBC_PROBE_PORTS)
		netdiag_event("Probe: gateway %s port %u no answer (%d), not yet known to answer",
			g, (unsigned) probe_ports[port_index], (int) res);
	if (untrusted_misses == 2 * HBC_PROBE_PORTS && !untrusted_logged) {
		untrusted_logged = true;
		netdiag_event("Probe: gateway %s has not answered on ports 80, 53 or 443: "
			"no reachability check, keep-alive only", g);
	}
	port_index = (port_index + 1) % HBC_PROBE_PORTS;
}

void netprobe_relaunch_declined(void) {
	restart_pending = false;
	netdiag_event("Probe: no fresh-IOS relaunch (limit reached or not the installed channel); "
		"restarts carry on");
}

u32 netprobe_gateway(void) { return gw; }
u16 netprobe_port(void) { return probe_ports[port_index]; }
bool netprobe_trusted(void) { return trusted; }
const char *netprobe_source(void) { return source; }
u32 netprobe_keepalive_sent(void) { return ka_sent; }
u32 netprobe_keepalive_errors(void) { return ka_errors; }
u32 netprobe_probes_ok(void) { return probes_ok; }
u32 netprobe_probes_failed(void) { return probes_failed; }
u32 netprobe_restarts(void) { return restarts; }
u32 netprobe_relaunches(void) { return relaunches; }
